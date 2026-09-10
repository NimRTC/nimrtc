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
option(NIMRTC_VENDORED_OPUS    "Use vendored libopus (default ON)"  ON)
option(NIMRTC_VENDORED_SRTP    "Use vendored libsrtp (default ON)"  ON)
option(NIMRTC_VENDORED_MBEDTLS "Use vendored mbedtls (default ON)"  ON)
option(NIMRTC_VENDORED_JUICE   "Use vendored libjuice (default ON)" ON)
option(NIMRTC_VENDORED_WOLFSSL "Use vendored wolfSSL (default ON)"  ON)

# -----------------------------------------------------------------------------
# Public target names (primary)
# -----------------------------------------------------------------------------
set(NIMRTC_VENDOR_LIBS
    nimrtc_vendor_libsrtp
    nimrtc_vendor_libopus
    nimrtc_vendor_mbedtls
    nimrtc_vendor_libjuice
    nimrtc_vendor_wolfssl
    # P1+
    # nimrtc_vendor_libvpx
    # P2+
    # nimrtc_vendor_usrsctp
    # nimrtc_vendor_webrtc_apm
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
            "Either populate it (see docs/zh/NimRTC-V2-技术文档.md §11), "
            "or set NIMRTC_VENDORED_<NAME>=OFF (use the canonical name "
            "OPUS / SRTP / MBEDTLS / JUICE) to fall back to find_package().")
    endif()
endfunction()

# -----------------------------------------------------------------------------
# Helper: acquire a library either from the vendored tree or from the
# system.
#
#   nimrtc_acquire_target(<NAME> <vendored_target>)
#
# NAME            : one of OPUS, SRTP, MBEDTLS, JUICE (uppercase — selects
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
        elseif(${name} STREQUAL "MBEDTLS")
            set(_vendor_subdir mbedtls)
        elseif(${name} STREQUAL "JUICE")
            set(_vendor_subdir libjuice)
        elseif(${name} STREQUAL "WOLFSSL")
            set(_vendor_subdir wolfssl)
        else()
            message(FATAL_ERROR
                "nimrtc_acquire_target: unknown library '${name}'. "
                "Expected one of OPUS, SRTP, MBEDTLS, JUICE, WOLFSSL.")
        endif()
        nimrtc_require_vendor(${_vendor_subdir})
        if(NOT TARGET ${vendored_target})
            message(FATAL_ERROR
                "nimrtc_acquire_target: vendor target '${vendored_target}' "
                "is not defined. Did src/third_party/${_vendor_subdir}/CMakeLists.txt "
                "fail to create it?")
        endif()
        if(NOT TARGET nimrtc_${name}_acquired)
            add_library(nimrtc_${name}_acquired ALIAS ${vendored_target})
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
        elseif(${name} STREQUAL "MBEDTLS")
            find_package(mbedtls ${ARGN} CONFIG QUIET)
            if(TARGET mbedtls::mbedtls)
                if(NOT TARGET nimrtc_${name}_acquired)
                    add_library(nimrtc_${name}_acquired INTERFACE IMPORTED)
                    set_target_properties(nimrtc_${name}_acquired PROPERTIES
                        INTERFACE_LINK_LIBRARIES "mbedtls::mbedtls;mbedtls::mbedcrypto;mbedtls::mbedx509")
                endif()
            else()
                find_library(MBEDTLS_LIB NAMES mbedtls mbedcrypto)
                find_library(MBEDX509_LIB NAMES mbedx509)
                find_library(MBEDCRYPTO_LIB NAMES mbedcrypto)
                add_library(mbedtls_imported UNKNOWN IMPORTED)
                set_target_properties(mbedtls_imported PROPERTIES IMPORTED_LOCATION "${MBEDTLS_LIB}")
                add_library(nimrtc_${name}_acquired ALIAS mbedtls_imported)
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
    nimrtc_acquire_target(MBEDTLS nimrtc_vendor_mbedtls)
    nimrtc_acquire_target(JUICE   nimrtc_vendor_libjuice)
    nimrtc_acquire_target(WOLFSSL nimrtc_vendor_wolfssl)
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

function(nimrtc_link_mbedtls target)
    target_link_libraries(${target} PRIVATE nimrtc_MBEDTLS_acquired)
    target_compile_definitions(${target} PRIVATE NIMRTC_USE_MBEDTLS=1)
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

# Future:
# function(nimrtc_link_usrsctp target) ...
# function(nimrtc_link_webrtc_apm target) ...
# function(nimrtc_link_libvpx target) ...
