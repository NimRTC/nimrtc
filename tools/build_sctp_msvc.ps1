#!/bin/bash
# Build the SCTP module with MSVC on Windows, to check the in-flight
# callback guard compiles under the primary dev toolchain too.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/build_sctp_msvc.ps1
set -u
$ErrorActionPreference = 'Stop'

$root = 'D:\MyOpen\NimRTC'
$build = Join-Path $root 'build\msvc-sctp'
$vsw = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vsw -latest -products * -property installationPath

$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found under $vs" }

$bat = Join-Path $env:TEMP 'nimrtc_build_sctp_msvc.bat'
@"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b 1
cd /d "$root"
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DNIMRTC_BUILD_TESTS=OFF -DNIMRTC_BUILD_EXAMPLES=OFF -DNIMRTC_VENDORED_WEBRTC_APM=OFF || exit /b 1
cmake --build "$build" --target nimrtc_sctp || exit /b 1
echo MSVC_BUILD_OK
"@ | Set-Content -Path $bat -Encoding ASCII

cmd /c $bat
exit $LASTEXITCODE
