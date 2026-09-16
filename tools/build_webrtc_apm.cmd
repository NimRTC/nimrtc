@echo off
REM Build WebRTC Audio Processing Module (APM) on Windows using MSVC + Meson.
REM
REM Prerequisites:
REM   - Visual Studio 2022 with C++ workload (any edition)
REM   - Python 3.9+ with meson and ninja:
REM       pip install --user meson ninja
REM
REM Usage:
REM   tools\build_webrtc_apm.cmd
REM   tools\build_webrtc_apm.cmd --debug
REM   tools\build_webrtc_apm.cmd --clean
REM
REM Exit codes:
REM   0  success
REM   1  prerequisite missing (meson, ninja, MSVC)
REM   2  meson setup failed
REM   3  ninja build failed
REM
REM This script is the canonical Windows build entry point; the legacy
REM copy at build\build_webrtc_apm.cmd is gitignored and will be removed.

setlocal enabledelayedexpansion

REM ---------------------------------------------------------------------------
REM Resolve project root relative to this script (no hard-coded absolute paths).
REM   tools/build_webrtc_apm.cmd lives two directories under the project root.
REM ---------------------------------------------------------------------------
set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "PROJECT_ROOT=%%~fI"

set "VENDOR_DIR=%PROJECT_ROOT%\src\third_party\webrtc_audio_processing"
set "SRC_DIR=%VENDOR_DIR%\src"
set "BUILD_DIR=%VENDOR_DIR%\build_meson"
set "LOG_DIR=%PROJECT_ROOT%\build"

set "BUILD_TYPE=release"

REM ---------------------------------------------------------------------------
REM Locate vcvars — search the standard Visual Studio install paths.
REM ---------------------------------------------------------------------------
set "VCVARS_BAT="
for %%E in (Community Professional Enterprise BuildTools) do (
    if exist "C:\Program Files\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS_BAT=C:\Program Files\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat"
        goto :vcvars_found
    )
    if exist "D:\Program Files\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS_BAT=D:\Program Files\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat"
        goto :vcvars_found
    )
)

:vcvars_not_found
echo [ERROR] Could not locate vcvars64.bat under C:\ or D:\Program Files\Microsoft Visual Studio\2022.
echo         Install Visual Studio 2022 with the C++ workload, or set VCVARS_BAT env var manually.
exit /b 1

:vcvars_found
echo [INFO] Using vcvars: %VCVARS_BAT%
call "%VCVARS_BAT%" >nul
if errorlevel 1 (
    echo [ERROR] Failed to source vcvars64.bat
    exit /b 1
)

REM ---------------------------------------------------------------------------
REM Parse CLI flags
REM ---------------------------------------------------------------------------
for %%A in (%*) do (
    if "%%A"=="--debug" set "BUILD_TYPE=debug"
    if "%%A"=="--clean" (
        echo [INFO] --clean: removing %BUILD_DIR%
        if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    )
    if "%%A"=="--help" goto :print_usage
    if "%%A"=="-h"     goto :print_usage
)

echo [INFO] PROJECT_ROOT: %PROJECT_ROOT%
echo [INFO] VENDOR_DIR:   %VENDOR_DIR%
echo [INFO] SRC_DIR:      %SRC_DIR%
echo [INFO] BUILD_DIR:    %BUILD_DIR%
echo [INFO] BUILD_TYPE:   %BUILD_TYPE%
echo.

REM ---------------------------------------------------------------------------
REM Prerequisite checks
REM ---------------------------------------------------------------------------
where meson >nul 2>&1 || (
    echo [ERROR] meson not found. Install: pip install --user meson
    exit /b 1
)
where ninja >nul 2>&1 || (
    echo [ERROR] ninja not found. Install: pip install --user ninja
    exit /b 1
)

echo [INFO] meson:  !meson --version!
echo [INFO] ninja:  !ninja --version!

REM ---------------------------------------------------------------------------
REM Step 1: meson setup
REM ---------------------------------------------------------------------------
if not exist "%BUILD_DIR%\build.ninja" (
    echo.
    echo [STEP 1] meson setup...
    if not exist "%SRC_DIR%\meson.build" (
        echo [ERROR] %SRC_DIR%\meson.build not found.
        echo         Run tools\fetch_webrtc_apm.py first to clone the source.
        exit /b 1
    )
    meson setup "%BUILD_DIR%" "%SRC_DIR%" --buildtype=%BUILD_TYPE% --default-library=static
    if errorlevel 1 (
        echo [ERROR] meson setup failed
        exit /b 2
    )
) else (
    echo [STEP 1] build.ninja already exists (run with --clean to reconfigure)
)

REM ---------------------------------------------------------------------------
REM Step 2: ninja build
REM ---------------------------------------------------------------------------
if not exist "%LOG_DIR%" mkdir "%LOG_DIR%"
echo.
echo [STEP 2] ninja build (logging to %LOG_DIR%\webrtc_apm_build.log)...
ninja -C "%BUILD_DIR%" > "%LOG_DIR%\webrtc_apm_build.log" 2>&1
set NINJA_RC=!ERRORLEVEL!

if !NINJA_RC! neq 0 (
    echo [ERROR] ninja build failed with rc=!NINJA_RC!
    echo Full log: %LOG_DIR%\webrtc_apm_build.log  (last 250 lines shown below)
    powershell -NoProfile -Command "Get-Content '%LOG_DIR%\webrtc_apm_build.log' -Tail 250"
    exit /b 3
)

echo.
echo [OK] WebRTC APM built successfully.
echo Library: %BUILD_DIR%\webrtc\modules\audio_processing\libwebrtc-audio-processing-2.a
for %%F in ("%BUILD_DIR%\webrtc\modules\audio_processing\libwebrtc-audio-processing-2.a") do (
    echo Size:   %%~zF bytes
)
exit /b 0

:print_usage
echo Usage:
echo   tools\build_webrtc_apm.cmd            # release build (default)
echo   tools\build_webrtc_apm.cmd --debug   # debug build
echo   tools\build_webrtc_apm.cmd --clean   # wipe build_meson first
exit /b 0
