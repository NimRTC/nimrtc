# NimRTCOptions.cmake
# ----------------------------------------------------------------------------
# Standardised compiler flags for the entire NimRTC tree.
# Every library/executable must include this via:
#     include(NimRTCOptions)
#     nimrtc_apply_options(<target>)
# ----------------------------------------------------------------------------

# C++20 baseline — set before project() so all targets inherit it.
#
# As of 2026, full C++20 support is stable on every compiler we target:
#   - MSVC 19.30+ (Visual Studio 2019 16.10 / 2022 17.x)
#   - GCC 10+
#   - Clang 12+
#   - Apple Clang shipped with Xcode 14+
#
# C++20 features we rely on:
#   - std::span (essential for media buffer views; see core/bytes.hpp)
#   - std::optional, std::variant (also C++17, but paired with span)
#   - structured bindings (C++17), fold expressions (C++17), concepts
#
# Note: we deliberately stay at C++20 and not C++23 yet — std::expected and
# std::flat_map stabilisation is still incomplete on MSVC (as of 19.43).
set(NIMRTC_CXX_STANDARD 20 CACHE STRING "C++ standard for NimRTC")

# We don't vendor anything that requires C, but usrsctp and libvpx have C
# code — request C11 for hygiene.
set(NIMRTC_C_STANDARD 11 CACHE STRING "C standard for vendored C libs")

# -----------------------------------------------------------------------------
# Apply language standards (project-wide)
# -----------------------------------------------------------------------------
# Always enforce C++20 — the project requires std::span, std::optional, etc.
# CMake's project() may set a default (e.g. 14 on some MSVC versions); override it.
set(CMAKE_CXX_STANDARD 20 CACHE STRING "C++ standard for NimRTC" FORCE)
if(NOT C_STANDARD)
    set(C_STANDARD ${NIMRTC_C_STANDARD})
endif()

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)        # -std=c++17, not -std=gnu++17
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # for clangd / IDE indexing

