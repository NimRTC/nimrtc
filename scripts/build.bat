@echo off
setlocal EnableDelayedExpansion

:: scripts/build.bat
:: Interactive build script for NimRTC on Windows.
:: Detects MSVC toolchain, configures CMake, builds, and optionally runs tests.
::
:: Usage:
::   .\scripts\build.bat            -- full build + tests
::   .\scripts\build.bat --configure  -- CMake configure only
::   .\scripts\build.bat --build      -- build only (no configure)
::   .\scripts\build.bat --test       -- run tests only
::   .\scripts\build.bat --rebuild    -- clean and rebuild everything

set "SCRIPT_DIR=%~dp0"
set "PROJECT_ROOT=%SCRIPT_DIR%.."

:: Parse arguments
set "MODE=full"
for %%A in (%*) do (
    if "%%A"=="--configure"  set "MODE=configure"
    if "%%A"=="--build"      set "MODE=build"
    if "%%A"=="--test"       set "MODE=test"
    if "%%A"=="--rebuild"    set "MODE=rebuild"
    if "%%A"=="--release"    set "BUILD_TYPE=Release"
    if "%%A"=="--debug"      set "BUILD_TYPE=Debug"
    if "%%A"=="-h"           set "MODE=help"
    if "%%A"=="--help"       set "MODE=help"
)

if "%MODE%"=="help" (
    echo NimRTC Build Script
    echo.
    echo Usage:
    echo   .\scripts\build.bat           -- configure + build
    echo   .\scripts\build.bat --configure -- CMake configure only
    echo   .\scripts\build.bat --build     -- build only (no configure)
    echo   .\scripts\build.bat --test      -- run tests only
    echo   .\scripts\build.bat --rebuild   -- clean and rebuild
    echo   .\scripts\build.bat --release   -- use Release config (default: Debug)
    echo   .\scripts\build.bat --debug     -- use Debug config
    exit /b 0
)

:: ---------------------------------------------------------------------------
:: Step 0: Check prerequisites
:: ---------------------------------------------------------------------------
echo.
echo ============================================================
echo  NimRTC Build  [Windows]
echo ============================================================
echo.

python "%PROJECT_ROOT%\tools\check_prerequisites.py" --compiler >nul 2>&1
if errorlevel 1 (
    echo [WARN] Some prerequisites may be missing.
    echo         Run:  python tools\check_prerequisites.py
    echo.
)

:: ---------------------------------------------------------------------------
:: Step 1: Detect MSVC toolchain
:: ---------------------------------------------------------------------------
echo [INFO] Detecting MSVC toolchain...

:: Check if cl.exe is in PATH (means we are in a VS dev prompt)
where cl >nul 2>&1
if errorlevel 1 (
    echo.
    echo [ERROR] cl.exe not found in PATH.
    echo         This script must be run from a Visual Studio Developer Prompt.
    echo.
    echo         Open one of these:
    echo           - "x64 Native Tools Command Prompt for VS 2022"
    echo           - "x86_x64 Cross Tools Command Prompt for VS 2022"
    echo.
    echo         Or run from a regular CMD after running vcvarsall.bat:
    echo           "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
    echo.
    set ERROR_MSVC=1
    goto :usage_error
)
goto :msvc_ok

:usage_error
echo.
echo To continue without the Developer Prompt, use cmake directly:
echo   cmake --preset dev.msvc
echo   cmake --build build --config Debug -j
exit /b 1

:msvc_ok
echo [OK]  MSVC toolchain detected.

:: ---------------------------------------------------------------------------
:: Step 2: Choose build configuration
:: ---------------------------------------------------------------------------
if not defined BUILD_TYPE (
    echo.
    echo Build configuration:
    echo   [1] Debug     (default, full symbols, slower)
    echo   [2] Release   (optimised, faster binaries)
    echo.
    set /p BUILD_CHOICE="Select [1/2] (default 1): "
    if "!BUILD_CHOICE!"=="2" (
        set "BUILD_TYPE=Release"
    ) else (
        set "BUILD_TYPE=Debug"
    )
)

if not defined BUILD_TYPE set "BUILD_TYPE=Debug"

echo.
echo Using: %BUILD_TYPE%

:: ---------------------------------------------------------------------------
:: Step 3: Build type handling
:: ---------------------------------------------------------------------------
if "%MODE%"=="rebuild" (
    echo.
    echo [INFO] Rebuild requested -- removing build directory...
    if exist "%PROJECT_ROOT%\build" (
        rmdir /s /q "%PROJECT_ROOT%\build"
        echo [OK]  Build directory removed.
    )
    set "MODE=configure"
)

:: ---------------------------------------------------------------------------
:: Step 4: CMake configure
:: ---------------------------------------------------------------------------
if "%MODE%"=="configure" or "%MODE%"=="full" (
    echo.
    echo ============================================================
    echo  Configuring NimRTC (preset: dev.msvc, %BUILD_TYPE%)...
    echo ============================================================
    echo.
    cd /d "%PROJECT_ROOT%"
    cmake --preset dev.msvc -DCMAKE_BUILD_TYPE=%BUILD_TYPE%
    if errorlevel 1 (
        echo.
        echo [ERROR] CMake configure failed. See errors above.
        echo.
        echo Common fixes:
        echo   1. Run in VS 2022 Developer Prompt (x64)
        echo   2. Check: python tools\check_prerequisites.py
        echo   3. Clean: rmdir /s /q build  (then re-run this script)
        exit /b 1
    )
    echo.
    echo [OK]  CMake configure succeeded.
)

:: ---------------------------------------------------------------------------
:: Step 5: Build
:: ---------------------------------------------------------------------------
if "%MODE%"=="build" or "%MODE%"=="full" (
    echo.
    echo ============================================================
    echo  Building NimRTC (%BUILD_TYPE%)...
    echo ============================================================
    echo.
    cd /d "%PROJECT_ROOT%"
    cmake --build build --config %BUILD_TYPE% -j
    if errorlevel 1 (
        echo.
        echo [ERROR] Build failed. See errors above.
        exit /b 1
    )
    echo.
    echo [OK]  Build succeeded.
)

:: ---------------------------------------------------------------------------
:: Step 6: Run tests (optional)
:: ---------------------------------------------------------------------------
if "%MODE%"=="test" or "%MODE%"=="full" (
    :: For full build, ask; for --test mode, run automatically
    if "%MODE%"=="full" (
        set /p RUN_TESTS="Run tests now? [Y/n]: "
        if /i "!RUN_TESTS!"=="n" goto :skip_tests
    )

    echo.
    echo ============================================================
    echo  Running tests (%BUILD_TYPE%)...
    echo ============================================================
    echo.
    cd /d "%PROJECT_ROOT%"
    ctest --preset tests.msvc -C %BUILD_TYPE% --output-on-failure
    if errorlevel 1 (
        echo.
        echo [WARN] Some tests failed. Review output above.
    ) else (
        echo.
        echo [OK]  All tests passed.
    )
    :skip_tests
)

:: ---------------------------------------------------------------------------
:: Done
:: ---------------------------------------------------------------------------
echo.
echo ============================================================
echo  Done. Build artifacts are in:
echo    %PROJECT_ROOT%\build\
echo ============================================================
echo.
echo Quick commands:
echo   ctest --preset tests.msvc -C %BUILD_TYPE%  -- run tests
echo   cmake --build build -j                      -- rebuild
exit /b 0
