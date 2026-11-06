<#
.SYNOPSIS
    One-shot probe: has the v1.1.0 release tag appeared on origin, and is it
    a properly signed annotated tag?

.DESCRIPTION
    Exit codes:
        0  the tag exists AND its signature verifies -> caller may start work
        1  the tag does not exist yet (the normal case while the release is
           still gated on the 7-day soak)
        2  the tag exists but its signature does NOT verify -> do not start
           work; this is a release-blocking condition, not a "not yet" case
        3  the probe itself could not run (no network, no git, no python)

    The script is deliberately silent on stdout except for one machine
    readable line, so it can be used as a watcher in a loop.

.NOTES
    Windows-only (Task Scheduler / the in-session watcher both run here).
    Output goes to stdout; the caller decides where to put it.
#>

[CmdletBinding()]
param(
    [string] $RepoPath = '',
    [string] $Tag = 'v1.1.0',
    [string] $Principal = 'realdesktop@163.com',
    # Suppress the machine-readable line on stdout; the exit code is the signal.
    [switch] $Quiet
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not $RepoPath) {
    # $PSCommandPath is not populated while the parameter block is bound, so
    # the default has to be resolved here instead of in the param default.
    $RepoPath = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
}

function Write-Result {
    param([string] $Status, [string] $Detail)
    if ($Quiet) { return }
    [pscustomobject]@{
        status = $Status
        detail = $Detail
        tag    = $Tag
        at     = (Get-Date).ToString('o')
    } | ConvertTo-Json -Compress
}

function Get-Git {
    param([string[]] $GitArgs)
    # A native command that writes to stderr (git always does on fetch: the
    # "From github.com:..." banner) is turned into a terminating error when
    # 2>&1 is used under $ErrorActionPreference = 'Stop'. Git's exit code is
    # the signal we want, so stderr is demoted to a normal stream here.
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & git -C $RepoPath @GitArgs 2>&1
    } finally {
        $ErrorActionPreference = $prev
    }
}

if (-not (Test-Path -LiteralPath $RepoPath)) {
    Write-Result -Status 'probe_error' -Detail "repo path not found: $RepoPath"
    exit 3
}

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Result -Status 'probe_error' -Detail 'git is not on PATH'
    exit 3
}

if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
    Write-Result -Status 'probe_error' -Detail 'python is not on PATH'
    exit 3
}

# --- 1. does the tag exist on the remote? --------------------------------
$remote = Get-Git @('ls-remote', '--tags', 'origin', "refs/tags/$Tag")
if ($LASTEXITCODE -ne 0) {
    # A network hiccup is not "the tag is absent". Do not wake anyone.
    Write-Result -Status 'probe_error' -Detail "git ls-remote failed: $remote"
    exit 3
}

$remoteLine = $remote | Where-Object { $_ -match "refs/tags/$Tag(\^\{\})?$" } | Select-Object -First 1
if (-not $remoteLine) {
    Write-Result -Status 'absent' -Detail 'tag not present on origin'
    exit 1
}

$remoteSha = ($remoteLine -split '\s+')[0]

# --- 2. fetch the tag object so it can be inspected locally --------------
$fetch = Get-Git @('fetch', '--no-tags', '--force', 'origin', "+refs/tags/${Tag}:refs/tags/${Tag}")
if ($LASTEXITCODE -ne 0) {
    Write-Result -Status 'probe_error' -Detail "git fetch of the tag failed: $fetch"
    exit 3
}

# --- 3. verify the signature --------------------------------------------
$verifyExit = 0
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    $verify = & python (Join-Path $RepoPath 'tools\verify_tag_signature.py') `
        --tag $Tag --repo $RepoPath --expected-principal $Principal --json 2>&1
    $verifyExit = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $prevEap
}

if ($verifyExit -ne 0) {
    Write-Result -Status 'unsigned' -Detail ($verify | Out-String).Trim()
    exit 2
}

Write-Result -Status 'ready' -Detail "signed tag $Tag at $remoteSha"
exit 0
