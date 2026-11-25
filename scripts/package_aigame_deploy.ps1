# Package the AI-game demo deployment bundle.
#
# This is the remote-desktop deployment bundle (`package_rd_deploy.ps1`)
# plus the AI-game pieces, so the whole thing can be unpacked on the
# operator's machine and used to play the synthetic whack-a-mole game
# over the wire -- and to *measure* how much latency the wire adds.
#
#   scripts\package_aigame_deploy.ps1
#   scripts\package_aigame_deploy.ps1 -BinDir build\aigame\examples\Release
#
# Outputs:
#   build\aigame-deploy\        the unpacked bundle
#   build\aigame-deploy.zip     the archive to copy to the operator
#
# What is in it, and why:
#
#   rd_device.exe / rd_viewer.exe  the media binaries.  `rd_device`
#                                  captures the synthetic game source
#                                  (`--game-source`), `rd_viewer` renders
#                                  it and drives the local input.
#   demo-agent-realtime.exe       the AI decision engine (slice
#   demo-agent-teammate.exe       v2-ai-decision / v4-ai-teammate).
#                                  **Self-contained**: no NimRTC
#                                  linkage, so no msquic / DTLS-cert
#                                  staging, and they run anywhere.
#   msquic.dll                    required by rd_viewer.exe.
#   crt/                          the MSVC runtime.
#   client.py + wsproto.py +      the pure-stdlib signalling/media
#   signalling.py + ...           harness.
#
# The AI decision binaries are **not** wired into the media path: the
# decision engine is a self-contained arithmetic model, and the remote
# gameplay session is the media path (capture -> encode -> RTP -> SRTP
# -> decode -> render -> input).  The bundle ships both so the operator
# can run the AI's own self-test next to a live session and compare the
# decision latency with the transport latency the viewer reports.

