# third_party/libsrtp/CMakeLists.txt
# =============================================================================
# libsrtp — SRTP (Secure Real-time Transport Protocol) library.
#
# Populated by: git clone https://github.com/cisco/libsrtp.git third_party/libsrtp/src
#
# Upstream version: libsrtp 3.0.0-dev (vendored commit: see src/.git).
# Target produced by upstream: `srtp3`.
#
# For now we build libsrtp against its built-in reference crypto backend
# (AES-CM + HMAC-SHA1) so the project links cleanly without an external
# crypto library. When the NimRTC DTLS module lands we will switch
# CRYPTO_LIBRARY to "mbedtls" and wire our vendored mbedtls target in.
# =============================================================================

set(_upstream_dir "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Upstream options we override for a clean static-only NimRTC build.
set(BUILD_SHARED_LIBS         OFF  CACHE BOOL "" FORCE)
set(LIBSRTP_TEST_APPS         OFF  CACHE BOOL "" FORCE)
set(ENABLE_WARNINGS_AS_ERRORS OFF  CACHE BOOL "" FORCE)
set(CRYPTO_LIBRARY            "internal" CACHE STRING "" FORCE)
# Internal backend only ships AES-CM; tell libsrtp to skip the GCM probe
# (it gates on the backend choice, but be explicit).
set(GCM                       OFF CACHE BOOL INTERNAL)

# Delegate to upstream CMakeLists.txt, which produces the `srtp3` STATIC target.
add_subdirectory(${_upstream_dir} ${CMAKE_BINARY_DIR}/_vendor_libsrtp_build)

# Wrap the upstream target in an INTERFACE library so consumers only see the
# public NimRTC conventions (nimrtc_vendor_libsrtp + nimrtc::vendor::libsrtp).
add_library(nimrtc_vendor_libsrtp INTERFACE)
add_library(nimrtc::vendor::libsrtp ALIAS nimrtc_vendor_libsrtp)

target_link_libraries(nimrtc_vendor_libsrtp INTERFACE srtp3)
target_include_directories(nimrtc_vendor_libsrtp
    INTERFACE
        $<BUILD_INTERFACE:${_upstream_dir}/include>
)

target_compile_definitions(nimrtc_vendor_libsrtp
    INTERFACE
        NIMRTC_USE_LIBSRTP=1
        SRTP_NO_DEPRECATED=1
)
