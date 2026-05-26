param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [ValidateSet("x64")]
    [string]$Platform = "x64",

    [string]$PlatformToolset = "v143"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$project = Join-Path $repoRoot "msvc\mmts-dsfilter.vcxproj"
$msbuild = "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\Msbuild\Current\Bin\amd64\MSBuild.exe"

if (-not (Test-Path -LiteralPath $msbuild)) {
    throw "MSBuild not found: $msbuild"
}

& $msbuild $project `
    /p:Configuration=$Configuration `
    /p:Platform=$Platform `
    /p:PlatformToolset=$PlatformToolset

if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$output = Join-Path $repoRoot "msvc\$Platform\$Configuration\mmts-dsfilter.ax"
Write-Host "Built: $output"
