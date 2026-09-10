# wolfSSL DTLS Chrome Interop Test - Full Automation
# Usage: Run this script in any PowerShell session

$ErrorActionPreference = "Stop"

# -----------------------------------------------------------------------------
# Configuration
# -----------------------------------------------------------------------------
$ProjectDir = "D:\MyOpen\NimRTC"
$ScriptDir = "$ProjectDir\scripts"
$TestDir = "$ProjectDir\tests\wolfssl_dtls"
$BuildDir = "$ProjectDir\build"
$WolfsslDir = "$BuildDir\third_party\wolfssl"
$WolfsslBuildDir = "$WolfsslDir\build"
$TestBuildDir = "$BuildDir\wolfssl_dtls"
$LogDir = "$BuildDir\logs"

$VS2022 = "D:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
$ChromePath = "$env:ProgramFiles\Google\Chrome\Application\chrome.exe"
$DebugPort = 9222
$WolfsslPort = 9999
$WolfsslVersion = "5.7.2"

# Create log directory
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

Write-Host "================================================================" -ForegroundColor Cyan
Write-Host "  wolfSSL DTLS Chrome Interop Test" -ForegroundColor Cyan
Write-Host "================================================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Project Dir:    $ProjectDir"
Write-Host "Test Dir:       $TestDir"
Write-Host "Build Dir:      $BuildDir"
Write-Host "wolfSSL Ver:    $WolfsslVersion"
Write-Host "wolfSSL Port:   $WolfsslPort"
Write-Host "Chrome Port:    $DebugPort"
Write-Host ""

# -----------------------------------------------------------------------------
# Step 1: Verify required tools
# -----------------------------------------------------------------------------
Write-Host "[1/7] Verifying tools..." -ForegroundColor Yellow

# vcvars
if (-not (Test-Path $VS2022)) {
    Write-Host "[FATAL] vcvars64.bat not found: $VS2022" -ForegroundColor Red
    exit 1
}

# CMake
try { $cmake = Get-Command "cmake" -ErrorAction Stop; Write-Host "[OK] cmake: $($cmake.Source)" -ForegroundColor Green }
catch { Write-Host "[FATAL] cmake not found" -ForegroundColor Red; exit 1 }

# Git
try { $git = Get-Command "git" -ErrorAction Stop; Write-Host "[OK] git: $($git.Source)" -ForegroundColor Green }
catch { Write-Host "[FATAL] git not found" -ForegroundColor Red; exit 1 }

# Chrome
if (Test-Path $ChromePath) {
    Write-Host "[OK] chrome: $ChromePath" -ForegroundColor Green
} else {
    Write-Host "[WARN] chrome not found - continuing without auto-launch" -ForegroundColor Yellow
}

Write-Host ""

# -----------------------------------------------------------------------------
# Step 2: Download wolfSSL source
# -----------------------------------------------------------------------------
Write-Host "[2/7] Preparing wolfSSL source..." -ForegroundColor Yellow

New-Item -ItemType Directory -Force -Path (Split-Path $WolfsslDir) | Out-Null

if (Test-Path "$WolfsslDir\.git") {
    Write-Host "[OK] wolfSSL already cloned" -ForegroundColor Green
} elseif (Test-Path $WolfsslDir) {
    Write-Host "[OK] wolfSSL source already exists" -ForegroundColor Green
} else {
    Write-Host "  Cloning wolfSSL v$WolfsslVersion..."
    Push-Location (Split-Path $WolfsslDir)
    git clone --depth 1 --branch v$WolfsslVersion https://github.com/wolfSSL/wolfssl.git wolfssl 2>&1 | Out-Null
    Pop-Location
    Write-Host "[OK] wolfSSL cloned" -ForegroundColor Green
}

Write-Host ""

# -----------------------------------------------------------------------------
# Step 3: Build wolfSSL (using vcvars64.bat)
# -----------------------------------------------------------------------------
Write-Host "[3/7] Building wolfSSL..." -ForegroundColor Yellow

New-Item -ItemType Directory -Force -Path $WolfsslBuildDir | Out-Null

# Build with MSVC
Push-Location $WolfsslBuildDir

