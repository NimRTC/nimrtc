@echo off
:: scripts/msvc_env.bat -- neutralize MSVC environment variables that
:: corrupt a NimRTC build.
::
:: Usage:
::   call scripts\msvc_env.bat          -- sanitize the current environment
::   call scripts\msvc_env.bat --quiet  -- sanitize, report nothing
::
:: This file is a *fragment*.  It uses the classic
:: "endlocal & set ..." idiom so that the variables it clears are
:: cleared in the *caller's* environment while everything else it does
:: stays local.  Running it as a standalone command has no lasting
:: effect -- call it.
::
:: ---------------------------------------------------------------------------
:: Why this exists
:: ---------------------------------------------------------------------------
:: MSVC reads three environment variables and prepends their contents to
:: every compiler command line:
::
::   CL        -- options for every cl.exe invocation
::   _CL_      -- the internal variant, used for the debug DLLs
::   CLFLAGS   -- an older name for the same thing
::
:: A CL that carries a language-standard option is the dangerous case,
:: because it is applied to C and to C++ sources alike:
::
::   set CL=/std:c++20 /c
::
:: The vendored libopus compiles as C11, so CMake passes "-std:c11" on
:: its command line, and cl.exe then sees both:
::
::   cl : Command line error D8016: '/std:c++20' and '/std:c11' options
::         are incompatible
::
:: The failure is not limited to the compile step.  CMake's configure
:: step probes the compiler with try_compile, and that probe inherits
:: the same environment.  A probe that fails is recorded in the cache
:: as a *negative* result, and a negative result is sticky: libopus
:: then concludes that the compiler has no variable-length arrays, no
:: alloca and no pseudostack, and the build dies much later with
::
::   fatal error C1189: #error: "Opus requires one of VAR_ARRAYS,
::   USE_ALLOCA, or NONTHREADSAFE_PSEUDOSTACK be defined"
::
:: Clearing CL fixes both: the conflict disappears, and the next
:: configure re-runs the probe with a clean environment.  Note that the
:: bad probe result survives in the cache -- a build tree that was
:: configured with a polluted CL has to be reconfigured, not merely
:: rebuilt.
::
:: ---------------------------------------------------------------------------
:: What is cleared, and why
:: ---------------------------------------------------------------------------
:: CL, _CL_ and CLFLAGS are cleared wholesale rather than filtered.
:: Filtering means parsing a shell-quoted option string, and a filter
:: that mis-parses is worse than no filter at all: it would leave a
:: half-stripped "/std:c++20" behind.  Everything NimRTC compiles is
:: driven by CMake, which passes the right standard per language and
:: per target already; the environment has no legitimate role here.
::
:: The values are printed when they are dropped, so a build that relied
:: on them is visible rather than mysterious.  Set NIMRTC_KEEP_CL=1 to
:: keep them, for the rare case where an option really is needed --
:: but expect the standard conflict to come back with it.

setlocal EnableExtensions

set "QUIET="
if /i "%~1"=="--quiet" set "QUIET=1"
if "%NIMRTC_MSCV_ENV_QUIET%"=="1" set "QUIET=1"

:: Trim the flags: `set X=1 & ...` leaves a trailing space in the value,
:: and a comparison against the literal "1" would then fail.
set "KEEP=%NIMRTC_KEEP_CL%"
:trim_keep
if not defined KEEP goto :keep_trimmed
if "%KEEP:~-1%"==" " (
    set "KEEP=%KEEP:~0,-1%"
    goto :trim_keep
)
:keep_trimmed

set "SAVED_CL=%CL%"
set "SAVED_CL_=%_CL_%"
set "SAVED_CLFLAGS=%CLFLAGS%"

if /i "%KEEP%"=="1" (
    if not "%QUIET%"=="1" (
        echo [INFO] NIMRTC_KEEP_CL=1 -- keeping the inherited CL / _CL_ / CLFLAGS.
        echo [WARN] If the build fails with "D8016: /std:c++20 and /std:c11
        echo [WARN] options are incompatible", that is this variable.
    )
    endlocal & exit /b 0
)

if "%QUIET%"=="1" (
    endlocal & set "CL=" & set "_CL_=" & set "CLFLAGS=" & exit /b 0
)

if defined SAVED_CL echo [INFO] Cleared the inherited CL variable: %SAVED_CL%
if defined SAVED_CL_ echo [INFO] Cleared the inherited _CL_ variable.
if defined SAVED_CLFLAGS echo [INFO] Cleared the inherited CLFLAGS variable.

endlocal & set "CL=" & set "_CL_=" & set "CLFLAGS=" & exit /b 0

