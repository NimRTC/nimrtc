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
    [string]$ZipPath = 'build/rd-deploy.zip'
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
$msquic = Join-Path $BinDir 'msquic.dll'
if (-not (Test-Path $msquic)) {
    $msquic = (Get-ChildItem (Join-Path $root 'build') -Recurse -Filter 'msquic.dll' -ErrorAction SilentlyContinue |
               Where-Object { $_.FullName -notlike '*rd-deploy*' } |
               Sort-Object LastWriteTime -Descending |
               Select-Object -First 1).FullName
}
if (-not $msquic -or -not (Test-Path $msquic)) {
    throw 'Cannot find msquic.dll in the build tree.'
}
Log "msquic.dll comes from $msquic"

# ---- assemble -----------------------------------------------------------
if (Test-Path $OutDir) { Remove-Item $OutDir -Recurse -Force }
New-Item -ItemType Directory -Path $OutDir | Out-Null
New-Item -ItemType Directory -Path (Join-Path $OutDir 'crt') | Out-Null

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
