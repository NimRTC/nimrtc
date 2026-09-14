# wolfSSL DTLS interoperability test
cmake_minimum_required(VERSION 3.20)
project(wolfssl_dtls_test LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use the local wolfSSL from src/third_party/wolfssl/src
set(WOLFSSL_DTLS "yes" CACHE BOOL "Enable DTLS" FORCE)
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
set(WOLFSSL_SP_MATH_ALL      "no"  CACHE BOOL "" FORCE)
set(WOLFSSL_SP_MATH          "no"  CACHE BOOL "" FORCE)
set(WOLFSSL_SP_NO_DYN_STACK  "yes" CACHE BOOL "" FORCE)
set(WOLFSSL_SMALL_STACK      "yes" CACHE BOOL "" FORCE)

# Avoid duplicating the `wolfssl` target when this directory is consumed
# via add_subdirectory from the top-level tests/CMakeLists.txt (the
# NimRTC project already builds wolfssl via src/third_party/wolfssl).
# When this file is configured standalone (cmake -S tests/wolfssl_dtls),
# the target does NOT yet exist and we build a local copy.
if(NOT TARGET wolfssl)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src/third_party/wolfssl/src wolfssl_build)
endif()

# Build the loopback test executable
add_executable(wolfssl_dtls_loopback
    main.cpp
)

target_link_libraries(wolfssl_dtls_loopback
    PRIVATE wolfssl ws2_32 wsock32 iphlpapi
)

# Compile definitions
target_compile_definitions(wolfssl_dtls_loopback
    PRIVATE
        WOLFSSL_DTLS
        HAVE_DTLS
)

# Copy certificates for testing
set(WOLFSSL_CERTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../src/third_party/wolfssl/src/certs")
configure_file(
    ${WOLFSSL_CERTS_DIR}/client-cert.pem
    ${CMAKE_BINARY_DIR}/client-cert.pem
    COPYONLY
)
configure_file(
    ${WOLFSSL_CERTS_DIR}/client-key.pem
    ${CMAKE_BINARY_DIR}/client-key.pem
    COPYONLY
)
configure_file(
    ${WOLFSSL_CERTS_DIR}/ca-cert.pem
    ${CMAKE_BINARY_DIR}/ca-cert.pem
    COPYONLY
)
