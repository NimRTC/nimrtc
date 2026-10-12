# set_repo_metadata.ps1
# Idempotently applies the canonical NimRTC repo description + topics via
# the GitHub CLI (`gh`). Safe to re-run; `gh repo edit` is a no-op when
# nothing changes.
#
# Prerequisites:
#   - gh (>= 2.0) on PATH  (Test: `gh --version`)
#   - `gh auth login` against github.com with `repo` scope
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\set_repo_metadata.ps1 [-Target OWNER/REPO]
#   # default OWNER/REPO: NimRTC/nimrtc (read from git remote if absent)

[CmdletBinding()]
param(
    [string]$Target
)

$ErrorActionPreference = 'Stop'

if (-not $Target) {
    try {
        $remoteUrl = git config --get remote.origin.url 2>$null
    } catch {
        $remoteUrl = $null
    }
    if ($remoteUrl -and $remoteUrl -match 'github\.com[:/]([^/]+/[^/.]+)(\.git)?$') {
        $Target = $Matches[1]
    } else {
        $Target = 'NimRTC/nimrtc'
    }
}

$Description = 'A from-scratch C++20 WebRTC engine. P2P client + SFU gateway in one source tree. ~5.6 MB static lib. Chrome / Firefox / libwebrtc wire interop. Pluggable crypto: OpenSSL / wolfSSL / mbedTLS / GMSSL-ready for Xinchuang. 4-platform CI.'

$Topics = @(
    'webrtc',
    'cpp20',
    'realtime-communication',
    'rtc',
    'sfu',
    'p2p',
    'audio-video',
    'webrtc-alternative',
    'xinchuang',
    'gmssl'
)

$topicArgs = @()
foreach ($t in $Topics) { $topicArgs += @('--add-topic', $t) }

Write-Host "Target repo: $Target"
Write-Host "Description: $Description"
Write-Host "Topics:      $($Topics -join ', ')"
Write-Host ''
Write-Host 'Applying via gh repo edit ...'

& gh repo edit $Target --description $Description @topicArgs
if ($LASTEXITCODE -ne 0) {
    throw "gh repo edit failed with exit code $LASTEXITCODE"
}

Write-Host ''
Write-Host 'Verifying:'
& gh repo view $Target --json name,description,repositoryTopics `
    --template '{{.name}} | {{.description}}{{range .repositoryTopics}}`n  - {{.name}}{{end}}'
