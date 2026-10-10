# =============================================================================
# record_loopback_demo.ps1 — record a REAL asciinema session of loopback-p2p
#
# Replaces the synthesized asciicast in examples/wasm-demo/www/loopback-p2p.cast
# with a real terminal capture. Requires:
#   1. asciinema on PATH:  pipx install asciinema  (then add Scripts/ to PATH)
#      OR:  scoop install asciinema  OR  choco install asciinema
#   2. A built `loopback-p2p.exe` (cmake --preset debug.msvc + build target loopback-p2p)
#
# Usage:
#   pwsh tools/record_loopback_demo.ps1                                  # auto-detect
#   pwsh tools/record_loopback_demo.ps1 -Binary build\examples\Debug\loopback-p2p.exe
#   $env:LOOPBACK_BIN = "..."; pwsh tools/record_loopback_demo.ps1
# =============================================================================
[CmdletBinding()]
param(
    [string]$Binary
)

$ErrorActionPreference = 'Stop'

$RepoRoot  = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$CastPath  = Join-Path $RepoRoot 'examples/wasm-demo/www/loopback-p2p.cast'

function Log([string]$msg) { Write-Host "[record] $msg" -ForegroundColor Blue }
function Warn([string]$msg) { Write-Host "[record] $msg" -ForegroundColor Yellow }
function Die([string]$msg) { Write-Host "[record] $msg" -ForegroundColor Red; exit 1 }

if (-not (Get-Command asciinema -ErrorAction SilentlyContinue)) {
    Die "asciinema not on PATH. Install: pipx install asciinema | scoop install asciinema | choco install asciinema"
}

# --- locate the binary ---------------------------------------------------------
if (-not $Binary) {
    if ($env:LOOPBACK_BIN) { $Binary = $env:LOOPBACK_BIN }
    else {
        $found = Get-ChildItem -Path (Join-Path $RepoRoot 'build') -Recurse -Filter 'loopback-p2p*.exe' -ErrorAction SilentlyContinue |
                 Select-Object -First 1 -ExpandProperty FullName
        if ($found) { $Binary = $found }
    }
}
if (-not $Binary -or -not (Test-Path $Binary)) {
    Die "loopback-p2p binary not found.
Hint:  cmake --preset debug.msvc ; cmake --build build --target loopback-p2p
       then re-run this script with -Binary build\examples\Debug\loopback-p2p.exe"
}
Log "binary: $Binary"

# --- smoke test ----------------------------------------------------------------
Log "smoke test (2s)..."
$proc = Start-Process -FilePath $Binary -ArgumentList '2' -NoNewWindow -PassThru -RedirectStandardOutput 'NUL' -RedirectStandardError 'NUL' -Wait
if ($proc.ExitCode -ne 0) {
    Warn "smoke test exited $($proc.ExitCode). The recording may not show 'connected'."
    Warn "Press Ctrl-C to abort, or any key to continue anyway."
    [void]$Host.UI.RawUI.ReadKey('NoEcho,IncludeKeyDown')
}

# --- backup + record -----------------------------------------------------------
if (Test-Path $CastPath) {
    $Backup = "$CastPath.synthesized.bak"
    Copy-Item -Path $CastPath -Destination $Backup -Force
    Log "backed up current cast -> $Backup"
}

Log "recording to $CastPath"
Log "press Ctrl-C after the loopback example finishes (typically ~8s)."

# asciinema rec flags mirror the .sh version. Note: --idle-time-limit prevents
# the cast from dragging during the tick loop.
& asciinema rec `
    --quiet `
    --title 'NimRTC loopback-p2p' `
    --cols 132 --rows 30 `
    --idle-time-limit 2 `
    --command "$Binary" `
    $CastPath

Log "recorded. file: $CastPath"
Log "verify locally:  python -m http.server -d examples/wasm-demo/www"
Log "                 open http://127.0.0.1:8000/"
Log "then:  git add $CastPath ; git commit ; git push"