[CmdletBinding()]
param(
    # Where the media binaries live.  `rd_device.exe` and `rd_viewer.exe`
    # are taken from here.
    [string]$BinDir = 'build/aigame/examples/Release',

    # Where the AI decision binaries live.  In a multi-config generator
    # (MSVC) they land in their own per-example sub-directory, so the
    # default is derived from $BinDir.
    [string]$AgentBinDir = '',

    # Where the Python harness lives.
    [string]$SrcDir = 'examples/remote-desktop',

    # Where the build tree that holds the MSVC runtime DLLs lives.
    [string]$CrtDir = '',

    [string]$OutDir = 'build/aigame-deploy',
    [string]$ZipPath = 'build/aigame-deploy.zip',

    # Silence the "the binaries are older than the sources" warning.
    [switch]$NoStaleCheck
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Log($m) { Write-Host ("[{0:HH:mm:ss}] {1}" -f (Get-Date), $m) }

function Abs($p) {
    if ([System.IO.Path]::IsPathRooted($p)) { return $p }
    return (Join-Path $root $p)
}

$BinDir = Abs $BinDir
$SrcDir = Abs $SrcDir
$OutDir = Abs $OutDir
$ZipPath = Abs $ZipPath

if (-not $AgentBinDir) {
    # `build/aigame/examples/Release` -> the agent binaries live in
    # `build/aigame/examples/demo-agent-realtime/Release`: the *examples*
    # directory is the parent of both, and each example has its own
    # sub-directory under it.
    $examplesDir = Split-Path -Parent $BinDir
    $cfg = Split-Path -Leaf $BinDir
    $AgentBinDir = Join-Path $examplesDir "demo-agent-realtime/$cfg"
}
$AgentBinDir = Abs $AgentBinDir

# ---- locate the runtime -------------------------------------------------
# The CRT DLLs ship with the Visual Studio *redistributable*, not with
# the build tree.  They live under `VC/Redist/MSVC/<ver>/x64/` and are
# the right payload to ship: they are the officially redistributable
# copies.
if (-not $CrtDir) {
    $vsRoot = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\2022',
                              'D:\Program Files\Microsoft Visual Studio\2022',
                              "${env:ProgramFiles}\Microsoft Visual Studio\2022" `
                -Directory -ErrorAction SilentlyContinue |
                Where-Object { Test-Path (Join-Path $_.FullName 'VC\Redist\MSVC') } |
                Select-Object -First 1
    if (-not $vsRoot) {
        throw 'Cannot find the Visual Studio installation; pass -CrtDir explicitly.'
    }
    # The newest `Microsoft.VC*.CRT` folder wins: it is the runtime the
    # current toolset was built against.  (The inner variable has a
    # different name on purpose -- PowerShell variable names are
    # case-insensitive, so `$crtDir` *is* `$CrtDir`.)
    $found = Get-ChildItem (Join-Path $vsRoot.FullName 'VC\Redist\MSVC') -Directory -ErrorAction SilentlyContinue |
             Sort-Object Name -Descending |
             ForEach-Object {
                 Get-ChildItem (Join-Path $_.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue
             } |
             Select-Object -First 1
    if (-not $found) {
        throw 'Cannot find the redistributable CRT; pass -CrtDir explicitly.'
    }
    $CrtDir = $found.FullName
}
$CrtDir = Abs $CrtDir
Log "the MSVC runtime comes from $CrtDir"

# ---- the CRT payload ----------------------------------------------------
# These are the DLLs the binaries import that are *not* part of Windows
# itself.
$wantCrt = @(
    'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
    'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll',
    'vcruntime140.dll', 'vcruntime140_1.dll', 'vcruntime140_threads.dll',
    'concrt140.dll', 'vccorlib140.dll'
)

# ---- msquic --------------------------------------------------------------
# `rd_viewer.exe` hard-links against msquic (the QUIC transport); without
# it the process dies with 0xc000007b before it prints anything.
#
# The DLL has to come from the *same* build tree as the binaries: a
# different configuration (Debug vs Release) links against a different
# msquic, and the two would not agree on the ABI.
#
# There is deliberately **no fallback search** here.  Picking up a
# msquic.dll from some *other* tree in `build/` produces a bundle whose
# media binary and whose QUIC runtime were built in different
# configurations -- an ABI mismatch that shows up as a crash on the
# operator's machine, with nothing in the bundle to explain it.
$msquic = Join-Path $BinDir 'msquic.dll'
if (-not (Test-Path $msquic)) {
    throw ("Cannot find msquic.dll next to the media binaries ($BinDir). " +
           "The bundle must be built from ONE build tree: the DLL and the " +
           "EXEs have to come from the same configuration. " +
           "Build the tree first, then re-run this script with -BinDir.")
}
Log "msquic.dll comes from $msquic"

# ---- the bundle's provenance -------------------------------------------
#
# A bundle is only useful if it carries the code that was just fixed.
# The failure this guards against is quiet and expensive: the binaries
# in `build/` are rebuilt, the bundle is *not* refreshed, and the
# operator receives a bundle with the old code in it.  Nothing in the
# bundle says so -- the operator just sees the bug they thought was
# fixed.
#
# So the bundle records what it was built from, and the staleness check
# compares the binaries against the sources they were built from.
function Get-NewestSourceTime {
    param([string[]]$Exclude = @())
    $newest = [datetime]::MinValue
    foreach ($dir in 'src', 'examples') {
        $srcRoot = Join-Path $root $dir
        if (-not (Test-Path $srcRoot)) { continue }
        Get-ChildItem $srcRoot -Recurse -File -Include *.cpp,*.hpp,*.h,*.cc,*.cxx -ErrorAction SilentlyContinue |
            Where-Object {
                ($_.FullName -notlike '*\third_party\*') -and
                ($Exclude -notcontains $_.FullName)
            } |
            ForEach-Object {
                if ($_.LastWriteTime -gt $newest) { $newest = $_.LastWriteTime }
            }
    }
    return $newest
}

# The staleness check is **per binary**, not one comparison against every
# source.  Each media binary has exactly one source file of its own
# (`rd_device.cpp` / `rd_viewer.cpp`); everything else in the tree is
# shared, and a shared file's change genuinely affects both.
#
# The naive version -- compare every binary against the newest source
# anywhere -- is wrong in the direction that hurts: editing
# `rd_viewer.cpp` marks `rd_device.exe` stale, even though `rd_device`
# does not compile that file.  A warning that fires on unrelated edits
# trains the operator to ignore it, and then it does not protect them
# from the case it exists for.
$staleBinaries = @()
foreach ($exe in 'rd_device.exe', 'rd_viewer.exe') {
    $src = Join-Path $BinDir $exe
    if (-not (Test-Path $src)) { continue }
    # The sibling's own source is the one file this binary does not see.
    $own = [System.IO.Path]::ChangeExtension($exe, '.cpp')
    $sibling = if ($own -eq 'rd_device.cpp') { 'rd_viewer.cpp' } else { 'rd_device.cpp' }
    $siblingPath = Join-Path (Join-Path $root 'examples/remote-desktop') $sibling
    $newestSource = Get-NewestSourceTime -Exclude @($siblingPath)
    $built = (Get-Item $src).LastWriteTime
    if ($built -lt $newestSource) {
        $staleBinaries += "$exe (built $built, newest source $newestSource)"
    }
}

if ($staleBinaries.Count -gt 0 -and -not $NoStaleCheck) {
    Write-Host ''
    Write-Host 'WARNING: the binaries are OLDER than the sources they were built from:' -ForegroundColor Yellow
    $staleBinaries | ForEach-Object { Write-Host ("  " + $_) -ForegroundColor Yellow }
    Write-Host ''
    Write-Host 'The bundle would ship code that predates the current sources.' -ForegroundColor Yellow
    Write-Host 'Rebuild the tree, or pass -BinDir pointing at a freshly built one.' -ForegroundColor Yellow
    Write-Host ''
    Write-Host 'Continuing anyway (pass -NoStaleCheck to silence this).' -ForegroundColor Yellow
    Write-Host ''
}

# ---- the AI decision binaries --------------------------------------------
#
# They are part of what this bundle is *for*, so they are checked
# **before** anything is assembled: a bundle that ships the media path
# without the "AI" half is a bundle that cannot do the thing it is
# named after, and a half-built bundle left on disk is worse than no
# bundle -- it looks finished.
#
# They are self-contained (no NimRTC linkage), so there is no msquic /
# DTLS-cert to stage for them -- but they DO have to exist.
foreach ($exe in 'demo-agent-realtime.exe', 'demo-agent-teammate.exe') {
    $src = Join-Path $AgentBinDir $exe
    if (-not (Test-Path $src)) {
        throw ("Missing $src`n" +
               "The AI decision binaries are part of this bundle.  Build them " +
               "with: cmake --build <tree> --config Release --target " +
               "demo-agent-realtime demo-agent-teammate")
    }
}
Log "the AI decision binaries come from $AgentBinDir"

