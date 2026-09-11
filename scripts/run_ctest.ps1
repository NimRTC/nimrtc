# scripts/run_ctest.ps1
# Convenience wrapper for ctest on multi-config MSVC builds.
# Multi-config generators (Visual Studio) place test binaries under
# build/tests/<CONFIG>/ (Debug/Release/...).  CMake's CTestTestfile.cmake
# wraps each test command in an
#   if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
# conditional, so running plain "ctest" (no -C) marks every test as NOT_AVAILABLE.
#
# Usage:
#   .\scripts\run_ctest.ps1            # run all tests
#   .\scripts\run_ctest.ps1 -R filter  # run tests matching regex filter
#   .\scripts\run_ctest.ps1 --rerun-failed
#   .\scripts\run_ctest.ps1 -V         # verbose
#
# Equivalent to:  ctest -C Debug <args>

[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments=$true)]
    [string[]]$CTestArgs
)

# Script lives in <repo>/scripts/, so the build dir is <repo>/build.
# Resolve relative to the script's own location so callers can dot-source
# this from any working directory.
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "build"

if (-not (Test-Path $buildDir)) {
    Write-Error "Build directory not found: $buildDir.  Configure first with: cmake -B $buildDir -S $repoRoot"
    exit 1
}

Push-Location $buildDir
try {
    & ctest -C Debug @CTestArgs
} finally {
    Pop-Location
}
exit $LASTEXITCODE