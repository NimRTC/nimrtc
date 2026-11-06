@echo off
:: scripts/msvc_build.bat -- run a command inside the VS 2022 x64 developer env.
::
:: Usage:
::   scripts\msvc_build.bat build            -- configure + build (Debug)
::   scripts\msvc_build.bat build --target X -- configure + build one target
::   scripts\msvc_build.bat test             -- build + ctest
::   scripts\msvc_build.bat raw "<command>"  -- run an arbitrary command
::
:: The CMake preset "debug.msvc" puts the compiler in PATH, but this script
:: also works with a plain `cmake --build` invocation, which needs the
:: vcvars environment that this script sets up.
setlocal EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
set "PROJECT_ROOT=%SCRIPT_DIR%.."
set "VCVARS=D:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"

if not exist "%VCVARS%" (
    echo [ERROR] vcvarsall.bat not found at "%VCVARS%".
    exit /b 1
)

call "%VCVARS%" x64 >nul 2>&1
if errorlevel 1 (
    echo [ERROR] vcvarsall.bat failed.
    exit /b 1
)

if "%~1"=="raw" (
    shift
    %*
    exit /b %errorlevel%
)

if "%~1"=="" set "MODE=build"
if "%~1"=="build" set "MODE=build"
if "%~1"=="test" set "MODE=test"
if "%~1"=="configure" set "MODE=configure"

set "BUILD_TYPE=Debug"
if "%~2"=="--release" set "BUILD_TYPE=Release"

set "TARGET_ARG="
if "%~2"=="--target" (
    set "TARGET_ARG=--target"
    set "TARGET_NAME=%~3"
)

cd /d "%PROJECT_ROOT%"

if "%MODE%"=="configure" (
    echo [INFO] Configuring (debug.msvc, %BUILD_TYPE%)...
    cmake --preset debug.msvc -DCMAKE_BUILD_TYPE=%BUILD_TYPE%
    exit /b %errorlevel%
)

echo [INFO] Building (%BUILD_TYPE%)...
if "%TARGET_NAME%"=="" (
    cmake --build build --config %BUILD_TYPE% -j
) else (
    cmake --build build --config %BUILD_TYPE% -j --target %TARGET_NAME%
)
if errorlevel 1 (
    echo [ERROR] Build failed.
    exit /b 1
)

if "%MODE%"=="test" (
    echo [INFO] Running ctest...
    ctest --test-dir build -C %BUILD_TYPE% --output-on-failure
    exit /b %errorlevel%
)

echo [OK] Build succeeded.
exit /b 0
