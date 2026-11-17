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

# --- DER length encoder ----------------------------------------------------
#
# Used by the SPKI computation below.  DER encodes a length either as a
# single byte (< 0x80) or as 0x80 | <byte count> followed by the big-endian
# length bytes.
function Write-DerLength([System.IO.Stream]$s, [int]$len) {
    if ($len -lt 0x80) {
        $s.WriteByte([byte]$len)
        return
    }
    $bytes = @()
    $v = $len
    while ($v -gt 0) { $bytes = @($v -band 0xFF) + $bytes; $v = $v -shr 8 }
    $s.WriteByte([byte](0x80 -bor $bytes.Count))
    foreach ($b in $bytes) { $s.WriteByte([byte]$b) }
}

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

# --- Write the SPKI SHA-256 hash for Chrome's certificate allowlist ---
#
# This is what makes Chrome accept the server, and it was the reason the
# WebTransport half of the Chrome interop case (Case E) had been failing
# since 2026-10-24.
#
# The chain of causes:
#
#   1. `New-SelfSignedCertificate -CertStoreLocation Cert:\CurrentUser\My`
#      puts the certificate in the *personal* store, which is where msquic
#      loads it from (QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH_STORE) -- so
#      the server side always worked.
#   2. But Chrome, like every other client, validates the *chain*: a
#      self-signed certificate is its own issuer, so unless the exact
#      certificate is in a TRUST store it is an "unknown root" and the
#      handshake dies with QUIC_TLS_CERTIFICATE_UNKNOWN (alert 46).
#   3. The harness passes `--ignore-certificate-errors`, which is NOT
#      enough for WebTransport: Chromium's QUIC proof verifier turns a
#      successful verification whose issuer is not a known root into
#      ERR_QUIC_CERT_ROOT_NOT_KNOWN anyway.
#
# There are two ways to fix that, and this script takes the second one:
#
#   (a) install the certificate into Cert:\CurrentUser\Root -- the machine's
#       *trusted* store, so the chain validates for real;
#   (b) publish the certificate's SPKI SHA-256 so the harness can hand it
#       to Chrome via --ignore-certificate-errors-spki-list.
#
# (b) is what this script does, because (a) is not viable in CI:
#
#   * Writing to Cert:\CurrentUser\Root from a *non-interactive* session
#     HANGS.  The protected-root store raises a confirmation dialog that
#     nobody can answer on a GitHub Actions runner, so the certificate
#     script never returns and the job dies at its 30-minute timeout.
#     (The same call from an interactive desktop works, which is exactly
#     what makes this so easy to get wrong: it passes locally and hangs on
#     the runner.)
#   * Mutating the machine's trust store to make a *test* pass is the wrong
#     shape anyway: it changes the trust decisions of every other program
#     on the machine for the lifetime of the certificate.
#
# (b) touches no machine state at all: the SPKI hash is a file next to the
# certificate hash, and only the harness's own Chrome instance is told to
# trust it.  This is the same mechanism the DTLS plane of Case E already
# uses (demo-p2p writes its SPKI; the harness passes it to Chrome).
#
# The SPKI is the SubjectPublicKeyInfo -- the DER block holding the
# algorithm identifier and the public key -- and Chrome hashes it with
# SHA-256 and base64-encodes the result.  The certificate's public key is
# reached through two store properties:
#
#   PublicKey.EncodedParameters -- the AlgorithmIdentifier's *parameters*
#   PublicKey.EncodedKeyValue  -- the DER RSAPublicKey SEQUENCE
#
# and the SPKI is
#
#   SEQUENCE { SEQUENCE { OID rsaEncryption, NULL }, BIT STRING <RSAPublicKey> }
#
# `EncodedParameters` is only the OID/NULL pair, so the AlgorithmIdentifier
# SEQUENCE is rebuilt here; its DER is fixed by RFC 3279 for rsaEncryption.
try {
    $rsaEncryption = [byte[]] @(
        0x30,0x0d, 0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01, 0x05,0x00)

    $pkDer = $cert.PublicKey.EncodedKeyValue.RawData

    # BIT STRING: tag 0x03, length, then the unused-bits byte and the key.
    $bitString = New-Object byte[] ($pkDer.Length + 1)
    $bitString[0] = 0x00
    [Array]::Copy($pkDer, 0, $bitString, 1, $pkDer.Length)

    $body = New-Object System.IO.MemoryStream
    $body.Write($rsaEncryption, 0, $rsaEncryption.Length)
    $body.WriteByte(0x03)               # BIT STRING
    Write-DerLength $body $bitString.Length
    $body.Write($bitString, 0, $bitString.Length)
    $bodyBytes = $body.ToArray()

    $spki = New-Object System.IO.MemoryStream
    $spki.WriteByte(0x30)
    Write-DerLength $spki $bodyBytes.Length
    $spki.Write($bodyBytes, 0, $bodyBytes.Length)
    $spkiBytes = $spki.ToArray()

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try { $spkiDigest = $sha256.ComputeHash($spkiBytes) }
    finally { $sha256.Dispose() }
    $spkiB64 = [Convert]::ToBase64String($spkiDigest)

    Set-Content -Path (Join-Path $BuildDir "wtserver_spki.b64") `
                -NoNewline -Value $spkiB64 -Encoding ASCII
    Write-Host "SPKI SHA-256 (base64): $spkiB64"
} catch {
    Write-Host "WARN: could not compute the certificate SPKI: $($_.Exception.Message)"
    Write-Host "WARN: Chrome will reject the WebTransport handshake (alert 46) unless the"
    Write-Host "WARN: certificate is trusted by the machine running the browser."
}

Write-Host "SHA1 hash: $hashHex"
Write-Host "Files written to $BuildDir"
Write-Host "Certificate stays in $CertStoreLocation until logout."
Write-Host "Done."
