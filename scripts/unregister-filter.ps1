param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [string]$FilterPath
)

$ErrorActionPreference = 'Stop'

function Test-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-PowerShellExe {
    $pwsh = Get-Command pwsh.exe -ErrorAction SilentlyContinue
    if ($pwsh) {
        return $pwsh.Source
    }
    return "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
}

function Resolve-DefaultFilterPath {
    $bundledPath = Join-Path $PSScriptRoot "mmts-dsfilter.ax"
    if (Test-Path -LiteralPath $bundledPath) {
        return $bundledPath
    }

    $repoRoot = Split-Path -Parent $PSScriptRoot
    return Join-Path $repoRoot "msvc\x64\$Configuration\mmts-dsfilter.ax"
}

if (-not $FilterPath) {
    $FilterPath = Resolve-DefaultFilterPath
}

$FilterPath = [IO.Path]::GetFullPath($FilterPath)

if (-not (Test-Admin)) {
    $ps = Get-PowerShellExe
    $args = @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', $PSCommandPath,
        '-Configuration', $Configuration,
        '-FilterPath', $FilterPath
    )
    $process = Start-Process -FilePath $ps -ArgumentList $args -Verb RunAs -Wait -PassThru
    exit $process.ExitCode
}

if (-not (Test-Path -LiteralPath $FilterPath)) {
    throw "Filter not found: $FilterPath"
}

Write-Host "Unregistering $FilterPath"
& "$env:SystemRoot\System32\regsvr32.exe" /s /u $FilterPath
if ($LASTEXITCODE -ne 0) {
    throw "regsvr32 /u failed with exit code $LASTEXITCODE"
}

Write-Host "Unregistered MMT/TLV DirectShow filter."
