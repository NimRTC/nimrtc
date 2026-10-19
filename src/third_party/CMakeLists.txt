# third_party/CMakeLists.txt
# =============================================================================
# Vendored third-party source libraries.
#
# Layout convention: one subdirectory per upstream library.
# Each subdirectory must contain a CMakeLists.txt that creates a target named
#   nimrtc_vendor_<name>          (e.g. nimrtc_vendor_libsrtp)
#
# All vendor targets must:
#   - be INTERFACE or STATIC (never SHARED)
#   - propagate include dirs via INTERFACE_INCLUDE_DIRECTORIES
#   - propagate compile definitions via INTERFACE_COMPILE_DEFINITIONS
#
# If a vendor's src/ directory is not yet populated, this file skips that
# subdirectory and relies on the empty stub target declared by
# NimRTCVendored.cmake. The stub will fail the build with a clear message
# when a module tries to link it.
# =============================================================================

# Helper: add a vendor only if its source tree exists.
# Sets up the target + include dirs + compile definitions when present.
function(_nimrtc_vendor_add_if_populated name)
    set(_src_dir "${CMAKE_CURRENT_SOURCE_DIR}/${name}/src")
    if(EXISTS "${_src_dir}/CMakeLists.txt")
        add_subdirectory(${name})
    elseif(NOT TARGET "nimrtc_vendor_${name}")
        # No source yet: create a bare INTERFACE target so downstream
        # target_link_libraries() does not fail at CMake generation time.
        add_library(nimrtc_vendor_${name} INTERFACE)
        add_library(nimrtc::vendor::${name} ALIAS nimrtc_vendor_${name})
    endif()
endfunction()

# Helper: patch msquic's src/platform/CMakeLists.txt to force the
# dummy raw datapath (datapath_raw_dummy.c) instead of the XDP-backed
# real datapath. Required because the Windows XDP SDK is not
# available in the NimRTC CI build env (windows-2022 image) and
# NimRTC does not ship XDP support. Must be called BEFORE
# add_subdirectory(msquic) so the patched file is what msquic's
# own CMake sees.
#
# The patch is idempotent (re-applying is a no-op) so a fresh
# `git submodule update --init` followed by another cmake configure
# is safe. We do NOT commit this patch into the submodule; the
# submodule stays vendor-clean (HEAD = upstream microsoft/msquic
# master SHA) and NimRTC overlays everything from this file.
function(_nimrtc_msquic_force_dummy_datapath)
    set(_platform "${CMAKE_CURRENT_SOURCE_DIR}/msquic/src/platform/CMakeLists.txt")
    if(NOT EXISTS "${_platform}")
        return()
    endif()
    file(READ "${_platform}" _cmake)
    # Check whether the platform/CMakeLists.txt has already been patched.
    # We cannot use `if(_cmake MATCHES ...)` or `if(_cmake CONTAINS ...)`
    # here because the marker contains a literal `)` which closes either
    # the CMake regex group (MATCHES) or the if() expression itself
    # (CONTAINS) prematurely. `string(FIND)` takes a plain substring and
    # returns the offset (or -1), which avoids both pitfalls.
    set(_marker "OR TRUE)  # NimRTC: force dummy raw datapath")
    string(FIND "${_cmake}" "${_marker}" _patched_idx)
    if(_patched_idx GREATER_EQUAL 0)
        return()  # already patched
    endif()
    # \${SYSTEM_PROCESSOR} below is escaped so CMake does not expand
    # the variable at parse time; the literal text we want to match
    # in msquic's CMakeLists is "${SYSTEM_PROCESSOR} STREQUAL \"arm64ec\")".
    string(REPLACE
        "\${SYSTEM_PROCESSOR} STREQUAL \"arm64ec\")"
        "\${SYSTEM_PROCESSOR} STREQUAL \"arm64ec\" ${_marker}"
        _cmake "${_cmake}")
    file(WRITE "${_platform}" "${_cmake}")
    message(STATUS "nimrtc: patched msquic platform/CMakeLists.txt to force dummy raw datapath (XDP SDK not in build env)")
