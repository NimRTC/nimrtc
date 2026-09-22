# NimRTCVendored.cmake
# ----------------------------------------------------------------------------
# Standardised integration of vendored third-party libraries.
#
# Vendored libraries are checked into src/third_party/<name>/ as source.
# Each has a CMakeLists.txt that creates an INTERFACE/STATIC target with
# a well-known name. We declare those target names here so other code
# can use them without knowing paths.
#
# All vendor targets:
#   - are STATIC (compiled into nimrtc, never dynamically linked to vendor)
#   - propagate their include directories via INTERFACE
#   - propagate their compile definitions (NIMRTC_USE_<NAME>, etc.)
#   - are optional: if third_party/<name>/ is absent, the target is a
#     stub that fails the build with a clear message
#
# Enterprise escape hatch:
#   Each library has a per-library NIMRTC_VENDORED_<NAME> cache option
#   (default ON).  When set to OFF, nimrtc_acquire_target() falls back to
#   find_package() against a system-installed copy, and exposes an ALIAS
#   target named `nimrtc_<name>_acquired` that consumers can link instead
#   of the vendor target.  This unblocks air-gapped / license-restricted
#   environments that cannot vendor the source tree.
# ----------------------------------------------------------------------------

# -----------------------------------------------------------------------------
# Per-library vendor switches (default ON — vendored is the default path).
# -----------------------------------------------------------------------------
option(NIMRTC_VENDORED_OPUS        "Use vendored libopus (default ON)"  ON)
option(NIMRTC_VENDORED_SRTP        "Use vendored libsrtp (default ON)"  ON)
option(NIMRTC_VENDORED_JUICE        "Use vendored libjuice (default ON)" ON)
option(NIMRTC_VENDORED_WOLFSSL      "Use vendored wolfSSL (default ON)"  ON)
option(NIMRTC_VENDORED_WEBRTC_APM   "Use vendored WebRTC APM (default ON)" ON)

# -----------------------------------------------------------------------------
# Public target names (primary)
# -----------------------------------------------------------------------------
set(NIMRTC_VENDOR_LIBS
    nimrtc_vendor_libsrtp
    nimrtc_vendor_libopus
    nimrtc_vendor_libjuice
    nimrtc_vendor_wolfssl
    nimrtc_vendor_webrtc_apm
    # P1+
    # nimrtc_vendor_libvpx
    # P2+
    # nimrtc_vendor_usrsctp
)

# Convenience aliases (nimrtc::vendor::<name>) — created by src/third_party/CMakeLists.txt
# when the upstream source is populated. Consumers may use either form:
#   target_link_libraries(my_app PRIVATE nimrtc_vendor_libsrtp)          # primary
#   target_link_libraries(my_app PRIVATE nimrtc::vendor::libsrtp)         # alias

# -----------------------------------------------------------------------------
# Helper: require a vendored library to exist
# -----------------------------------------------------------------------------
function(nimrtc_require_vendor name)
    set(vendor_dir "${CMAKE_SOURCE_DIR}/src/third_party/${name}")
    if(NOT EXISTS "${vendor_dir}/CMakeLists.txt")
        message(FATAL_ERROR
            "Vendor library '${name}' not found at ${vendor_dir}.\n"
            "Either populate it (see docs/zh/architecture.md §11), "
            "or set NIMRTC_VENDORED_<NAME>=OFF (use the canonical name "
            "OPUS / SRTP / JUICE) to fall back to find_package().")
    endif()
endfunction()

