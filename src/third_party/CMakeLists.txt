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
