# cmake/toolchains/aarch64-linux-gnu.cmake
# ----------------------------------------------------------------------------
# Cross-compilation (or native build) toolchain for aarch64-linux-gnu.
#
# Two operating modes, in order of preference:
#
#   1. Native arm64 host (e.g. the `ubuntu-24.04-arm` GitHub runner, an
#      arm64 board running Linux).  The host already produces aarch64
#      object code, so the system compiler is used as-is: the
#      toolchain leaves `CMAKE_C/CXX_COMPILER` alone and only sets
#      `CMAKE_SYSTEM_NAME=Linux` + `CMAKE_SYSTEM_PROCESSOR=aarch64` so
#      CMake's detection agrees with the preset.  No cross package, no
#      sysroot — the native image has neither, and pointing
#      `CMAKE_SYSROOT` at a cross sysroot that does not exist breaks
#      the libc / libgcc lookup.
#
#   2. Real cross toolchain installed (`gcc-aarch64-linux-gnu` /
#      `g++-aarch64-linux-gnu` apt package on Debian / Ubuntu, or the
#      distro equivalent) on a non-aarch64 host. We discover
#      `aarch64-linux-gnu-gcc` / `aarch64-linux-gnu-g++` with
#      `find_program()` and use them directly.  Sets
#      `CMAKE_C/CXX_COMPILER_TARGET=aarch64-linux-gnu` so packages that
#      emit per-target libs (libgcc, libc, …) follow the multilib
#      convention.  When the host is not aarch64 and no cross package
#      is installed, the fallback is clang with `--target=aarch64-linux-gnu`
#      so the build at least emits aarch64 object code; the linker is
#      the system one.
#
# Usage in CMakePresets.json:
#   "toolchainFile": "${sourceDir}/cmake/toolchains/aarch64-linux-gnu.cmake"
# ----------------------------------------------------------------------------

# Always advertise aarch64-Linux to CMake so that find_* modules and
# GNUInstallDirs pick the right paths.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Is the build host itself an aarch64 Linux machine?  This decides
# between mode 1 (native) and mode 2 (cross) and must be answered
# *before* the cross-toolchain probe: on a native arm64 host the
# cross package would otherwise win the probe and drag in a sysroot
# the image does not have.
execute_process(
    COMMAND uname -m
    OUTPUT_VARIABLE _host_arch
    OUTPUT_STRIP_TRAILING_WHITESPACE)

if(_host_arch STREQUAL "aarch64")
    # Native aarch64 build: leave CMAKE_C/CXX_COMPILER untouched
    # (CMake auto-detects them from CC/CXX env or `which`).
    message(STATUS
        "aarch64-linux-gnu toolchain: host is already aarch64, "
        "using native compiler")
    return()
endif()

# Detect the canonical cross compiler.
find_program(_aarch64_cc  NAMES aarch64-linux-gnu-gcc
                             PATHS /usr/bin /usr/local/bin /opt/bin)
find_program(_aarch64_cxx NAMES aarch64-linux-gnu-g++
                             PATHS /usr/bin /usr/local/bin /opt/bin)

if(_aarch64_cc AND _aarch64_cxx)
    # Cross-compile: explicit toolchain.
    set(CMAKE_C_COMPILER  "${_aarch64_cc}")
    set(CMAKE_CXX_COMPILER "${_aarch64_cxx}")
    set(CMAKE_C_COMPILER_TARGET   aarch64-linux-gnu)
    set(CMAKE_CXX_COMPILER_TARGET aarch64-linux-gnu)
    # When targeting a foreign system, CMake needs the sysroot to find
    # libc / libgcc. The Debian/Ubuntu cross packages drop it under
    # /usr/aarch64-linux-gnu; fall back to /usr if not present.
    if(EXISTS "/usr/aarch64-linux-gnu")
        set(CMAKE_SYSROOT "/usr/aarch64-linux-gnu" CACHE PATH "")
    endif()
    message(STATUS
        "aarch64-linux-gnu toolchain: using cross compilers "
        "${_aarch64_cc} / ${_aarch64_cxx}")
    return()
endif()

# No cross toolchain installed — try clang with --target.
find_program(_clang_c  NAMES clang  PATHS /usr/bin /usr/local/bin)
find_program(_clang_cxx NAMES clang++ PATHS /usr/bin /usr/local/bin)

if(_clang_cxx)
    set(CMAKE_C_COMPILER   "${_clang_c}")
    set(CMAKE_CXX_COMPILER "${_clang_cxx}")
    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   --target=aarch64-linux-gnu")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --target=aarch64-linux-gnu")
    message(STATUS
        "aarch64-linux-gnu toolchain: cross via clang --target "
        "(${_clang_cxx})")
    return()
endif()

message(FATAL_ERROR
    "aarch64-linux-gnu toolchain: no compiler found. Install "
    "either the Debian/Ubuntu cross package "
    "('apt-get install g++-aarch64-linux-gnu') or a recent clang "
    "with aarch64 target support.")