# -----------------------------------------------------------------------------
# Helper: acquire a library either from the vendored tree or from the
# system.
#
#   nimrtc_acquire_target(<NAME> <vendored_target>)
#
# NAME            : one of OPUS, SRTP, JUICE, WOLFSSL (uppercase — selects
#                   the matching NIMRTC_VENDORED_<NAME> cache option and
#                   the system-target mapping).
# vendored_target : the in-tree STATIC/INTERFACE target created by the
#                   vendored CMakeLists.txt (e.g. nimrtc_vendor_libopus).
#
# Vendored path (default): require the on-disk tree under
#   src/third_party/<dir>/, then create an ALIAS
#   `nimrtc_<NAME>_acquired` → <vendored_target>.
#
# System path (NIMRTC_VENDORED_<NAME>=OFF): dispatch on NAME to the
#   matching find_package()/find_library()/find_path() recipe and produce
#   the same `nimrtc_<NAME>_acquired` ALIAS so consumers don't need to
#   change their target_link_libraries() calls.
#
# This function must be called AFTER add_subdirectory(src/third_party) so
# the vendored targets exist; for the system path it can be called any
# time.
# -----------------------------------------------------------------------------
function(nimrtc_acquire_target name vendored_target)
    if(NIMRTC_VENDORED_${name})
        # Vendored path: require the tree to be present and alias it.
        # The on-disk subdir name doesn't always match the canonical
        # uppercase NAME (libopus vs OPUS, libjuice vs JUICE, …), so we
        # map it explicitly here.
        if(${name} STREQUAL "OPUS")
            set(_vendor_subdir libopus)
        elseif(${name} STREQUAL "SRTP")
            set(_vendor_subdir libsrtp)
        elseif(${name} STREQUAL "JUICE")
            set(_vendor_subdir libjuice)
        elseif(${name} STREQUAL "WOLFSSL")
            set(_vendor_subdir wolfssl)
        elseif(${name} STREQUAL "WEBRTC_APM")
            set(_vendor_subdir webrtc_audio_processing)
        else()
            message(FATAL_ERROR
                "nimrtc_acquire_target: unknown library '${name}'. "
                "Expected one of OPUS, SRTP, JUICE, WOLFSSL, WEBRTC_APM.")
        endif()
        nimrtc_require_vendor(${_vendor_subdir})
        if(NOT TARGET ${vendored_target})
            message(FATAL_ERROR
                "nimrtc_acquire_target: vendor target '${vendored_target}' "
                "is not defined. Did src/third_party/${_vendor_subdir}/CMakeLists.txt "
                "fail to create it?")
        endif()
        if(NOT TARGET nimrtc_${name}_acquired)
            # Resolve ALIAS chains.  Some vendored wrappers (e.g. wolfssl)
            # create an ALIAS like `add_library(nimrtc_vendor_wolfssl ALIAS wolfssl)`.
            # CMake 3.20+ forbids `ALIAS -> ALIAS`, so we have to alias the
            # ultimate (non-ALIAS) target.  Walk the chain until we hit a
            # target that has no ALIASED_TARGET property.
            set(_root ${vendored_target})
            while(TRUE)
                get_target_property(_aliased ${_root} ALIASED_TARGET)
                if(_aliased)
                    set(_root ${_aliased})
                else()
                    break()
                endif()
            endwhile()
            add_library(nimrtc_${name}_acquired ALIAS ${_root})
        endif()
    else()
        # System path: dispatch on library name because the imported-target
        # names produced by each upstream config-file package differ.
        if(${name} STREQUAL "OPUS")
            find_package(Opus ${ARGN} REQUIRED)
            add_library(nimrtc_${name}_acquired ALIAS Opus::opus)
        elseif(${name} STREQUAL "SRTP")
            find_package(srtp2 ${ARGN} CONFIG QUIET)
            if(TARGET srtp2::srtp2)
                add_library(nimrtc_${name}_acquired ALIAS srtp2::srtp2)
            else()
                # manual imported target via find_library
                find_library(SRTP_LIB NAMES srtp2 srtp)
                find_path(SRTP_INCLUDE_DIR srtp2/srtp.h)
                add_library(srtp2_imported UNKNOWN IMPORTED)
                set_target_properties(srtp2_imported PROPERTIES IMPORTED_LOCATION "${SRTP_LIB}" INTERFACE_INCLUDE_DIRECTORIES "${SRTP_INCLUDE_DIR}")
                add_library(nimrtc_${name}_acquired ALIAS srtp2_imported)
            endif()
        elseif(${name} STREQUAL "JUICE")
            find_library(JUICE_LIB NAMES juice)
            find_path(JUICE_INCLUDE_DIR libjuice.h)
            add_library(juice_imported UNKNOWN IMPORTED)
            set_target_properties(juice_imported PROPERTIES IMPORTED_LOCATION "${JUICE_LIB}" INTERFACE_INCLUDE_DIRECTORIES "${JUICE_INCLUDE_DIR}")
            add_library(nimrtc_${name}_acquired ALIAS juice_imported)
        elseif(${name} STREQUAL "WOLFSSL")
            find_package(wolfssl ${ARGN} CONFIG QUIET)
            if(TARGET wolfssl::wolfssl)
                add_library(nimrtc_${name}_acquired ALIAS wolfssl::wolfssl)
            elseif(TARGET wolfssl_shared)
                add_library(nimrtc_${name}_acquired ALIAS wolfssl_shared)
            else()
                find_library(WOLFSSL_LIB NAMES wolfssl)
                find_path(WOLFSSL_INCLUDE_DIR wolfssl/ssl.h)
                add_library(wolfssl_imported UNKNOWN IMPORTED)
                set_target_properties(wolfssl_imported PROPERTIES
                    IMPORTED_LOCATION "${WOLFSSL_LIB}"
                    INTERFACE_INCLUDE_DIRECTORIES "${WOLFSSL_INCLUDE_DIR}")
                add_library(nimrtc_${name}_acquired ALIAS wolfssl_imported)
            endif()
        endif()
    endif()
