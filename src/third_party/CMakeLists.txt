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
        add_subdirectory(msquic)

        # Thin INTERFACE wrapper: consumers link against this, not "msquic" directly.
        # The wrapper propagates include dirs and links the msquic DLL transitively.
        add_library(nimrtc_vendor_msquic INTERFACE)
        add_library(nimrtc::vendor::msquic ALIAS nimrtc_vendor_msquic)
        target_include_directories(nimrtc_vendor_msquic INTERFACE
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/msquic/src/inc>)
        target_link_libraries(nimrtc_vendor_msquic INTERFACE msquic)
    endif()
endif()