endfunction()

_nimrtc_vendor_add_if_populated(libsrtp)
_nimrtc_vendor_add_if_populated(libopus)
_nimrtc_vendor_add_if_populated(mbedtls)
_nimrtc_vendor_add_if_populated(libjuice)
_nimrtc_vendor_add_if_populated(wolfssl)
_nimrtc_vendor_add_if_populated(webrtc_audio_processing)
# TPAL-5 (v0.11.0) — usrsctp SCTP userland stack. Required by the
# `UsrsctpSocket` production backend registered through
# `nimrtc::sctp::register_default_plugins()`. Source populated under
# `src/` by `scripts/vendor-fetch.sh usrsctp` (or curl/tar from
# https://codeload.github.com/sctplab/usrsctp/tar.gz/refs/tags/0.9.5.0).
_nimrtc_vendor_add_if_populated(usrsctp)

# v0.14.0-beta.1 (ADR-014): msquic QUIC library.
# WebTransport / QUIC backend for the IWebTransportSession seam.
# Populated by: git submodule add https://github.com/microsoft/msquic src/third_party/msquic
#
# msquic's root CMakeLists.txt lives at src/third_party/msquic/ (not a src/ subdirectory
# like the other vendors), so _nimrtc_vendor_add_if_populated() would fail the existence
# check.  We handle it explicitly here:
#   1. Add the msquic subdirectory so its "msquic" SHARED library target is created.
#   2. Create a thin INTERFACE wrapper "nimrtc_vendor_msquic" that propagates the public
#      include dir (src/inc) to every consumer and links against the "msquic" DLL.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/msquic/CMakeLists.txt")
    if(NOT TARGET nimrtc_vendor_msquic)
        # Add the msquic build (produces the SHARED "msquic" target).
        # QUIC_BUILD_TOOLS/QUIC_BUILD_TEST/QUIC_BUILD_PERF are all OFF by default
        # so only the core library is built.
        set(QUIC_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
        set(QUIC_BUILD_TEST  OFF CACHE BOOL "" FORCE)
        set(QUIC_BUILD_PERF  OFF CACHE BOOL "" FORCE)

        # v0.14.0-beta.1 (CI regression): see _nimrtc_msquic_force_dummy_datapath()
        # above for rationale. Must run BEFORE add_subdirectory(msquic) below so
        # the patched platform/CMakeLists.txt is what msquic's own CMake sees.
        _nimrtc_msquic_force_dummy_datapath()

        add_subdirectory(msquic)

        # v0.14.0-beta.1 (CI regression): msquic's `core` target compiles
        # `src/core/crypto_tls.c` with `/analyze /WX /sdl`, which trips a
        # known false-positive C28020 (SAL value-range check) on the
        # long macro-driven TlsRead/Write switch in lines 733-1238.
        # Symptom (Windows MSVC): `error C2220: warning treated as error`
        # at crypto_tls.c(1282,1), Build step fails before the engine link.
        #
        # Workaround: disable the specific false-positive from the NimRTC
        # side, without modifying the third-party submodule. The flag is
        # scoped to the `core` static lib so it doesn't leak into NimRTC's
        # own targets (which are built with their own /W4 /WX settings).
        if(MSVC AND TARGET core)
            target_compile_options(core PRIVATE /wd28020)
        endif()

        # Thin INTERFACE wrapper: consumers link against this, not "msquic" directly.
        # The wrapper propagates include dirs and links the msquic DLL transitively.
        add_library(nimrtc_vendor_msquic INTERFACE)
        add_library(nimrtc::vendor::msquic ALIAS nimrtc_vendor_msquic)
        target_include_directories(nimrtc_vendor_msquic INTERFACE
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/msquic/src/inc>)
        target_link_libraries(nimrtc_vendor_msquic INTERFACE msquic)
    endif()
endif()