endfunction()

# -----------------------------------------------------------------------------
# Per-library acquire helpers.  Call sites in module CMakeLists.txt files
# stay unchanged — they still call `nimrtc_link_libopus(target)` etc., and
# that function reaches `nimrtc_<name>_acquired` which is either the
# vendored target (default) or the system imported target.
#
# `nimrtc_acquire_all_vendored()` is called once from the top-level
# CMakeLists.txt AFTER `add_subdirectory(src/third_party)` so the vendored
# targets already exist.  Calling it earlier would fail the vendored-path
# check below because the vendor targets haven't been created yet.
# -----------------------------------------------------------------------------
function(nimrtc_acquire_all_vendored)
    nimrtc_acquire_target(OPUS    nimrtc_vendor_libopus)
    nimrtc_acquire_target(SRTP    nimrtc_vendor_libsrtp)
    nimrtc_acquire_target(JUICE   nimrtc_vendor_libjuice)
    nimrtc_acquire_target(WOLFSSL nimrtc_vendor_wolfssl)
    # WEBRTC APM is optional — its git submodule must be populated manually
    # (git clone of the webrtc-audio-processing tree is ~100 MB).  Skip it
    # gracefully if the source directory is absent so Linux builds without
    # the submodule still succeed.
    if(EXISTS "${CMAKE_SOURCE_DIR}/src/third_party/webrtc_audio_processing/src/CMakeLists.txt")
        nimrtc_acquire_target(WEBRTC_APM nimrtc_vendor_webrtc_apm)
    endif()
endfunction()

# -----------------------------------------------------------------------------
# Helper: convenience wrappers
# -----------------------------------------------------------------------------
function(nimrtc_link_libsrtp target)
    target_link_libraries(${target} PRIVATE nimrtc_SRTP_acquired)
    target_compile_definitions(${target} PRIVATE
        NIMRTC_USE_LIBSRTP=1
        # Suppress libsrtp's deprecation warnings if any
        SRTP_NO_DEPRECATED=1)
endfunction()

function(nimrtc_link_libopus target)
    target_link_libraries(${target} PRIVATE nimrtc_OPUS_acquired)
    target_compile_definitions(${target} PRIVATE NIMRTC_USE_LIBOPUS=1)
