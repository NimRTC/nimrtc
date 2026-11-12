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
# Warning baseline
# -----------------------------------------------------------------------------
# `-Wconversion` and `-Wsign-conversion` are the two strictest warnings
# the project enables, and they are also the two that create the most
# *debt*: a warning is reported at the point of use, so a single new
# line of code can surface a conversion problem in a function that was
# written months earlier.  The observed cost was concrete — three of
# the commits on 2026-11-12 were pure `-Wsign-conversion` cleanup with
# no functional content (267 lines changed, zero feature value), and
# every one of them was *retrospective* work triggered by unrelated
# work.
#
# The mechanism below keeps the warning ON for every target (so it is
# still reported, still visible in the build log, still greppable) but
# stops it from being *fatal* unless the target asks for it.  A target
# opts in by passing `STRICT_CONVERSIONS` to nimrtc_apply_options():
#
#     nimrtc_apply_options(nimrtc_rtp)                # warns, not fatal
#     nimrtc_apply_options(nimrtc_rtp STRICT_CONVERSIONS)  # warns AND fatal
#
# The default is deliberately the non-fatal one for exactly one reason:
# the project's own contract is that *new* code must not introduce a
# conversion problem, and that is enforced by the review of the diff,
# not by the compiler.  Turning the warning fatal for a whole target
# means that every future change to that target inherits the target's
# historical debt, which is how a 10-line feature turns into a
# 200-line cleanup.
#
# Targets whose *entire* surface is numeric (the RTP/RTCP packet
# parsers, the jitter buffer, the pacing pacer) opt in: for them a
# narrowing conversion is a protocol bug, not a style question, and
# they have no backlog to speak of.
set(NIMRTC_CONVERSION_WARNINGS_FATAL "OFF" CACHE STRING
    "Make -Wconversion / -Wsign-conversion fatal on every production target")

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
#   nimrtc_apply_options(<target>)                        # strict (production code)
#   nimrtc_apply_options(<target> LENIENT)                # test/bench harnesses
#   nimrtc_apply_options(<target> STRICT_CONVERSIONS)      # conversion warnings fatal
#
# LENIENT mode disables -Werror (and a few specific warnings that are
# endemic to test code but never appear in production src/) so that
# legacy C-style casts in stat dumps, unused helpers behind #ifdef, and
# non-literal printf formats in debug helpers don't block CI. Warnings
# remain enabled — they still surface in the build log; we just don't
# treat them as fatal. Production src/ targets must NOT pass LENIENT.
#
# STRICT_CONVERSIONS makes -Wconversion / -Wsign-conversion fatal on
# top of the default set.  See the "Warning baseline" section above for
# why this is opt-in per target rather than global.
# -----------------------------------------------------------------------------
function(nimrtc_apply_options target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "nimrtc_apply_options: target '${target}' does not exist")
    endif()

    cmake_parse_arguments(NIMRTC_OPT
        "LENIENT;STRICT_CONVERSIONS"   # options
        ""          # one-value args
        ""          # multi-value args
        ${ARGN})

    # Vendored third-party library targets (nimrtc_vendor_*) must receive
    # NO flags from NimRTCOptions. Their source trees ship with upstream
    # warnings (flexible array members in usrsctp, old-style C casts in
    # wolfSSL) that are not under our control. Skipping all options
    # ensures they compile cleanly regardless of warning levels.
    string(TOLOWER "${target}" _target_lower)
    if(_target_lower MATCHES "^nimrtc_vendor_")
        return()
    endif()

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

        # -------------------------------------------------------------------------
        # Conversion warnings: warn always, fail only on request.
        #
        # GCC and Clang have no per-warning "-Werror=" spelling for
        # enabling, but they DO have one for *demoting*: the
        # -Wno-error=<warning> form.  So the way to make ONE warning
        # non-fatal while the rest stay fatal is:
        #
        #     -Wno-error=conversion
        #     -Wno-error=sign-conversion
        #
        # The net effect is exactly what we want: a conversion problem is
        # still printed as a *warning* in every build, but it does not
        # fail the compile unless the target asked for STRICT_CONVERSIONS.
        #
        # Verified empirically against GCC 11.4 (the compiler the CI
        # Linux leg uses):
        #
        #   -Wconversion -Wsign-conversion -Werror
        #       -> "error: ... [-Werror=conversion]", build fails
        #   ... -Werror -Wno-error=conversion -Wno-error=sign-conversion
        #       -> "warning: ... [-Wconversion]", build succeeds
        #   ... and an unrelated warning (e.g. -Wunused-variable) is
        #       still fatal under the same flags.
        #
        # The option ORDER does not matter: GCC applies the per-warning
        # demotion regardless of whether it appears before or after
        # -Werror on the command line.  Both orders were tested and
        # behave identically.  (This was verified rather than assumed,
        # because the intuitive "demotion must come after -Werror"
        # reasoning does not hold here.)
        # -------------------------------------------------------------------------
        if(NOT NIMRTC_OPT_STRICT_CONVERSIONS AND
           NOT NIMRTC_CONVERSION_WARNINGS_FATAL)
            target_compile_options(${target} PRIVATE
                -Wno-error=conversion
                -Wno-error=sign-conversion)
        endif()

        # -Wduplicated-cond (duplicate `if` conditions inside an if/else
        # chain) is a GCC-only spelling.  Clang has no equivalent
        # diagnostic — it rejects BOTH '-Wduplicated-cond' and the
        # '-Wduplicate-cond' variant with "unknown warning option", and
        # -Werror turns that into a hard build failure.  Verified against
        # upstream clang 14 and Apple Clang 15; only GCC accepts it.
        #
        # Keying this off the compiler ID (not off the platform) matters:
        # the previous 'if(NOT APPLE)' guard only protected Apple Clang
        # and left every upstream-Clang build broken.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
           CMAKE_C_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${target} PRIVATE -Wduplicated-cond)
        endif()

        # LENIENT mode: relax warnings that are endemic to test harnesses
        # but never appear in production src/ code. See header comment.
        # -Wno-pedantic is required because NimRTC's SCTP seam includes
        # upstream usrsctp.h (a C99 header that uses flexible array
        # members like `char sre_data[]`); GCC treats these as errors
        # under -Wpedantic when compiled as C++. The usrsctp vendor
        # target itself compiles the .c files as C, but any .cpp file
        # that #includes <usrsctp.h> needs -Wno-pedantic to avoid the
        # "ISO C++ forbids flexible array member" fatal.
        if(NIMRTC_OPT_LENIENT)
            target_compile_options(${target} PRIVATE
                -Wno-pedantic
                -Wno-old-style-cast
                -Wno-unused-function
                -Wno-format-nonliteral)
        endif()

        # Treat warnings as errors on production targets (non-vendor).
        # wolfSSL is handled via WARNINGS_AS_ERRORS=OFF before add_subdirectory
        # in src/third_party/wolfssl/CMakeLists.txt so upstream warnings in
        # wolfSSL headers don't affect nimrtc_dtls.  usrsctp vendor target
        # is skipped entirely via the vendor_ return() guard above.
        if(MSVC)
            target_compile_options(${target} PRIVATE /WX)
        else()
            target_compile_options(${target} PRIVATE -Werror)
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

    # -------------------------------------------------------------------------
    # MSVC runtime library — handled project-wide via
    # CMAKE_MSVC_RUNTIME_LIBRARY in the top-level CMakeLists.txt.  Nothing
    # per-target needs to be set here because the variable cascades to
    # every target (including third_party via add_subdirectory).
    # -------------------------------------------------------------------------

    # -------------------------------------------------------------------------
    # Sanitizers — NOT handled here.
    #
    # The sanitizer flags are applied globally at the top level via
    # nimrtc_enable_sanitizer() (see the end of this file).  Rationale:
    # a sanitizer runtime must be linked into EVERY final binary, including
    # the ones that pull in vendored static libraries.  Applying the flags
    # per-target here would leave the vendor objects uninstrumented and —
    # worse — would miss the final link line of any executable that links
    # only vendored code, producing undefined references to e.g.
    # __asan_init.
    # -------------------------------------------------------------------------
