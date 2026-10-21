# Generate a self-signed certificate for demo-wtserver.
#
# Installs it in Cert:\CurrentUser\My and writes the SHA-1 thumbprint +
# hash to files for msquic to load via QUIC_CREDENTIAL_TYPE_CERTIFICATE_
# HASH_STORE.  The default build directory is `<project_root>/build`,
# resolved from this script's location, so the script is portable.
#
# Override the build directory via -BuildDir:
#   powershell -File .\scripts\generate_wtserver_cert.ps1 -BuildDir E:\out\build
#
# Both this script and its Python sibling generate the same two files
# (wtserver_thumbprint.txt and wtserver_hash.txt) so the C++ demo-wtserver
# can load the cert regardless of which one was used to generate it.
#
# Idempotent: rerunning replaces existing files with the regenerated cert.
# Cert:\CurrentUser\My is cleaned up at logout, so long-lived deployments
# should also pin the cert into Cert:\LocalMachine.
#
# NOTE: This script avoids [CmdletBinding()] / param() because the
# PowerShell 5.1 parser (the default on Windows Server 2019 and GitHub
# Actions windows-2019/windows-2022 runners) sometimes emits
# "UnexpectedAttribute" errors when scanning [CmdletBinding()] at the
# top of a -File-loaded .ps1, even though the construct works in
# interactive REPL mode.  Plain global $args / param() parsing is
# robust across both modes.
#
# Positional args:
#   1st optional arg = $Days  (default 365)
#   2nd optional arg = $KeyLength  (default 2048)
#   -BuildDir <path>         override build directory (default:
#                             <project_root>/build)
#   -CertStoreLocation <p>   override store location (default
#                             Cert:\CurrentUser\My)

$ErrorActionPreference = "Stop"

# --- Parse args (manual so we don't need [CmdletBinding()]) ---
$Days     = if ($args.Count -ge 1 -and $args[0] -match '^\d+$') { [int]$args[0] } else { 365 }
$KeyLength = if ($args.Count -ge 2 -and $args[1] -match '^\d+$') { [int]$args[1] } else { 2048 }

$BuildDir = ""
$CertStoreLocation = "Cert:\CurrentUser\My"

for ($i = 0; $i -lt $args.Count; $i++) {
    switch ($args[$i]) {
        "-BuildDir"          { $BuildDir = $args[++$i] }
        "-CertStoreLocation" { $CertStoreLocation = $args[++$i] }
        default { }
    }
}

# --- Resolve project root from this script's location ---
if (-not $PSScriptRoot) {
    $PSScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
}
$ProjectRoot = Split-Path -Parent $PSScriptRoot

if (-not $BuildDir) {
    $BuildDir = Join-Path $ProjectRoot "build"
}
if (-not (Test-Path $BuildDir)) {
    Write-Host "Creating build directory: $BuildDir"
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
}

Write-Host "Project root: $ProjectRoot"
Write-Host "Build dir:    $BuildDir"

# --- Generate the self-signed cert ---
$cert = New-SelfSignedCertificate `
    -DnsName "localhost" `
    -CertStoreLocation $CertStoreLocation `
    -NotAfter ((Get-Date).AddDays($Days)) `
    -KeyAlgorithm RSA `
    -KeyLength $KeyLength `
    -KeyExportPolicy Exportable

$thumbprint = $cert.Thumbprint
Write-Host "Certificate created: thumbprint=$thumbprint"

# --- Write thumbprint (uppercase, matches `New-SelfSignedCertificate`) ---
Set-Content -Path (Join-Path $BuildDir "wtserver_thumbprint.txt") `
            -NoNewline -Value $thumbprint -Encoding ASCII

# --- Write raw SHA-1 hex (lowercase; read by C++ via NIMRTC_BUILD_ROOT) ---
$hashBytes = $cert.GetCertHash()
$hashHex = -join ($hashBytes | ForEach-Object { $_.ToString("x2") })
Set-Content -Path (Join-Path $BuildDir "wtserver_hash.txt") `
            -NoNewline -Value $hashHex -Encoding ASCII

Write-Host "SHA1 hash: $hashHex"
Write-Host "Files written to $BuildDir"
Write-Host "Certificate stays in $CertStoreLocation until logout."
Write-Host "Done."