# ---- assemble -----------------------------------------------------------
if (Test-Path $OutDir) { Remove-Item $OutDir -Recurse -Force }
New-Item -ItemType Directory -Path $OutDir | Out-Null
New-Item -ItemType Directory -Path (Join-Path $OutDir 'crt') | Out-Null

foreach ($exe in 'rd_device.exe', 'rd_viewer.exe') {
    $src = Join-Path $BinDir $exe
    if (-not (Test-Path $src)) { throw "Missing $src" }
    Copy-Item $src $OutDir -Force
}

foreach ($exe in 'demo-agent-realtime.exe', 'demo-agent-teammate.exe') {
    Copy-Item (Join-Path $AgentBinDir $exe) $OutDir -Force
}

Copy-Item $msquic $OutDir -Force

foreach ($py in 'client.py', 'wsproto.py', 'signalling.py', 'signalling_core.py') {
    $src = Join-Path $SrcDir $py
    if (-not (Test-Path $src)) { throw "Missing $src" }
    Copy-Item $src $OutDir -Force
}

# The operator's cheat sheet: the AI-game one, falling back to the
# remote-desktop one.
$aigameReadme = Join-Path $SrcDir 'deploy/AIGAME-README.md'
$rdReadme = Join-Path $SrcDir 'deploy/README.md'
if (Test-Path $aigameReadme) { Copy-Item $aigameReadme $OutDir -Force }
elseif (Test-Path $rdReadme) { Copy-Item $rdReadme $OutDir -Force }

$missingCrt = @()
foreach ($dll in $wantCrt) {
    $src = Join-Path $CrtDir $dll
    if (Test-Path $src) { Copy-Item $src (Join-Path $OutDir 'crt') -Force }
    else { $missingCrt += $dll }
}
if ($missingCrt.Count -gt 0) {
    throw "These runtime DLLs are missing from $CrtDir : $($missingCrt -join ', ')"
}

# ---- the bundle's provenance --------------------------------------------
#
# The operator receives a zip and cannot see into the tree it came from.
# A record inside the bundle says what it was built from, so a bug
# report ("it still drops frames") can be answered with "which build
# did you actually run?" instead of a guess.
$commit = 'unknown'
try {
    $commit = (& git -C $root rev-parse --short HEAD 2>$null).Trim()
    if (-not $commit) { $commit = 'unknown' }
} catch { $commit = 'unknown' }

