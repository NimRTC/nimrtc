<#
.SYNOPSIS
    Long-running watcher for the v1.1.0 release tag.

.DESCRIPTION
    Wraps `scripts/watch-v11-tag.ps1` (the one-shot probe) in a poll loop
    and records every result under `build/v11-watch/`, so that the state
    survives a closed editor or a terminated session.

    The probe's exit code is the signal:
        0  the tag exists and its signature verifies
        1  the tag is not on origin yet
        2  the tag exists but the signature does not verify
        3  the probe itself failed (network, git, python)

    Exit 0 and exit 2 are both terminal states: they are written to
    `build/v11-watch/READY` and `build/v11-watch/BAD_SIGNATURE` and the
    loop stops, because neither state changes on its own.

    This script is meant to be launched by the Windows Scheduled Task
    "NimRTC v1.1 tag watch"; it is not a foreground command.
#>

[CmdletBinding()]
param(
    [string] $RepoPath = '',
    [string] $Tag = 'v1.1.0',
    [string] $Principal = 'realdesktop@163.com',
    # The repository to probe. Defaults to the repository this script lives in.
    [string] $ProbeRepoPath = '',
    # Where the state is written. Defaults to <probe repo>/build/v11-watch.
    [string] $StateDir = '',
    # Seconds between probes.
    [int] $IntervalSeconds = 1800,
    # Stop after this many probes; 0 means run until a terminal state.
    [int] $MaxProbes = 0
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# The script's own directory is <repo>/scripts, so its parent is the repo
# that holds the probe script -- independent of which repository is probed.
$scriptRepo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

if (-not $ProbeRepoPath) {
    $ProbeRepoPath = $scriptRepo
}

$probeScript = Join-Path $scriptRepo 'scripts\watch-v11-tag.ps1'
if (-not (Test-Path -LiteralPath $probeScript)) {
    Write-Error "probe script not found: $probeScript"
    exit 3
}

# State lives under build/ so the project root stays clean.
if (-not $StateDir) {
    $StateDir = Join-Path $ProbeRepoPath 'build\v11-watch'
}
New-Item -ItemType Directory -Force -Path $StateDir | Out-Null

$logFile = Join-Path $stateDir 'watch.log'
$statusFile = Join-Path $stateDir 'status.json'
$readyFile = Join-Path $stateDir 'READY'
$badFile = Join-Path $stateDir 'BAD_SIGNATURE'

function Write-WatchLog {
    param([string] $Message)
    $line = "{0} {1}" -f (Get-Date).ToString('o'), $Message
    Add-Content -LiteralPath $logFile -Value $line -Encoding utf8
    Write-Output $line
}

function Write-StatusFile {
    param([string] $State, [int] $ExitCode, [string] $Detail)
    $payload = [pscustomobject]@{
        state      = $State
        exit_code  = $ExitCode
        detail     = $Detail
        tag        = $Tag
        probe_at   = (Get-Date).ToString('o')
        probe_count = $script:probeCount
    }
    $payload | ConvertTo-Json -Compress | Set-Content -LiteralPath $statusFile -Encoding utf8
}

$script:probeCount = 0

Write-WatchLog "watcher started: tag=$Tag interval=${IntervalSeconds}s repo=$ProbeRepoPath"

while ($true) {
    $script:probeCount++

    $output = & powershell -NoProfile -ExecutionPolicy Bypass -File $probeScript `
        -RepoPath $ProbeRepoPath -Tag $Tag -Principal $Principal 2>&1
    $code = $LASTEXITCODE
    $detail = ($output | Out-String).Trim()

    Write-WatchLog "probe #$($script:probeCount) exit=$code"

    switch ($code) {
        0 {
            Write-StatusFile -State 'ready' -ExitCode $code -Detail $detail
            Write-WatchLog "READY: $detail"
            Set-Content -LiteralPath $readyFile -Value $detail -Encoding utf8
            Remove-Item -LiteralPath $badFile -ErrorAction SilentlyContinue
            exit 0
        }
        2 {
            Write-StatusFile -State 'bad_signature' -ExitCode $code -Detail $detail
            Write-WatchLog "BAD_SIGNATURE: $detail"
            Set-Content -LiteralPath $badFile -Value $detail -Encoding utf8
            exit 2
        }
        1 {
            Write-StatusFile -State 'absent' -ExitCode $code -Detail $detail
        }
        default {
            # A probe failure (network, git, python) is not a verdict. Record
            # it and try again on the next tick.
            Write-StatusFile -State 'probe_error' -ExitCode $code -Detail $detail
            Write-WatchLog "probe error (will retry): $detail"
        }
    }

    if ($MaxProbes -gt 0 -and $script:probeCount -ge $MaxProbes) {
        Write-WatchLog "reached MaxProbes=$MaxProbes, stopping"
        exit 0
    }

    Start-Sleep -Seconds $IntervalSeconds
}
