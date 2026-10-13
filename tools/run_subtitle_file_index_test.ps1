$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
[xml]$project = Get-Content (Join-Path $repo 'msvc\mmts-dsfilter.vcxproj')
$objDir = Join-Path $repo 'msvc\mmts-dsfilter\x64\Release'
$objects = @($project.Project.ItemGroup.ClCompile | Where-Object {
    $_.Include -like '*dantto4k*' -or $_.Include -match '(DsHandler|TtmlAdapter|SubtitleFileIndex|SubtitleDebugLogShim)\.cpp$'
} | ForEach-Object {
    $path = Join-Path $objDir ([IO.Path]::GetFileNameWithoutExtension($_.Include) + '.obj')
    if (-not (Test-Path -LiteralPath $path)) { throw "Build Release before running this test: $path" }
    '"' + $path + '"'
})
$rsp = Join-Path $PSScriptRoot 'obj\subtitle_index_objects.rsp'
$objects | Set-Content -LiteralPath $rsp -Encoding ascii
$batch = Join-Path $PSScriptRoot 'obj\run_subtitle_index.cmd'
@"
@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "$PSScriptRoot"
cl /nologo /c /EHsc /std:c++20 /utf-8 /MD /I"..\src" /I"..\..\dantto4k\src" subtitle_file_index_test.cpp /Fo:obj\
if errorlevel 1 exit /b 1
link /nologo obj\subtitle_file_index_test.obj @"$rsp" /OUT:subtitle_file_index_test.exe /LTCG Winscard.lib ws2_32.lib bcrypt.lib user32.lib gdi32.lib
if errorlevel 1 exit /b 1
subtitle_file_index_test.exe
"@ | Set-Content -LiteralPath $batch -Encoding ascii
& $batch
if ($LASTEXITCODE -ne 0) { throw 'Subtitle file index tests failed' }