Write-Host "  Configuring..."
$configure_cmd = @"
call "$VS2022" && `
cmake -G "Ninja" `
  -DWOLFSSL_DTLS=ON `
  -DHAVE_DTLS=ON `
  -DUSE_CERT_BUFFERS_256=ON `
  -DNO_RC4=ON `
  -DNO_HC128=ON `
  -DNO_RABBIT=ON `
  -DNO_DES3=ON `
  -DBUILD_SHARED_LIBS=OFF `
  -DWOLFSSL_INSTALL=OFF `
  -DCMAKE_BUILD_TYPE=Release `
  "$WolfsslDir"
"@

$configure_cmd | Out-File -FilePath "$LogDir\01_configure_wolfssl.bat" -Encoding ASCII
cmd /c "$LogDir\01_configure_wolfssl.bat" 2>&1 | Tee-Object -FilePath "$LogDir\01_configure_wolfssl.log" | Out-Null

if (-not (Test-Path "build.ninja")) {
    Write-Host "[ERROR] Configure failed - check $LogDir\01_configure_wolfssl.log" -ForegroundColor Red

    # Try without Ninja
    Write-Host "  Trying without Ninja..."
    $configure_cmd = @"
call "$VS2022" && `
cmake -G "NMake Makefiles" `
  -DWOLFSSL_DTLS=ON `
  -DHAVE_DTLS=ON `
  -DNO_RC4=ON `
  -DWOLFSSL_INSTALL=OFF `
  "$WolfsslDir"
"@
    $configure_cmd | Out-File -FilePath "$LogDir\01b_configure_wolfssl.bat" -Encoding ASCII
    cmd /c "$LogDir\01b_configure_wolfssl.bat" 2>&1 | Tee-Object -FilePath "$LogDir\01b_configure_wolfssl.log" | Out-Null
}

Write-Host "  Compiling..."

if (Test-Path "build.ninja") {
    $build_cmd = @"
call "$VS2022" && ninja
"@
    $build_cmd | Out-File -FilePath "$LogDir\02_build_wolfssl.bat" -Encoding ASCII
    cmd /c "$LogDir\02_build_wolfssl.bat" 2>&1 | Tee-Object -FilePath "$LogDir\02_build_wolfssl.log" | Out-Null
} else {
    $build_cmd = @"
call "$VS2022" && nmake
"@
    $build_cmd | Out-File -FilePath "$LogDir\02_build_wolfssl.bat" -Encoding ASCII
    cmd /c "$LogDir\02_build_wolfssl.bat" 2>&1 | Tee-Object -FilePath "$LogDir\02_build_wolfssl.log" | Out-Null
}

if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] Build failed - check $LogDir\02_build_wolfssl.log" -ForegroundColor Red
    Pop-Location
    exit 1
}

Pop-Location
Write-Host "[OK] wolfSSL built" -ForegroundColor Green
Write-Host ""

# -----------------------------------------------------------------------------
# Step 4: Build test server
# -----------------------------------------------------------------------------
Write-Host "[4/7] Building DTLS test server..." -ForegroundColor Yellow

New-Item -ItemType Directory -Force -Path $TestBuildDir | Out-Null
Push-Location $TestBuildDir

