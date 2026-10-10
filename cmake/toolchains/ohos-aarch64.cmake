# cmake/toolchains/ohos-aarch64.cmake
# ----------------------------------------------------------------------------
# Cross-compilation (or native build) toolchain for OpenHarmony (OHOS) on
# aarch64.  OHOS uses a Linux-based kernel with a musl libc userland and
# the OpenHarmony Native API (C++ NDK equivalent, DevEco Studio toolchain).
#
# Two operating modes, in order of preference:
#
#   1. Real OHOS SDK cross toolchain installed (e.g. the aarch64-linux-musl
#      target from Huawei's ohos-sdk or the DevEco Studio native toolchain).
#      We discover the compiler via find_program() and use it directly.
#      Sets CMAKE_C/CXX_COMPILER_TARGET=aarch64-linux-gnu so packages that
#      emit per-target libs follow the multilib convention.
#
#   2. Native arm64 OHOS device / emulator (e.g. ubuntu-22.04-arm64 host
#      targeting aarch64-linux-musl).  We fall back to system clang with
#      --target=aarch64-linux-gnu so the build at least emits aarch64
#      object code; the linker is the system one.  When the system compiler
#      is already aarch64 the toolchain leaves CMAKE_C/CXX_COMPILER alone.
#
# Usage in CMakePresets.json:
#   "toolchainFile": "${sourceDir}/cmake/toolchains/ohos-aarch64.cmake"
# ----------------------------------------------------------------------------

# Always advertise OHOS to CMake so that find_* modules and GNUInstallDirs
# pick the right paths.  OHOS is architecturally "Linux" (same kernel ABI)
# but musl instead of glibc, so we set CMAKE_SYSTEM_NAME=OHOS so that
# CMake's platform-detection logic does not auto-assume glibc.
set(CMAKE_SYSTEM_NAME OHOS)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# OpenHarmony uses C++_shared as the default STL (equivalent to
# _GLIBCXX_USE_CXX11_ABI=1 in the GCC world).  Expose this as a cache
# variable so callers can switch to c++_static if they bundle libc++.
set(OHOS_STL "c++_shared" CACHE STRING "OHOS C++ STL: c++_shared or c++_static")

# Detect the canonical cross compiler.
find_program(_ohos_cc  NAMES aarch64-linux-gnu-gcc
                             aarch64-linux-musl-gcc
                             ohos-clang
                             PATHS
                             /opt/ohos-sdk/llvm/bin
                             /opt/ohos-sdk/native/llvm/bin
                             /usr/aarch64-linux-gnu
                             /usr/bin
                             /usr/local/bin)
find_program(_ohos_cxx NAMES aarch64-linux-gnu-g++
                             aarch64-linux-musl-g++
                             ohos-clang++
                             PATHS
                             /opt/ohos-sdk/llvm/bin
                             /opt/ohos-sdk/native/llvm/bin
                             /usr/aarch64-linux-gnu
                             /usr/bin
                             /usr/local/bin)

if(_ohos_cc AND _ohos_cxx)
    # Real cross toolchain: explicit compiler + sysroot.
    set(CMAKE_C_COMPILER   "${_ohos_cc}")
    set(CMAKE_CXX_COMPILER "${_ohos_cxx}")
    set(CMAKE_C_COMPILER_TARGET   aarch64-linux-gnu)
    set(CMAKE_CXX_COMPILER_TARGET aarch64-linux-gnu)

    # When targeting a foreign system CMake needs the sysroot to find
    # libc / libgcc.  OHOS SDK cross packages drop it under the toolchain
    # prefix (/usr/aarch64-linux-gnu for the GNU triple, or
    # /opt/ohos-sdk/native/sysroot for the musl triple).
    if(EXISTS "/usr/aarch64-linux-gnu")
        set(CMAKE_SYSROOT "/usr/aarch64-linux-gnu" CACHE PATH "")
    elseif(EXISTS "/opt/ohos-sdk/native/sysroot")
        set(CMAKE_SYSROOT "/opt/ohos-sdk/native/sysroot" CACHE PATH "")
    endif()

    # RPATH-friendly settings for OHOS (musl does not have /etc/ld.so.conf.d)
    set(CMAKE_SKIP_RPATH FALSE)
    set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)

    message(STATUS
        "ohos-aarch64 toolchain: using cross compilers "
        "${_ohos_cc} / ${_ohos_cxx}")
    return()
endif()

# No OHOS cross toolchain installed — try clang with --target.
find_program(_clang_c   NAMES clang   PATHS /usr/bin /usr/local/bin)
find_program(_clang_cxx NAMES clang++ PATHS /usr/bin /usr/local/bin)

if(_clang_cxx)
    # Check whether the host is already aarch64 (native arm64 runner).
    execute_process(
        COMMAND uname -m
        OUTPUT_VARIABLE _host_arch
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_host_arch STREQUAL "aarch64")
        # Native aarch64 build: leave CMAKE_C/CXX_COMPILER untouched.
        message(STATUS
            "ohos-aarch64 toolchain: host is already aarch64, "
            "using native compiler (musl sysroot must be provided by user)")
    else()
        # Cross via clang --target.  Note: this produces code for
        # aarch64-linux-gnu ABI (not OHOS-specific) but is sufficient
        # to verify the cmake configure step.  Real device testing
        # requires the OHOS SDK toolchain above.
        set(CMAKE_C_COMPILER   "${_clang_c}")
        set(CMAKE_CXX_COMPILER "${_clang_cxx}")
        set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   --target=aarch64-linux-gnu -fno-pic")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --target=aarch64-linux-gnu -fno-pic")
        message(STATUS
            "ohos-aarch64 toolchain: cross via clang --target "
            "(${_clang_cxx}) — for real device testing install the OHOS SDK")
    endif()
    return()
endif()

message(FATAL_ERROR
    "ohos-aarch64 toolchain: no compiler found.  Install either:\n"
    "  1. The OHOS SDK (DevEco Studio) aarch64 cross toolchain and set PATH\n"
    "  2. A Debian/Ubuntu cross package: apt-get install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu\n"
    "  3. A recent clang with aarch64 target support.")
