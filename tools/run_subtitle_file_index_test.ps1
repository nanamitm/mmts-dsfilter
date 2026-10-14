param([ValidateSet('subtitle_file_index_test', 'subtitle_render_test', 'playback_state_test')][string]$Test = 'subtitle_file_index_test')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$toolset = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory |
    Where-Object { [version]$_.Name -lt [version]'14.50' } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $toolset) { throw 'Install the v143 toolset used by the Release build' }
$toolsetVersion = $toolset.Name
[xml]$project = Get-Content (Join-Path $repo 'msvc\mmts-dsfilter.vcxproj')
$objDir = Join-Path $repo 'msvc\mmts-dsfilter\x64\Release'
$objects = @($project.Project.ItemGroup.ClCompile | Where-Object {
    ($_.Include -and $Test -in @('subtitle_render_test', 'playback_state_test') -and $_.Include -notmatch 'MmtTlvSplitter\.cpp$') -or
    $_.Include -like '*dantto4k*' -or $_.Include -match '(DsHandler|TtmlAdapter|SubtitleFileIndex|SubtitleDebugLogShim)\.cpp$'
} | ForEach-Object {
    $path = Join-Path $objDir ([IO.Path]::GetFileNameWithoutExtension($_.Include) + '.obj')
    if (-not (Test-Path -LiteralPath $path)) { throw "Build Release before running this test: $path" }
    '"' + $path + '"'
})
New-Item -ItemType Directory -Force (Join-Path $PSScriptRoot 'obj') | Out-Null
$fixtureDir = Join-Path $repo 'work\playback-state'
if ($Test -eq 'playback_state_test') { New-Item -ItemType Directory -Force $fixtureDir | Out-Null }
$testArguments = if ($Test -eq 'playback_state_test') { '"' + $fixtureDir + '"' } else { '' }
$rsp = Join-Path $PSScriptRoot ('obj\' + $Test + '_objects.rsp')
$objects | Set-Content -LiteralPath $rsp -Encoding ascii
$batch = Join-Path $PSScriptRoot ('obj\run_' + $Test + '.cmd')
@"
@echo off
set "PATH=$(Split-Path $vswhere);%PATH%"
call "$vcvars" -vcvars_ver=$toolsetVersion >nul
if errorlevel 1 exit /b 1
cd /d "$PSScriptRoot"
cl /nologo /c /EHsc /std:c++20 /utf-8 /MD /permissive /wd4596 /DUNICODE /D_UNICODE /DNODDRAW /I"..\src" /I"..\..\dantto4k\src" /I"..\baseclasses\Samples\Win7Samples\multimedia\directshow\baseclasses" /I"..\thirdparty\ffmpeg\include" $Test.cpp /Fo:obj\
if errorlevel 1 exit /b 1
link /nologo obj\$Test.obj @"$rsp" /OUT:$Test.exe /LTCG /LIBPATH:"..\thirdparty\ffmpeg\lib\MD" strmiids.lib winmm.lib ole32.lib oleaut32.lib uuid.lib Winscard.lib ws2_32.lib bcrypt.lib user32.lib gdi32.lib advapi32.lib avcodec.lib avutil.lib swresample.lib
if errorlevel 1 exit /b 1
"$PSScriptRoot\$Test.exe" $testArguments
"@ | Set-Content -LiteralPath $batch -Encoding ascii
& $batch
if ($LASTEXITCODE -ne 0) { throw "$Test failed" }
