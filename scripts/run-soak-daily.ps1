<#
.SYNOPSIS
    One daily soak-evidence run, wrapped for the Windows Scheduled Task.

.DESCRIPTION
    Runs `tools/run_soak.py` for the Windows platform and the two
    acceptance cases the v1.1 release gate requires (`d` and `e`), and
    tees the collector's output into `build/soak/soak-task.log`.

    The collector writes one machine-readable record per case into
    `build/soak/windows-<day>.jsonl`; this wrapper only adds the
    human-readable log and a stable exit code for the scheduler:

        0  every required case passed
        1  at least one case failed
        2  the collector could not run at all

    The log is truncated on every run so that it always describes the
    most recent day rather than growing without bound.

.PARAMETER RepoPath
    The repository to measure. Defaults to the repository this script
    lives in.

.PARAMETER Python
    The interpreter that runs the collector. Defaults to the first
    candidate that can both execute code and import `playwright` --
    Case D drives a real Chrome peer through Playwright, so an
    interpreter without it cannot produce evidence.

.PARAMETER Cases
    Comma-separated acceptance cases. Defaults to `d,e`.
#>

[CmdletBinding()]
param(
    [string] $RepoPath = '',
    [string] $Python = '',
    [string] $Cases = 'd,e'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = if ($RepoPath) { $RepoPath } else { Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path) }
$collector = Join-Path $repo 'tools\run_soak.py'
if (-not (Test-Path -LiteralPath $collector)) {
    Write-Error "collector not found: $collector"
    exit 2
}

function Resolve-Python {
    param([string] $Preferred)

    $candidates = @()
    if ($Preferred) { $candidates += $Preferred }

    if (-not $candidates) {
        foreach ($name in @('python', 'python3')) {
            $cmd = Get-Command $name -ErrorAction SilentlyContinue
            if ($cmd) { $candidates += $cmd.Source }
        }
        # The Miniconda install that carries the Playwright wheels is a
        # common second home; probing it costs nothing and saves a day
        # of "the package is not importable" inconclusive records.
        $candidates += 'D:\ProgramData\Miniconda3\python.exe'
    }

    foreach ($candidate in $candidates) {
        if (-not $candidate) { continue }
        if (-not (Test-Path -LiteralPath $candidate)) { continue }
        & $candidate -c "import sys, playwright" 2>$null
        if ($LASTEXITCODE -eq 0) { return $candidate }
    }
    return $null
}

$interpreter = Resolve-Python -Preferred $Python
if (-not $interpreter) {
    Write-Error "no usable Python interpreter was found (need one that can import playwright)"
    exit 2
}

$logDir = Join-Path $repo 'build\soak'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$logFile = Join-Path $logDir 'soak-task.log'

$head = @(
    "=== NimRTC daily soak run ===",
    "started_at : {0}" -f (Get-Date).ToString('o'),
    "repo       : {0}" -f $repo,
    "python     : {0}" -f $interpreter,
    "branch     : {0}" -f (& git -C $repo rev-parse --abbrev-ref HEAD 2>$null),
    "commit     : {0}" -f (& git -C $repo rev-parse HEAD 2>$null)
) -join [Environment]::NewLine

Set-Content -LiteralPath $logFile -Value $head -Encoding utf8

Push-Location $repo
try {
    & $interpreter -u $collector --platform windows --case $Cases 2>&1 |
        Tee-Object -FilePath $logFile -Append
    $code = $LASTEXITCODE
}
finally {
    Pop-Location
}

Add-Content -LiteralPath $logFile -Value ("finished_at : {0} exit={1}" -f (Get-Date).ToString('o'), $code) -Encoding utf8
exit $code