endfunction()

function(nimrtc_link_libjuice target)
    target_link_libraries(${target} PRIVATE nimrtc_JUICE_acquired)
    target_compile_definitions(${target} PRIVATE NIMRTC_USE_LIBJUICE=1)
endfunction()

function(nimrtc_link_wolfssl target)
    target_link_libraries(${target} PRIVATE nimrtc_WOLFSSL_acquired)
    target_compile_definitions(${target} PRIVATE
        NIMRTC_USE_WOLFSSL=1
        WOLFSSL_USER_SETTINGS=1)
endfunction()

# ADR-013: link GMSSL v3.x native library.
# GMSSL uses the same TLS API names as OpenSSL 1.1.1 (TLS_CONNECT,
# TLS_CTX, tls_do_handshake, tls_send, tls_recv etc.) but lives
# under <gmssl/tls.h> and links to libgmssl / gmssl.lib.
# The function searches: $ENV{GMSSL_ROOT}/include, /usr/local/include,
# /usr/include for <gmssl/tls.h>; and $ENV{GMSSL_ROOT}/lib, /usr/local/lib,
# /usr/lib for libgmssl / gmssl.lib.
function(nimrtc_link_gmssl target)
    # Propagate the GMSSL_ROOT hint from the caller (or the environment).
    set(_gmssl_roots
        "$ENV{GMSSL_ROOT}"
        "$ENV{GMSSL_ROOT}/lib"
        "$ENV{GMSSL_ROOT}/lib64"
        )
    # Only add system paths when no explicit root is set.
    if(NOT DEFINED ENV{GMSSL_ROOT} OR "$ENV{GMSSL_ROOT}" STREQUAL "")
        list(APPEND _gmssl_roots
            /usr/local
            /usr/local/lib
            /usr/local/lib64
            /usr
            )
    endif()

    # Find the include directory
    find_path(GMSSL_INCLUDE_DIR gmssl/tls.h
        PATHS ${_gmssl_roots}
        PATH_SUFFIXES include include/gmssl
        NO_DEFAULT_PATH)

    # Find the library
    find_library(GMSSL_LIB NAMES gmssl
        PATHS ${_gmssl_roots}
        PATH_SUFFIXES lib lib64
        NO_DEFAULT_PATH)

    if(NOT GMSSL_INCLUDE_DIR OR NOT GMSSL_LIB)
        message(FATAL_ERROR
            "NIMRTC_ENABLE_DTLS_GMSSL=ON but GMSSL v3.x not found.\n"
            "  GMSSL_INCLUDE_DIR = ${GMSSL_INCLUDE_DIR}\n"
            "  GMSSL_LIB         = ${GMSSL_LIB}\n"
            "  Searched roots: ${_gmssl_roots}\n"
            "  Set $ENV{GMSSL_ROOT} to your GMSSL install prefix.\n"
            "  Download & build GMSSL: https://github.com/guanzhi/GmSSL")
    endif()

    add_library(gmssl_imported UNKNOWN IMPORTED)
    set_target_properties(gmssl_imported PROPERTIES
        IMPORTED_LOCATION "${GMSSL_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${GMSSL_INCLUDE_DIR}")

    target_link_libraries(${target} PRIVATE gmssl_imported)
    target_compile_definitions(${target} PRIVATE
        NIMRTC_USE_GMSSL=1
        NIMRTC_HAS_DTLS_GMSSL=1)
endfunction()

# Future:
# function(nimrtc_link_usrsctp target) ...
function(nimrtc_link_webrtc_apm target)
    target_link_libraries(${target} PRIVATE nimrtc_WEBRTC_APM_acquired)
    target_compile_definitions(${target} PRIVATE NIMRTC_USE_WEBRTC_APM=1)
endfunction()
# function(nimrtc_link_libvpx target) ...
