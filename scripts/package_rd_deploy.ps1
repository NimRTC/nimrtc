# Refresh the remote-desktop deployment bundle.
#
# The bundle is what ships to an operator's machine: the two media
# binaries, the MSVC runtime they need, `msquic.dll`, and the pure
# stdlib Python harness that drives them.  The operator's machine needs
# no build toolchain and no "VC++ redistributable" install.
#
#   scripts\package_rd_deploy.ps1
#   scripts\package_rd_deploy.ps1 -BinDir build\msvc-full\examples -BuildType RelWithDebInfo
#
# Outputs:
#   build\rd-deploy\       the unpacked bundle
#   build\rd-deploy.zip    the archive to copy to the operator

[CmdletBinding()]
param(
    # Where the media binaries live.  Both `rd_device.exe` and
    # `rd_viewer.exe` are taken from here.
    [string]$BinDir = 'build/msvc-full/examples',

    # Where the Python harness lives.  The four files are copied
    # verbatim -- they are the same ones the source tree runs.
    [string]$SrcDir = 'examples/remote-desktop',

    # The build tree that holds the MSVC runtime DLLs.  The runtime is
    # copied into `crt/` so the `.bat` wrappers can put it on the DLL
    # search path.
    [string]$CrtDir = '',

    [string]$OutDir = 'build/rd-deploy',
    [string]$ZipPath = 'build/rd-deploy.zip',

    # Silence the "the binaries are older than the sources" warning.
    # It is a warning, not an error: there are legitimate reasons to
    # package a build that is not the newest one (a bisect, a release
    # candidate from a tag).  The warning exists so the *common* mistake
    # -- forgetting to rebuild -- is visible, not to forbid the rest.
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

# ---- locate the runtime -------------------------------------------------
# The CRT DLLs ship with the Visual Studio *redistributable*, not with
# the build tree.  They live under `VC/Redist/MSVC/<ver>/x64/` and are
# the right payload to ship: they are the officially redistributable
# copies.
if (-not $CrtDir) {
    # The Visual Studio install may sit under any drive, and the
    # redistributable lives inside the *edition* folder (Community,
    # Professional, Enterprise), not directly under the version folder.
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
# These are the DLLs the two binaries import that are *not* part of
# Windows itself.  `msvcp140*.dll` and `vcruntime140*.dll` come from
# the redistributable; the `vccorlib140` / `concrt140` pair comes from
# the toolset.
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
# msquic, and the two would not agree on the ABI.  The copy that sits
# next to the binaries in the build tree is that one.
#
# There is deliberately **no fallback search** here.  Picking up a
# msquic.dll from some *other* tree in `build/` produces a bundle whose
# media binary and whose QUIC runtime were built in different
# configurations -- an ABI mismatch that shows up as a crash on the
# operator's machine, with nothing in the bundle to explain it.  The
# bundle has to come from one tree, or not at all.
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
#
# A binary is *older* than the sources when a source file is newer than
# it.  That is the conservative direction: a rebuild that touched
# nothing relevant still counts as fresh, but a source edited after the
# last build always marks the bundle as stale.  The comparison ignores
# `src/third_party/` (vendored code, and its timestamps say nothing
# about *this* build) and the build tree itself.
function Get-NewestSourceTime {
    $srcRoot = Join-Path $root 'src'
    if (-not (Test-Path $srcRoot)) { return [datetime]::MinValue }
    $newest = [datetime]::MinValue
    Get-ChildItem $srcRoot -Recurse -File -Include *.cpp,*.hpp,*.h,*.cc,*.cxx -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notlike '*\third_party\*' } |
        ForEach-Object {
            if ($_.LastWriteTime -gt $newest) { $newest = $_.LastWriteTime }
        }
    return $newest
}

$newestSource = Get-NewestSourceTime
$staleBinaries = @()
foreach ($exe in 'rd_device.exe', 'rd_viewer.exe') {
    $src = Join-Path $BinDir $exe
    if (-not (Test-Path $src)) { continue }
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

# ---- assemble -----------------------------------------------------------
if (Test-Path $OutDir) { Remove-Item $OutDir -Recurse -Force }
New-Item -ItemType Directory -Path $OutDir | Out-Null
New-Item -ItemType Directory -Path (Join-Path $OutDir 'crt') | Out-Null

# The DTLS certificate.  It is looked up **relative to the working
# directory** first (`certs/server-ecc.pem` next to the executable), and
# the absolute fallback (`NIMRTC_WOLFSSL_CERT_DIR`) points into the
# *build machine's* source tree -- which does not exist on the
# operator's machine.  Without this copy the operator gets a bundle
# whose signalling works, whose pairing works, whose ICE candidates
# gather, and which then dies at the handshake with
# "wolfSSL_CTX setup failed": the one failure the operator cannot
# diagnose from the message.
$certSrc = Join-Path $BinDir 'certs'
foreach ($pem in 'server-ecc.pem', 'ecc-key.pem') {
    $src = Join-Path $certSrc $pem
    if (-not (Test-Path $src)) {
        throw ("Missing $src`n" +
               "The DTLS certificate is part of this bundle: without it the " +
               "handshake fails on the operator's machine with " +
               "'wolfSSL_CTX setup failed'.  Build the tree first (CMake " +
               "stages the certs next to the executables).")
    }
}
$certDst = Join-Path $OutDir 'certs'
New-Item -ItemType Directory -Path $certDst | Out-Null
foreach ($pem in 'server-ecc.pem', 'ecc-key.pem') {
    Copy-Item (Join-Path $certSrc $pem) $certDst -Force
}

foreach ($exe in 'rd_device.exe', 'rd_viewer.exe') {
    $src = Join-Path $BinDir $exe
    if (-not (Test-Path $src)) { throw "Missing $src" }
    Copy-Item $src $OutDir -Force
}

Copy-Item $msquic $OutDir -Force

foreach ($py in 'client.py', 'wsproto.py', 'signalling.py', 'signalling_core.py') {
    $src = Join-Path $SrcDir $py
    if (-not (Test-Path $src)) { throw "Missing $src" }
    Copy-Item $src $OutDir -Force
}

# The operator's cheat sheet.
$readme = Join-Path $SrcDir 'deploy/README.md'
if (Test-Path $readme) { Copy-Item $readme $OutDir -Force }

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
#
# It is a plain text file on purpose: it has to be readable with
# `notepad` on a machine that has no toolchain at all.
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
    'NimRTC remote-desktop bundle',
    '',
    "built:      $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')",
    "commit:     $commit",
    "branch:     $branch",
    "binaries:   $BinDir",
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
# argument to `client.py`.  `--media rd` is already the default, so the
# operator does not have to spell it out.
@'
@echo off
REM The device-side entry point of the remote-desktop deployment package.
REM
REM   rd_device.bat --url ws://SIGNALLING_IP:8765
REM
setlocal
cd /d "%~dp0"
set PATH=%~dp0crt;%PATH%
set PYTHONIOENCODING=utf-8
python "%~dp0client.py" --role device --media rd --binary "%~dp0rd_device.exe" %*
endlocal
'@ | Set-Content (Join-Path $OutDir 'rd_device.bat') -Encoding ascii

@'
@echo off
REM The client-side entry point of the remote-desktop deployment package.
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

# ---- the zip ------------------------------------------------------------
if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
Compress-Archive -Path (Join-Path $OutDir '*') -DestinationPath $ZipPath -Force
Log "the archive is $ZipPath ($([math]::Round((Get-Item $ZipPath).Length / 1MB, 2)) MB)"

Log 'OK: the deployment bundle is ready'
