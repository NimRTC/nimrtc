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
::
:: NOTE: cmd.exe parses a parenthesised block as a whole statement, so an
:: unescaped "(" or ")" inside the text of an echo within such a block ends
:: the block early and aborts the script. Keep the messages below free of
:: parentheses, and read %errorlevel% as !errorlevel! inside blocks so the
:: value is taken after the command ran rather than when the block was read.
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

:: A CL that carries a language-standard option breaks the vendored C11
:: sources and poisons CMake's configure-time feature probes.  See
:: scripts/msvc_env.bat for the details.
call "%SCRIPT_DIR%msvc_env.bat"

if "%~1"=="raw" (
    :: NOTE: `shift` does NOT change %*, so the arguments cannot be taken
    :: from %* after a shift -- the mode word would still be in front of the
    :: command and cmd would try to execute "raw" as the command.  The
    :: positional expansion below drops the mode word explicitly.
    call %2 %3 %4 %5 %6 %7 %8 %9
    exit /b !errorlevel!
)

if "%~1"=="" set "MODE=build"
if "%~1"=="build" set "MODE=build"
if "%~1"=="test" set "MODE=test"
if "%~1"=="configure" set "MODE=configure"

set "BUILD_TYPE=Debug"
if "%~2"=="--release" set "BUILD_TYPE=Release"

set "TARGET_NAME="
if "%~2"=="--target" set "TARGET_NAME=%~3"

cd /d "%PROJECT_ROOT%"

:: A cache that was configured with a polluted CL holds a failed
:: compiler probe, and CMake never recomputes a probe on its own.  The
:: repair drops it so the configure below re-runs it.  It exits 1 when it
:: changed something, which is not an error here.
if exist "%PROJECT_ROOT%\build\CMakeCache.txt" (
    python "%SCRIPT_DIR%repair_msvc_cache.py" "%PROJECT_ROOT%\build"
    if errorlevel 2 (
        echo [ERROR] The CMake cache repair failed.
        exit /b 1
    )
)

if "%MODE%"=="configure" (
    echo [INFO] Configuring preset debug.msvc, type %BUILD_TYPE% ...
    cmake --preset debug.msvc -DCMAKE_BUILD_TYPE=%BUILD_TYPE%
    exit /b !errorlevel!
)

echo [INFO] Building %BUILD_TYPE% ...
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
    echo [INFO] Running ctest ...
    ctest --test-dir build -C %BUILD_TYPE% --output-on-failure
    exit /b !errorlevel!
)

echo [OK] Build succeeded.
exit /b 0
