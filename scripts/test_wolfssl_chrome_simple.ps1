# WolfSSL DTLS Chrome Interop Test - Simple Version
# Focus: Get wolfSSL working, then Chrome + wolfSSL interop

$ErrorActionPreference = "Continue"

$ProjectDir = "D:\MyOpen\NimRTC"
$BuildRoot = "$ProjectDir\build"
$WolfsslDir = "$BuildRoot\wolfssl"
$WolfsslBuildDir = "$WolfsslDir\build"
$TestDir = "$ProjectDir\tests\wolfssl_dtls"
$TestBuildDir = "$BuildRoot\wolfssl_dtls_test"
$LogRoot = "$BuildRoot\logs"
$VCVARS = "D:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
$Chrome = "C:\Program Files\Google\Chrome\Application\chrome.exe"

New-Item -ItemType Directory -Force -Path $LogRoot -ErrorAction SilentlyContinue | Out-Null

function Run-In-MSVC {
    param([string]$Command, [string]$LogFile)
    $tmpfile = "$LogRoot\_tmp.bat"
    @"
call "$VCVARS" >nul 2>&1
$Command
"@ | Out-File -FilePath $tmpfile -Encoding ASCII -Force
    cmd /c $tmpfile 2>&1 | Tee-Object -FilePath $LogFile | Out-Null
    return $LASTEXITCODE
}

Write-Host "`n[1/6] Verify tools" -ForegroundColor Yellow
$cl_check = Run-In-MSVC "where cl" "$LogRoot\01_cl.log"
if ($cl_check -ne 0) {
    Write-Host "  [FAIL] cl not found" -ForegroundColor Red
    exit 1
}
Write-Host "  [OK] cl available" -ForegroundColor Yellow

Write-Host "`n[2/6] Clone wolfSSL v5.7.2" -ForegroundColor Yellow
if (-not (Test-Path "$WolfsslDir\configure.ac")) {
    if (-not (Test-Path "$WolfsslDir")) {
        Push-Location $BuildRoot
        Write-Host "  Cloning..."
        git clone --depth=1 --branch=v5.7.2 https://github.com/wolfSSL/wolfssl.git wolfssl 2>&1 | Tee-Object "$LogRoot\02_clone.log" | Out-Null
        Pop-Location
    }
}
if (Test-Path "$WolfsslDir\configure.ac") {
    Write-Host "  [OK] wolfSSL source ready" -ForegroundColor Yellow
} else {
    Write-Host "  [FAIL] wolfSSL source missing" -ForegroundColor Red
    exit 1
}

Write-Host "`n[3/6] Build wolfSSL" -ForegroundColor Yellow
New-Item -ItemType Directory -Force -Path $WolfsslBuildDir | Out-Null
Push-Location $WolfsslBuildDir

# Configure (try NMake for simplicity)
Run-In-MSVC "cmake -G `"NMake Makefiles`" -DWOLFSSL_DTLS=ON -DHAVE_DTLS=ON -DNO_RC4=ON $WolfsslDir" "$LogRoot\03_config.log"
Write-Host "  Configured"

# Build
Run-In-MSVC "nmake" "$LogRoot\04_build.log"

if (Test-Path "$WolfsslBuildDir\wolfssl.lib") {
    Write-Host "  [OK] wolfssl.lib built" -ForegroundColor Yellow
} else {
    Write-Host "  [FAIL] wolfssl.lib not found" -ForegroundColor Red
    Pop-Location
    exit 1
}
Pop-Location

Write-Host "`n[4/6] Build test server" -ForegroundColor Yellow
New-Item -ItemType Directory -Force -Path $TestBuildDir | Out-Null
Push-Location $TestBuildDir

Run-In-MSVC "cmake -G `"NMake Makefiles`" -DWOLFSSL_DIR=`"$WolfsslBuildDir`" $TestDir" "$LogRoot\05_config_test.log"
Run-In-MSVC "nmake" "$LogRoot\06_build_test.log"

if (Test-Path "$TestBuildDir\wolfssl_dtls_server.exe") {
    Write-Host "  [OK] server built" -ForegroundColor Yellow
} else {
    Write-Host "  [FAIL] server build failed" -ForegroundColor Red
    Pop-Location
    exit 1
}
Pop-Location

Write-Host "`n[5/6] Run server and get fingerprint" -ForegroundColor Yellow
$server_log = "$LogRoot\07_server.log"
$server_proc = Start-Process "$TestBuildDir\wolfssl_dtls_server.exe" `
    -PassThru `
    -NoNewWindow `
    -RedirectStandardOutput $server_log `
    -RedirectStandardError "$LogRoot\07_server.err"

Start-Sleep -Seconds 3

# Extract fingerprint
$fingerprint = $null
if (Test-Path $server_log) {
    $content = Get-Content $server_log -Raw
    if ($content -match "sha256/([A-Za-z0-9+/=]+)") {
        $fingerprint = $matches[1]
    }
}

if (-not $fingerprint) {
    Write-Host "  [FAIL] Could not extract fingerprint" -ForegroundColor Red
    Get-Content $server_log
    Stop-Process -Id $server_proc.Id -Force -ErrorAction SilentlyContinue
    exit 1
}

Write-Host "  Fingerprint: sha256/$fingerprint" -ForegroundColor Green

Write-Host "`n[6/6] Launch Chrome with fingerprint" -ForegroundColor Yellow

# Stop existing chrome
Get-Process chrome -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

# Start chrome
$chrome_flag = "--ignore-certificate-errors-spki-list=sha256/$fingerprint"
$user_data = "$env:TEMP\chrome_wolfssl_$([guid]::NewGuid().ToString('N').Substring(0,8))"
$chrome_args = @(
    $chrome_flag
    "--user-data-dir=`"$user_data`""
    "--remote-debugging-port=9222"
    "--no-first-run"
    "--no-default-browser-check"
    "--new-window"
    "about:blank"
)

Write-Host "  Launching Chrome..." -ForegroundColor Green
Write-Host "  Flag: $chrome_flag" -ForegroundColor Gray

$chrome_proc = Start-Process -FilePath $Chrome -ArgumentList $chrome_args -PassThru
Start-Sleep -Seconds 3

if (Test-Path $server_log) {
    $content = Get-Content $server_log -Raw
    Write-Host "`nServer log so far:" -ForegroundColor Cyan
    Write-Host ("=" * 70)
    Write-Host $content
    Write-Host ("=" * 70)
}

Write-Host "`nTest setup complete:" -ForegroundColor Cyan
Write-Host "  Server PID: $($server_proc.Id)"
Write-Host "  Chrome PID: $($chrome_proc.Id)"
Write-Host "  Server Log: $server_log"
Write-Host "  Fingerprint file: $TestBuildDir\fingerprint.txt"

# Save fingerprint
"sha256/$fingerprint" | Out-File -FilePath "$TestBuildDir\fingerprint.txt" -Encoding ASCII

Write-Host "`nTo monitor:" -ForegroundColor Cyan
Write-Host "  Get-Content '$server_log' -Wait" -ForegroundColor Yellow

Write-Host "`nTo stop:" -ForegroundColor Cyan
Write-Host "  Stop-Process -Id $($server_proc.Id),$($chrome_proc.Id)" -ForegroundColor Yellow

# Don't kill - let user control
exit 0
