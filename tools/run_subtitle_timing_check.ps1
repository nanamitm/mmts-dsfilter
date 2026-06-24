<#
.SYNOPSIS
    One-shot subtitle-timing verification: mmts-dsfilter's own schedule vs.
    dantto4k-converted TS decoded the way arib-splitter would show it.

.DESCRIPTION
    Runs the full pipeline against a single .mmts source file:
      1) mmts_compare_dump.exe   - single pass over the source file; writes
                                   out.ts (dantto4k-equivalent) and A.csv
                                   (mmts-dsfilter's actual subtitle schedule).
      2) test_caption_timeline_probe.exe - decodes out.ts the way
                                   arib-splitter/libaribcaption would, into
                                   B.csv.
      3) compare_subtitle_timing.py - diffs A.csv vs B.csv and reports
                                   timing anomalies / dropped cues.

    Exits with the comparison script's exit code (0 = PASS, 1 = FAIL or
    error), so this can be dropped into a regression-check workflow.

.PARAMETER InputMmts
    Path to the source .mmts file to verify.

.PARAMETER OutDir
    Where to write out.ts / A.csv / B.csv / report.csv. Defaults to a
    "subtitle_timing_check" folder next to the input file.

.PARAMETER ToleranceMs
    Passed through to compare_subtitle_timing.py --tolerance-ms (default 300).

.EXAMPLE
    .\run_subtitle_timing_check.ps1 -InputMmts D:\rec\sample.mmts
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InputMmts,

    [string]$OutDir,

    [int]$ToleranceMs = 300,

    [string]$ToolAExe,

    [string]$ToolBExe,

    [string]$Python = "py"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $InputMmts -PathType Leaf)) {
    Write-Error "Input file not found: $InputMmts"
    exit 1
}
$InputMmts = (Resolve-Path -LiteralPath $InputMmts).Path

if (-not $OutDir) {
    $OutDir = Join-Path (Split-Path -Parent $InputMmts) "subtitle_timing_check"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path

if (-not $ToolAExe) {
    $ToolAExe = Join-Path $PSScriptRoot "msvc\x64\Release\mmts_compare_dump.exe"
}
if (-not $ToolBExe) {
    $ToolBExe = Join-Path $PSScriptRoot "..\..\arib-splitter\libaribcaption\build\x64\Release\Release\test_caption_timeline_probe.exe"
}
$CompareScript = Join-Path $PSScriptRoot "compare_subtitle_timing.py"

foreach ($path in @($ToolAExe, $ToolBExe, $CompareScript)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        Write-Error "Required tool not found: $path"
        exit 1
    }
}

# Use a fixed ASCII-only base name for all artifacts, regardless of the
# input filename. ffmpeg's avformat_open_input (used by
# test_caption_timeline_probe.exe) has been observed to fail to open paths
# containing non-ASCII characters or square brackets on Windows -- both of
# which are routine in real Japanese broadcast recording filenames -- so the
# intermediate .ts must not inherit the source name.
$base = "output"
$tsPath = Join-Path $OutDir "$base.ts"
$aCsv = Join-Path $OutDir "$base.A.csv"
$bCsv = Join-Path $OutDir "$base.B.csv"
$reportCsv = Join-Path $OutDir "$base.report.csv"

Write-Host "== [1/3] mmts_compare_dump ==" -ForegroundColor Cyan
Write-Host "  $InputMmts -> $tsPath, $aCsv"
& $ToolAExe $InputMmts $tsPath $aCsv
if ($LASTEXITCODE -ne 0) {
    Write-Error "mmts_compare_dump failed (exit $LASTEXITCODE)"
    exit 1
}

Write-Host "== [2/3] test_caption_timeline_probe ==" -ForegroundColor Cyan
Write-Host "  $tsPath -> $bCsv"
& $ToolBExe $tsPath $bCsv
if ($LASTEXITCODE -ne 0) {
    Write-Error "test_caption_timeline_probe failed (exit $LASTEXITCODE)"
    exit 1
}

Write-Host "== [3/3] compare_subtitle_timing ==" -ForegroundColor Cyan
& $Python $CompareScript $aCsv $bCsv --tolerance-ms $ToleranceMs --report $reportCsv
exit $LASTEXITCODE