endfunction()

# -----------------------------------------------------------------------------
# Enable a sanitizer for the whole build.
#
# Usage:  nimrtc_enable_sanitizer(address|thread|undefined)
#
# Applies the flag to BOTH the compile and the link lines of every target
# created afterwards, including the vendored third-party ones (their
# add_subdirectory() calls happen after this function is invoked).  The
# link flag is the important part: without it the final executable has
# undefined references to the sanitizer runtime when it links a
# sanitized static library.
#
# TSan and ASan are mutually exclusive — enabling both is a hard error
# because their instrumentations conflict at runtime.
# -----------------------------------------------------------------------------
function(nimrtc_enable_sanitizer kind)
    string(TOLOWER "${kind}" _kind)

    if(_kind STREQUAL "address")
        if(NIMRTC_TSAN)
            message(FATAL_ERROR
                "nimrtc_enable_sanitizer(address): NIMRTC_TSAN is also ON. "
                "AddressSanitizer and ThreadSanitizer cannot be enabled "
                "in the same build (their instrumentations conflict).")
        endif()
        if(MSVC)
            add_compile_options(/fsanitize=address)
            add_link_options(/fsanitize=address)
        else()
            add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
            add_link_options(-fsanitize=address)
        endif()
        message(STATUS "  Sanitizer        : AddressSanitizer (global)")
    elseif(_kind STREQUAL "thread")
        if(NIMRTC_ASAN)
            message(FATAL_ERROR
                "nimrtc_enable_sanitizer(thread): NIMRTC_ASAN is also ON. "
                "AddressSanitizer and ThreadSanitizer cannot be enabled "
                "in the same build (their instrumentations conflict).")
        endif()
        if(MSVC)
            message(WARNING
                "nimrtc_enable_sanitizer(thread): ThreadSanitizer is not "
                "supported by MSVC. Skipping.")
            return()
        endif()
        # TSan needs its own shadow memory mapping; modern Linux distros
        # default to PIE, which TSan supports, but the flag is kept
        # explicit for portability with the CI image.
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=thread)
        message(STATUS "  Sanitizer        : ThreadSanitizer (global)")
    elseif(_kind STREQUAL "undefined")
        if(MSVC)
            message(WARNING
                "nimrtc_enable_sanitizer(undefined): UBSan is not supported "
                "by MSVC. Skipping.")
            return()
        endif()
        # -fno-sanitize-recover makes UB a hard failure instead of a
        # "continue and print" diagnostic — a sanitizer gate is only
        # useful if it actually fails the build.
        add_compile_options(-fsanitize=undefined -fno-sanitize-recover=all -g)
        add_link_options(-fsanitize=undefined -fno-sanitize-recover=all)
        message(STATUS "  Sanitizer        : UndefinedBehaviorSanitizer (global)")
    else()
        message(FATAL_ERROR "nimrtc_enable_sanitizer: unknown kind '${kind}'")
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
