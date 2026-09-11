# wolfSSL ↔ Chrome DTLS-SRTP interop bridge
#
# Builds a standalone wolfSSL (DTLS 1.2 + SRTP + ECC + AES-GCM) and a
# small responder executable that acts as the NimRTC DTLS peer against
# a real Chrome browser tab behind the Python `bridge_shim.py`.
#
# The wolfSSL source is taken from `src/third_party/wolfssl/src` (no
# network fetch) and configured with DTLS, SRTP, ECC, AES-GCM, and
# SHA-2 — the minimum set WebRTC needs.

cmake_minimum_required(VERSION 3.20)
project(nimrtc_wolfssl_dtls_bridge LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Resolve wolfSSL source (project vendored, not fetched).
set(NIMRTC_WOLFSSL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/third_party/wolfssl/src"
    CACHE PATH "Path to wolfSSL source tree")

if(NOT EXISTS "${NIMRTC_WOLFSSL_SRC}/CMakeLists.txt")
    message(FATAL_ERROR
        "wolfSSL source not found at ${NIMRTC_WOLFSSL_SRC}. "
        "Expected src/third_party/wolfssl/src/CMakeLists.txt to exist.")
endif()

# Configure wolfSSL for DTLS-SRTP.
set(WOLFSSL_DTLS "yes"  CACHE BOOL "Enable DTLS" FORCE)
set(WOLFSSL_TLS13 "yes" CACHE BOOL "Enable TLS 1.3 (not used but kept for compat)" FORCE)
set(WOLFSSL_SRTP  "yes" CACHE BOOL "Enable SRTP" FORCE)
set(WOLFSSL_ECC   "yes" CACHE BOOL "Enable ECC" FORCE)
set(WOLFSSL_AESGCM "yes" CACHE BOOL "Enable AES-GCM" FORCE)
set(WOLFSSL_AESCBC "yes" CACHE BOOL "Enable AES-CBC" FORCE)
set(WOLFSSL_SHA256 "yes" CACHE BOOL "Enable SHA-256" FORCE)
set(WOLFSSL_SHA384 "yes" CACHE BOOL "Enable SHA-384" FORCE)
set(WOLFSSL_SHA512 "yes" CACHE BOOL "Enable SHA-512" FORCE)
set(WOLFSSL_RNG    "yes" CACHE BOOL "Enable RNG" FORCE)
set(WOLFSSL_ERROR_STRINGS "yes" CACHE BOOL "Enable error strings" FORCE)
set(WOLFSSL_SINGLE_THREADED "yes" CACHE BOOL "Single-threaded" FORCE)
# Disable things we don't need.
set(WOLFSSL_RSA   "no" CACHE BOOL "" FORCE)
set(WOLFSSL_DSA   "no" CACHE BOOL "" FORCE)
set(WOLFSSL_PSK   "no" CACHE BOOL "" FORCE)
set(WOLFSSL_ANON  "no" CACHE BOOL "" FORCE)
set(WOLFSSL_MD5   "no" CACHE BOOL "" FORCE)
set(WOLFSSL_OLD_TLS "no" CACHE BOOL "" FORCE)
set(WOLFSSL_SRP   "no" CACHE BOOL "" FORCE)
set(WOLFSSL_SSH   "no" CACHE BOOL "" FORCE)
set(WOLFSSL_OPENSSL "no" CACHE BOOL "" FORCE)
set(WOLFSSL_CERTGEN   "no" CACHE BOOL "" FORCE)
set(WOLFSSL_CERTREQ   "no" CACHE BOOL "" FORCE)
set(WOLFSSL_CERTEXT   "no" CACHE BOOL "" FORCE)
set(WOLFSSL_KEYGEN    "no" CACHE BOOL "" FORCE)
set(WOLFSSL_OCSP      "no" CACHE BOOL "" FORCE)
set(WOLFSSL_CRL       "no" CACHE BOOL "" FORCE)
set(WOLFSSL_DEBUG     "no" CACHE BOOL "" FORCE)
set(WOLFSSL_ASM       "no" CACHE BOOL "" FORCE)
set(WOLFSSL_DES3      "no" CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# Avoid stack-allocated mp_int / VLA codepaths (not legal in MSVC) and
# match the NimRTC in-tree wolfSSL configuration.  These must be set
# BEFORE add_subdirectory(wolfssl) because config.h is generated there.
# The vendored wolfSSL is configured by the NimRTC project (see
# src/third_party/wolfssl/CMakeLists.txt) without sp_int.c — keep the
# bridge consistent so sp_int.c isn't compiled.
#
# Bridge can be either consumed via add_subdirectory() from the top-level
# tests/CMakeLists.txt (NIMRTC_BUILD_BRIDGE_TESTS=ON path, where the
# NimRTC project already builds wolfssl via src/third_party/wolfssl) or
# configured standalone (cmake -S tests/wolfssl_dtls/bridge) for an
# out-of-tree debug session.  In the latter case the `wolfssl` target has
# not yet been defined and we need to build a local copy.
if(NOT TARGET wolfssl)
    add_subdirectory(${NIMRTC_WOLFSSL_SRC} wolfssl_build EXCLUDE_FROM_ALL)
endif()

# Make sure the consumer targets can find the wolfSSL include path.
include_directories(${NIMRTC_WOLFSSL_SRC})

# Build the responder.
add_executable(wolfssl_responder wolfssl_responder.cpp)

# Link with ws2_32 + wolfssl (case insensitive glob — wolfssl.lib / wolfssl.a).
target_link_libraries(wolfssl_responder PRIVATE wolfssl ws2_32 wsock32 iphlpapi)

# Define required macros for wolfSSL's DTLS-SRTP code paths on the consumer.
target_compile_definitions(wolfssl_responder PRIVATE WOLFSSL_DTLS HAVE_DTLS)

# Copy certs alongside the binary so a plain `./wolfssl_responder` works
# (default CLI values assume `certs/server-ecc.pem` relative to cwd).
add_custom_command(TARGET wolfssl_responder POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:wolfssl_responder>/certs
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/../certs/server-ecc.pem
            $<TARGET_FILE_DIR:wolfssl_responder>/certs/server-ecc.pem
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/../certs/ecc-key.pem
            $<TARGET_FILE_DIR:wolfssl_responder>/certs/ecc-key.pem
    COMMENT "Copying test certs into $<TARGET_FILE_DIR:wolfssl_responder>/certs")

# A phony aggregate target: `cmake --build build/wolfssl_dtls_bridge --target bridge`.
add_custom_target(bridge DEPENDS wolfssl_responder)