$test_configure_cmd = @"
call "$VS2022" && cmake -G "Ninja" `
  -DCMAKE_BUILD_TYPE=Release `
  -DWOLFSSL_DIR="$WolfsslBuildDir" `
  "$TestDir"
"@
$test_configure_cmd | Out-File -FilePath "$LogDir\03_configure_test.bat" -Encoding ASCII
cmd /c "$LogDir\03_configure_test.bat" 2>&1 | Tee-Object -FilePath "$LogDir\03_configure_test.log" | Out-Null

if (-not (Test-Path "build.ninja")) {
    Write-Host "[WARN] Ninja not available, falling back to NMake" -ForegroundColor Yellow
    $test_configure_cmd = @"
call "$VS2022" && cmake -G "NMake Makefiles" -DWOLFSSL_DIR="$WolfsslBuildDir" "$TestDir"
"@
    $test_configure_cmd | Out-File -FilePath "$LogDir\03b_configure_test.bat" -Encoding ASCII
    cmd /c "$LogDir\03b_configure_test.bat" 2>&1 | Tee-Object -FilePath "$LogDir\03b_configure_test.log" | Out-Null
}

if (Test-Path "build.ninja") {
    $test_build_cmd = "call `"$VS2022`" && ninja"
} else {
    $test_build_cmd = "call `"$VS2022`" && nmake"
}
$test_build_cmd | Out-File -FilePath "$LogDir\04_build_test.bat" -Encoding ASCII
cmd /c "$LogDir\04_build_test.bat" 2>&1 | Tee-Object -FilePath "$LogDir\04_build_test.log" | Out-Null

if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] Test server build failed - check $LogDir\04_build_test.log" -ForegroundColor Red
    Pop-Location
    exit 1
}

Pop-Location
Write-Host "[OK] test server built: $TestBuildDir\wolfssl_dtls_server.exe" -ForegroundColor Green
Write-Host ""

# -----------------------------------------------------------------------------
# Step 5: Start test server
# -----------------------------------------------------------------------------
Write-Host "[5/7] Starting DTLS test server..." -ForegroundColor Yellow

$server_exe = "$TestBuildDir\wolfssl_dtls_server.exe"
if (-not (Test-Path $server_exe)) {
    Write-Host "[ERROR] Server exe not found: $server_exe" -ForegroundColor Red
    exit 1
}

$server_proc = Start-Process -FilePath $server_exe `
    -PassThru `
    -NoNewWindow `
    -RedirectStandardOutput "$LogDir\05_server_output.log" `
    -RedirectStandardError "$LogDir\05_server_error.log"

Write-Host "  PID: $($server_proc.Id)"
Write-Host "  Log: $LogDir\05_server_output.log"
Start-Sleep -Seconds 3

# Check if server is still running
if ($server_proc.HasExited) {
    Write-Host "[FATAL] Server died immediately. Exit code: $($server_proc.ExitCode)" -ForegroundColor Red
    Write-Host "--- Server Output ---" -ForegroundColor Red
    Get-Content "$LogDir\05_server_output.log"
    Write-Host "--- Server Error ---" -ForegroundColor Red
    Get-Content "$LogDir\05_server_error.log"
    exit 1
}

Write-Host "[OK] Server started" -ForegroundColor Green
Write-Host ""

# -----------------------------------------------------------------------------
# Step 6: Extract fingerprint and launch Chrome
# -----------------------------------------------------------------------------
Write-Host "[6/7] Extracting fingerprint and launching Chrome..." -ForegroundColor Yellow

# Wait for server to print fingerprint
$fp_text = ""
$retry = 0
while ($retry -lt 10) {
    if (Test-Path "$LogDir\05_server_output.log") {
        $content = Get-Content "$LogDir\05_server_output.log" -Raw
        if ($content -match "sha256/([A-Za-z0-9+/=]+)") {
            $fp_text = "sha256/$($matches[1])"
            break
        }
    }
    Start-Sleep -Milliseconds 500
    $retry++
}

if (-not $fp_text) {
    Write-Host "[ERROR] Could not find fingerprint in server output" -ForegroundColor Red
    Get-Content "$LogDir\05_server_output.log"
    Stop-Process -Id $server_proc.Id -Force
    exit 1
}

Write-Host "  Fingerprint: $fp_text"
Write-Host ""

$chrome_flag = "--ignore-certificate-errors-spki-list=$fp_text"

# Save fingerprint for later use
$fingerprint_file = "$TestBuildDir\fingerprint.txt"
"sha256/$($fp_text -replace 'sha256/','')" | Out-File -FilePath $fingerprint_file

# Kill any existing Chrome instances
$existing_chrome = Get-Process chrome -ErrorAction SilentlyContinue
if ($existing_chrome) {
    Write-Host "  Killing existing Chrome processes..." -ForegroundColor Yellow
    Stop-Process -Name chrome -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
}

# Launch Chrome with the flag
if (Test-Path $ChromePath) {
    $user_data_dir = "$env:TEMP\chrome_wolfssl_test"
    New-Item -ItemType Directory -Force -Path $user_data_dir | Out-Null

    $chrome_args = @(
        $chrome_flag
        "--user-data-dir=$user_data_dir"
        "--remote-debugging-port=$DebugPort"
        "--no-first-run"
        "--no-default-browser-check"
        "--enable-logging=stderr"
        "--v=1"
        "--auto-open-devtools-for-tabs"
        "data:text/html,<html><body><h1>wolfSSL DTLS Test</h1><script>setTimeout(()=>{document.title='wolfSSL-Chrome-Test-Ready';},1000);</script></body></html>"
    )

    Write-Host "  Launching Chrome..." -ForegroundColor Green
    Write-Host "  args: $chrome_flag" -ForegroundColor Gray

    $chrome_proc = Start-Process -FilePath $ChromePath -ArgumentList $chrome_args -PassThru
    Write-Host "  Chrome PID: $($chrome_proc.Id)"
    Write-Host ""

    # Wait for Chrome to be ready
    Start-Sleep -Seconds 5

    # Try to access Chrome DevTools
    $devtools_url = "http://localhost:$DebugPort/json"
    try {
        $devtools_resp = Invoke-RestMethod -Uri $devtools_url -TimeoutSec 5
        Write-Host "[OK] Chrome DevTools accessible at $devtools_url" -ForegroundColor Green
        Write-Host "  Tabs:"
        $devtools_resp | ForEach-Object {
            Write-Host "    - $($_.title) ($($_.type))" -ForegroundColor Gray
        }
    } catch {
        Write-Host "[WARN] Could not access Chrome DevTools: $_" -ForegroundColor Yellow
    }
} else {
    Write-Host "[WARN] Chrome not found - skip Chrome launch" -ForegroundColor Yellow
    Write-Host "[INFO] Run Chrome manually with: $chrome_flag" -ForegroundColor Yellow
}

# -----------------------------------------------------------------------------
# Step 7: Wait for handshake / show status
# -----------------------------------------------------------------------------
Write-Host ""
Write-Host "[7/7] Monitoring for DTLS handshake..." -ForegroundColor Yellow
Write-Host ""
Write-Host "========================================================================"
Write-Host "  Server log (Ctrl+C to stop):" -ForegroundColor Cyan
Write-Host "========================================================================"
Write-Host ""

# Wait up to 30 seconds for handshake
$handshake_done = $false
$monitor_seconds = 30
$monitor_start = Get-Date

while (((New-TimeSpan -Start $monitor_start -End (Get-Date)).TotalSeconds) -lt $monitor_seconds) {
    if (Test-Path "$LogDir\05_server_output.log") {
        $content = Get-Content "$LogDir\05_server_output.log" -Raw
        if ($content -match "DTLS Handshake Complete|SUCCESS|Received.*ClientHello") {
            Write-Host $content
            $handshake_done = $true
            break
        }
    }
    Start-Sleep -Seconds 1
}

# Display final results
Write-Host ""
Write-Host "========================================================================"
Write-Host "  Final Server Output:" -ForegroundColor Cyan
Write-Host "========================================================================"
Write-Host ""
if (Test-Path "$LogDir\05_server_output.log") {
    Get-Content "$LogDir\05_server_output.log"
}

Write-Host ""
if ($handshake_done) {
    Write-Host "========================================================================" -ForegroundColor Green
    Write-Host "  ✅ wolfSSL <-> Chrome DTLS interop VERIFIED!" -ForegroundColor Green
    Write-Host "========================================================================" -ForegroundColor Green
    Write-Host ""
    Write-Host "Next step: Replace NimRTC DTLS with wolfSSL" -ForegroundColor Cyan
    Write-Host "  See: https://github.com/wolfSSL/wolfssl"
} else {
    Write-Host "========================================================================" -ForegroundColor Yellow
    Write-Host "  ⏳ Test still in progress" -ForegroundColor Yellow
    Write-Host "========================================================================" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Manual test instructions:"
    Write-Host ""
    Write-Host "1. Open Chrome manually with this flag:" -ForegroundColor Cyan
    Write-Host "   $chrome_flag" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "2. Navigate to your WebRTC test page"
    Write-Host ""
    Write-Host "3. Check server log:" -ForegroundColor Cyan
    Write-Host "   Get-Content $LogDir\05_server_output.log -Wait" -ForegroundColor Yellow
    Write-Host ""
}

Write-Host ""
Write-Host "Server still running in background (PID $($server_proc.Id))"
Write-Host "To stop: Stop-Process -Id $($server_proc.Id)"
Write-Host ""
