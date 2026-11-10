# Initialize the MSVC x64 build environment in the current PowerShell
# session, then run a command.
#
# Why this exists: the NimRTC build is driven by CMake, which passes the
# right language standard per target.  A `CL` inherited from the shell
# overrides that and breaks the vendored C11 sources (libopus), and an
# un-initialized shell has no INCLUDE / LIB at all, which makes every
# standard-library include fail with "cannot open include file".
#
# Usage (from the repository root):
#
#     .\scripts\msvc_env.ps1 -Command "cmake --build build\win-msvc --target test_fuzz_replay"
#
# The Visual Studio instance is located automatically via vswhere, so
# the script works whether the IDE lives on C: or D:.

param(
    [Parameter(Mandatory = $true)]
    [string]$Command
)

$ErrorActionPreference = "Stop"

function Find-VcVars64 {
    # vswhere ships with the Visual Studio installer and knows the
    # install path even when the drive letter is not C:.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $installPath = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath
        if ($installPath) {
            $candidate = Join-Path $installPath "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $candidate) { return $candidate }
        }
    }

    # Fall back to the conventional layouts.
    foreach ($root in @("C:\Program Files", "D:\Program Files", "C:\Program Files (x86)")) {
        foreach ($edition in @("Community", "Professional", "Enterprise", "BuildTools")) {
            $candidate = Join-Path $root "Microsoft Visual Studio\2022\$edition\VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $candidate) { return $candidate }
        }
    }
    return $null
}

$vcvars = Find-VcVars64
if (-not $vcvars) {
    throw "vcvars64.bat not found (looked via vswhere and the conventional VS 2022 layouts)"
}

# vcvars64.bat is a batch script, so it can only set the environment for
# a child cmd.exe.  We run the requested command inside that cmd.exe
# and let it inherit the freshly-set variables.
$script = @"
call "$vcvars" >nul
if errorlevel 1 exit /b 1
set "CL="
set "_CL_="
set "CLFLAGS="
$Command
"@

$scriptPath = Join-Path $env:TEMP "nimrtc_msvc_env_$([guid]::NewGuid().ToString('N')).cmd"
try {
    Set-Content -Path $scriptPath -Value $script -Encoding OEM
    & cmd.exe /d /c $scriptPath
    exit $LASTEXITCODE
} finally {
    Remove-Item -Path $scriptPath -Force -ErrorAction SilentlyContinue
}
