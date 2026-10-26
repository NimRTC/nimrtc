# Install the NimRTC WebTransport test CA and its leaf certificate into the
# Windows certificate stores.
#
# Why this exists
# ---------------
# Chrome does not relax the certificate check for WebTransport / QUIC.
# Both `--ignore-certificate-errors` and
# `--ignore-certificate-errors-spki-list` are honoured for classic TLS but
# *not* for the QUIC handshake, so Chrome rejects a self-signed
# demo-wtserver certificate with
# `QUIC_TLS_CERTIFICATE_UNKNOWN (46: certificate unknown)`.
#
# The browser only accepts the server when its certificate chains up to a
# root it already trusts.  This script therefore installs:
#
#   * the CA certificate  -> Cert:\CurrentUser\Root   (browser trust anchor)
#   * the leaf certificate -> Cert:\CurrentUser\My     (msquic HASH_STORE)
#
# CAUTION (measured 2026-10-26, Chrome 155): the root-store install is
# necessary but NOT sufficient for the demo-wtserver.  With the CA installed
# Chrome accepts the leaf over classic TLS, and over QUIC when an
# OpenSSL-based QUIC server presents it -- but the msquic/SChannel listener
# presents the byte-identical leaf (no chain) and Chrome still aborts the
# QUIC handshake with `QUIC_TLS_CERTIFICATE_UNKNOWN (46: certificate
# unknown)` / `CERTIFICATE_VERIFY_FAILED`.  That residual failure is in the
# SChannel TLS presentation, not in the trust store; see
# `scripts/generate_wtserver_ca_cert.py` and the `[Unreleased]` section
# of `CHANGELOG.md` ("The Chrome e2e Case E WebTransport gate is still
# blocked...").
#
# Scope: the *current user* personal store plus the system Root store.
#
# NOTE on the Root store: on Windows 8 and newer the "Root" system store is
# shared between the per-user and the per-machine view -- `Cert:\CurrentUser\
# Root` enumerates the same physical store as `Cert:\LocalMachine\Root`,
# and entries added to it are persisted machine-wide.  Removing such an
# entry therefore needs machine privileges, which is why the removal path
# below goes through `certutil -delstore Root` instead of
# `Remove-Item Cert:\...` (the latter pops an interactive confirmation
# dialog that fails inside a non-interactive shell).
#
# Usage:
#   powershell -File .\scripts\install_wtserver_ca.ps1
#   powershell -File .\scripts\install_wtserver_ca.ps1 -CleanupOld
#   powershell -File .\scripts\install_wtserver_ca.ps1 -Uninstall
#
# NOTE: this script avoids [CmdletBinding()] / param() on purpose -- the
# PowerShell 5.1 parser sometimes chokes on [CmdletBinding()] when a .ps1
# is loaded with -File (see generate_wtserver_cert.ps1 for the details).

$ErrorActionPreference = "Stop"

# --- Parse args (manual, no CmdletBinding) ---
$Uninstall = $false
$CleanupOld = $false
$BuildDir = ""

for ($i = 0; $i -lt $args.Count; $i++) {
    switch ($args[$i]) {
        "-Uninstall"  { $Uninstall = $true }
        "-CleanupOld" { $CleanupOld = $true }
        "-BuildDir"   { $BuildDir = $args[++$i] }
        default { }
    }
}

if (-not $PSScriptRoot) {
    $PSScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
}
$ProjectRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) {
    $BuildDir = Join-Path $ProjectRoot "build"
}

$CaPem    = Join-Path $BuildDir "wtserver_ca_cert.pem"
$CaCer    = Join-Path $BuildDir "wtserver_ca_cert.cer"
$CaPfx    = Join-Path $BuildDir "wtserver_ca.pfx"
$LeafPfx  = Join-Path $BuildDir "wtserver_cert.pfx"
$PfxPassword = "nimrtc-wtserver"

function Get-NimRtcLeafThumbprint {
    $hashFile = Join-Path $BuildDir "wtserver_hash.txt"
    if (Test-Path $hashFile) {
        return (Get-Content -Raw $hashFile).Trim().ToUpperInvariant()
    }
    return $null
}

# Remove a certificate from a store.  The Root store refuses the plain
# Remove-Item path ("no user interface operation" / access denied) because
# it wants an interactive confirmation, so use certutil there.
function Remove-NimRtcCertificate {
    param(
        [Parameter(Mandatory = $true)][string]$Thumbprint,
        [Parameter(Mandatory = $true)][string]$Store  # "Root" or "My"
    )
    $certutil = "certutil"
    if ($Store -eq "Root") {
        & $certutil -delstore Root $Thumbprint 2>&1 | Out-Null
        return ($LASTEXITCODE -eq 0)
    }
    $cert = Get-ChildItem "Cert:\CurrentUser\My" -ErrorAction SilentlyContinue |
        Where-Object { $_.Thumbprint -eq $Thumbprint }
    foreach ($c in $cert) {
        Remove-Item $c.PSPath -Force -ErrorAction SilentlyContinue
    }
    return $true
}