# -----------------------------------------------------------------------------
# Helper: apply NimRTC flags to a target
#
# Usage:
#   nimrtc_apply_options(<target>)             # strict (production code)
#   nimrtc_apply_options(<target> LENIENT)     # test/bench harnesses
#
# LENIENT mode disables -Werror (and a few specific warnings that are
# endemic to test code but never appear in production src/) so that
# legacy C-style casts in stat dumps, unused helpers behind #ifdef, and
# non-literal printf formats in debug helpers don't block CI. Warnings
# remain enabled — they still surface in the build log; we just don't
# treat them as fatal. Production src/ targets must NOT pass LENIENT.
# -----------------------------------------------------------------------------
function(nimrtc_apply_options target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "nimrtc_apply_options: target '${target}' does not exist")
    endif()

    cmake_parse_arguments(NIMRTC_OPT
        "LENIENT"   # options
        ""          # one-value args
        ""          # multi-value args
        ${ARGN})

    target_compile_features(${target} PUBLIC
        cxx_std_${NIMRTC_CXX_STANDARD}
        c_std_11)

    # Default visibility: only the public headers' symbols are exported.
    # Each module is responsible for marking its API with NIMRTC_API.
    set_target_properties(${target} PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        C_VISIBILITY_PRESET   hidden
        VISIBILITY_INLINES_HIDDEN ON)

    # -------------------------------------------------------------------------
    # Warnings — strict, ASCII-portable, no surprises
    # -------------------------------------------------------------------------
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4                 # baseline warning level
            /w14242             # 'identifier': conversion from 'T1' to 'T2'
            /w14254             # operator '|': used before 'switch' (false positive on enums)
            /w14263             # 'enum': not all members are initialised
            /permissive-        # standard-conforming
            /Zc:__cplusplus     # report correct __cplusplus macro
            /utf-8              # source encoding
            $<$<CONFIG:Debug>:/Od /Z7 /RTC1>
            $<$<CONFIG:Release>:/O2 /GL>)
        # Suppress MSVC C4996 'getenv is unsafe' — pre-existing code uses
        # std::getenv for feature-flag lookups (e.g. NIMRTC_DTLS_TRACE,
        # NIMRTC_ANSWER_DUMP); replacing every site with _dupenv_s would
        # be invasive churn for no real safety gain (env vars come from
        # trusted process-local configuration, not untrusted input).  This
        # define must be set BEFORE any standard headers are included,
        # hence it's a compile_definition (not just /D) so the project
        # build lines carry it consistently.
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wshadow -Wnon-virtual-dtor -Wold-style-cast
            -Wcast-align -Wunused -Woverloaded-virtual
            -Wconversion -Wsign-conversion
            -Wdouble-promotion
            -Wformat=2 -Wformat-security
            -Wmisleading-indentation
            -fno-common
            $<$<CONFIG:Debug>:-O0 -g3>
            $<$<CONFIG:Release>:-O2 -DNDEBUG>)

        # -Wduplicated-cond was added in Clang 13. Apple Clang's version
        # numbering tracks Xcode (not upstream Clang major), and Apple Clang
        # 15 (the GitHub-hosted macos runner) does NOT actually recognise the
        # flag — empirically it errors with 'unknown warning option'. Skip
        # it on Apple to keep -Werror strict; we still get the rest of the
        # warning set. Linux GCC/Clang always get the flag.
        if(NOT APPLE)
            target_compile_options(${target} PRIVATE -Wduplicated-cond)
        endif()

        # LENIENT mode: relax warnings that are endemic to test harnesses
        # but never appear in production src/ code. See header comment.
        if(NIMRTC_OPT_LENIENT)
            target_compile_options(${target} PRIVATE
                -Wno-old-style-cast
                -Wno-unused-function
                -Wno-format-nonliteral)
        endif()
    endif()

    # -------------------------------------------------------------------------
    # OpenHarmony (OHOS) musl-specific defines
    # -------------------------------------------------------------------------
    # OHOS uses musl libc; auto-defined by the musl compiler, but we add
    # it explicitly so downstream code can `__has_include(<features.h>)`
    # style checks remain stable when cross-compiling from a glibc host.
    if(CMAKE_SYSTEM_NAME STREQUAL "OHOS")
        target_compile_definitions(${target} PRIVATE
            NIMRTC_PLATFORM_OHOS=1
            _LIBCPP_HAS_MUSL_LIBC=1)
        # OHOS native runtime defaults to c++_shared (shared libc++).
        # cache var OHOS_STL was set in the toolchain file.
        if(OHOS_STL STREQUAL "c++_shared")
            target_compile_options(${target} PRIVATE
                -stdlib=libc++)
        endif()
    endif()

    # Vendored third-party library targets (nimrtc_vendor_*) must NEVER
    # get -Werror. Their source trees ship with upstream warnings that are
    # not under our control and are not our responsibility to fix.  Enabling
    # -Werror on them (e.g. on macOS where wolfSSL headers use old-style
    # C casts that transitively affect nimrtc_dtls) breaks the build with
    # no recourse other than patching upstream — which we don't want to do.
    string(TOLOWER "${target}" _target_lower)
    if(_target_lower MATCHES "^nimrtc_vendor_")
        return()
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /WX)
    else()
        target_compile_options(${target} PRIVATE -Werror)
    endif()

    # -------------------------------------------------------------------------
    # MSVC runtime library — handled project-wide via
    # CMAKE_MSVC_RUNTIME_LIBRARY in the top-level CMakeLists.txt.  Nothing
    # per-target needs to be set here because the variable cascades to
    # every target (including third_party via add_subdirectory).
    # -------------------------------------------------------------------------

    # -------------------------------------------------------------------------
    # Sanitizers (debug builds)
    # -------------------------------------------------------------------------
    if(NIMRTC_ASAN AND CMAKE_BUILD_TYPE STREQUAL "Debug")
        if(MSVC)
            target_compile_options(${target} PRIVATE /fsanitize=address)
        else()
            target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
            target_link_options(${target}      PRIVATE -fsanitize=address)
        endif()
    endif()

    if(NIMRTC_UBSAN AND CMAKE_BUILD_TYPE STREQUAL "Debug")
        if(NOT MSVC)
            target_compile_options(${target} PRIVATE -fsanitize=undefined)
            target_link_options(${target}      PRIVATE -fsanitize=undefined)
        endif()
    endif()
endfunction()

# -----------------------------------------------------------------------------
# NIMRTC_API export macro
# -----------------------------------------------------------------------------
# NOTE: P0 leaves this commented out. We generate the export header only
# when the first STATIC library with public symbols lands in P1. To enable:
#
#   include(GenerateExportHeader)
#   generate_export_header(nimrtc_<module>
#       BASE_NAME nimrtc
#       EXPORT_MACRO_NAME NIMRTC_API
#       EXPORT_FILE_NAME ${CMAKE_BINARY_DIR}/include/nimrtc/core/nimrtc_export.h)
#
# Until then, public symbols must not be marked NIMRTC_API.