$branch = 'unknown'
try {
    $branch = (& git -C $root rev-parse --abbrev-ref HEAD 2>$null).Trim()
    if (-not $branch) { $branch = 'unknown' }
} catch { $branch = 'unknown' }

$provenance = @(
    'NimRTC AI-game demo bundle',
    '',
    "built:      $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')",
    "commit:     $commit",
    "branch:     $branch",
    "binaries:   $BinDir",
    "agents:     $AgentBinDir",
    "msquic:     $(Split-Path -Leaf $msquic)",
    "stale:      $(if ($staleBinaries.Count -gt 0) { 'YES -- the binaries predate the sources' } else { 'no' })",
    '',
    'If a bug is reported against this bundle, quote the commit above.',
    ''
)
$provenance | Set-Content (Join-Path $OutDir 'BUNDLE-INFO.txt') -Encoding ascii
Log "the bundle records commit $commit"

Log "the bundle is in $OutDir"

# ---- the entry points ---------------------------------------------------
# The `.bat` wrappers put `crt/` on the DLL search path and hand every
# argument to `client.py`.
#
# The device side is the one that matters for the game: `--game-source`
# makes `rd_device` capture the synthetic whack-a-mole board instead of
# the real screen, so the session is a *game* and not a screen share.
@'
@echo off
REM The device-side entry point of the AI-game deployment package.
REM
REM   rd_device.bat --url ws://SIGNALLING_IP:8765
REM
REM The game source is the default here: this bundle is for playing the
REM synthetic whack-a-mole board over the wire and measuring the
REM latency, not for sharing a real screen.
setlocal
cd /d "%~dp0"
set PATH=%~dp0crt;%PATH%
set PYTHONIOENCODING=utf-8
python "%~dp0client.py" --role device --media rd --binary "%~dp0rd_device.exe" --game-source %*
endlocal
'@ | Set-Content (Join-Path $OutDir 'rd_device.bat') -Encoding ascii

@'
@echo off
REM The client-side entry point of the AI-game deployment package.
REM
REM It puts the bundled MSVC runtime on the DLL search path, then runs
REM the Python harness, which drives rd_viewer.exe over stdin/stdout.
REM
REM   rd_viewer.bat --url ws://DEVICE_IP:8765 --code 123456
REM
REM Every extra argument is passed through to client.py.
setlocal
cd /d "%~dp0"
set PATH=%~dp0crt;%PATH%
set PYTHONIOENCODING=utf-8
python "%~dp0client.py" --role viewer --media rd --binary "%~dp0rd_viewer.exe" %*
endlocal
'@ | Set-Content (Join-Path $OutDir 'rd_viewer.bat') -Encoding ascii

# The AI decision binaries get entry points too, so the operator does
# not have to remember their flags.  They are self-tests: exit 0 means
# every acceptance criterion held.
@'
@echo off
REM The AI decision engine's own acceptance run (slice v2-ai-decision).
REM
REM   demo_agent_self_test.bat
REM
REM Exits 0 when all four acceptance criteria hold, non-zero otherwise.
setlocal
cd /d "%~dp0"
set PATH=%~dp0crt;%PATH%
"%~dp0demo-agent-realtime.exe" --self-test --decisions 100 --debug
endlocal
'@ | Set-Content (Join-Path $OutDir 'demo_agent_self_test.bat') -Encoding ascii

@'
@echo off
REM The local-model AI teammate's own acceptance run (slice v4-ai-teammate).
REM
REM   demo_teammate_self_test.bat
REM
REM Exits 0 when all five acceptance criteria hold, non-zero otherwise.
setlocal
cd /d "%~dp0"
set PATH=%~dp0crt;%PATH%
"%~dp0demo-agent-teammate.exe" --self-test
endlocal
'@ | Set-Content (Join-Path $OutDir 'demo_teammate_self_test.bat') -Encoding ascii

# ---- the zip ------------------------------------------------------------
if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
Compress-Archive -Path (Join-Path $OutDir '*') -DestinationPath $ZipPath -Force
Log "the archive is $ZipPath ($([math]::Round((Get-Item $ZipPath).Length / 1MB, 2)) MB)"

Log 'OK: the AI-game deployment bundle is ready'