# ---------------------------------------------------------------- uninstall
if ($Uninstall) {
    $removed = 0
    $targets = @()
    foreach ($store in @("Cert:\CurrentUser\Root", "Cert:\CurrentUser\My")) {
        $targets += Get-ChildItem $store -ErrorAction SilentlyContinue |
            Where-Object { $_.Subject -match "NimRTC Dev Root CA" -or
                           $_.Subject -eq "CN=localhost" }
    }
    foreach ($c in $targets) {
        $storeName = if ($c.PSPath -like "*\Root\*") { "Root" } else { "My" }
        if (Remove-NimRtcCertificate -Thumbprint $c.Thumbprint -Store $storeName) {
            Write-Host "removed '$($c.Subject)' ($($c.Thumbprint)) from $storeName"
            $removed++
        } else {
            Write-Host "FAILED to remove '$($c.Subject)' ($($c.Thumbprint)) from $storeName"
        }
    }
    Write-Host "Uninstall done ($removed certificate(s) removed)."
    Write-Host "Chrome must be restarted for the trust change to take effect."
    exit 0
}

# ------------------------------------------------------------------ install
if (-not (Test-Path $LeafPfx)) {
    Write-Error "Missing $LeafPfx -- run scripts/generate_wtserver_ca_cert.py first."
    exit 1
}
if (-not (Test-Path $CaPem)) {
    Write-Error "Missing $CaPem -- run scripts/generate_wtserver_ca_cert.py first."
    exit 1
}

# 1) leaf certificate + private key -> Cert:\CurrentUser\My
$leaf = Import-PfxCertificate -FilePath $LeafPfx -CertStoreLocation `
        "Cert:\CurrentUser\My" -Password (ConvertTo-SecureString `
        -String $PfxPassword -AsPlainText -Force)
Write-Host "leaf installed in Cert:\CurrentUser\My (thumbprint $($leaf.Thumbprint))"

# 1b) make sure the thumbprint files describe the leaf we just installed.
#     msquic looks the certificate up by the SHA-1 hash in
#     build/wtserver_hash.txt, so a stale hash file makes the server load
#     the wrong certificate (or none at all) even though the install
#     succeeded.  Regenerate both files from the installed leaf.
$hashFile = Join-Path $BuildDir "wtserver_hash.txt"
$thumbFile = Join-Path $BuildDir "wtserver_thumbprint.txt"
$expected = Get-NimRtcLeafThumbprint
if ($expected -ne $leaf.Thumbprint) {
    Write-Host "NOTE: wtserver_hash.txt described '$expected' but the installed"
    Write-Host "      leaf is '$($leaf.Thumbprint)' -- rewriting both files."
}
Set-Content -Path $hashFile -Value $leaf.Thumbprint.ToLowerInvariant() -NoNewline
Set-Content -Path $thumbFile -Value $leaf.Thumbprint.ToUpperInvariant() -NoNewline

# 2) CA certificate -> the Root (trusted) store.
#    Two things matter here:
#      * Import-Certificate is picky about the input format, so convert the
#        PEM to DER first.
#      * Import-Certificate / the Cert: provider show an interactive
#        "security warning" dialog when a certificate is added to the Root
#        store.  In a non-interactive shell that dialog never gets answered
#        and the script hangs until it is killed.  `certutil -addstore`
#        writes the store through CAPI without any UI, so use that.
$raw = Get-Content -Raw $CaPem
$b64 = ($raw -split "`r?`n" | Where-Object { $_ -notmatch "-----" }) -join ""
$der = [Convert]::FromBase64String($b64.Trim())
[System.IO.File]::WriteAllBytes($CaCer, $der)
$sha1 = [System.Security.Cryptography.SHA1]::Create()
$caThumbprint = ([BitConverter]::ToString(
    $sha1.ComputeHash($der))).Replace("-", "")
& certutil -addstore Root $CaCer 2>&1 | Out-Null
$ca = Get-ChildItem "Cert:\CurrentUser\Root" -ErrorAction SilentlyContinue |
    Where-Object { $_.Thumbprint -eq $caThumbprint }
if (-not $ca) {
    $ca = Get-ChildItem "Cert:\CurrentUser\Root" -ErrorAction SilentlyContinue |
        Where-Object { $_.Subject -match "NimRTC Dev Root CA" } |
        Select-Object -First 1
}
if (-not $ca) {
    Write-Error "Could not install the CA certificate into the Root store."
    exit 1
}
Write-Host "CA installed in Cert:\CurrentUser\Root (thumbprint $($ca.Thumbprint))"

# 3) keep the CA private key around so new leaves can be issued later
if (Test-Path $CaPfx) {
    Import-PfxCertificate -FilePath $CaPfx -CertStoreLocation `
        "Cert:\CurrentUser\My" -Password (ConvertTo-SecureString `
        -String $PfxPassword -AsPlainText -Force) | Out-Null
    Write-Host "CA key kept in Cert:\CurrentUser\My"
}

# 4) optional cleanup of stale self-signed localhost certificates
if ($CleanupOld) {
    $keep = $leaf.Thumbprint
    Get-ChildItem "Cert:\CurrentUser\My" |
        Where-Object { $_.Subject -eq "CN=localhost" -and
                       $_.Thumbprint -ne $keep } |
        ForEach-Object {
            Remove-Item $_.PSPath -Force -ErrorAction SilentlyContinue
            Write-Host "removed stale '$($_.Subject)' ($($_.Thumbprint))"
        }
}

Write-Host ""
Write-Host "Done. Chrome now trusts the chain for https://localhost:9443."
Write-Host "Restart any running Chrome before re-running the Case E harness."
Write-Host "To undo: powershell -File .\scripts\install_wtserver_ca.ps1 -Uninstall"
