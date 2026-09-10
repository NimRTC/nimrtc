# wolfSSL DTLS interoperability test
cmake_minimum_required(VERSION 3.20)
project(wolfssl_dtls_test LANGUAGES C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fetch wolfSSL
include(FetchContent)

FetchContent_Declare(
    wolfssl
    GIT_REPOSITORY https://github.com/wolfSSL/wolfssl.git
    GIT_TAG v5.7.2
    GIT_SHALLOW TRUE
)

# Configure wolfSSL for DTLS support
set(WOLFSSL_DTLS TRUE)
set(HAVE_DTLS TRUE)
set(WOLFSSL_STATIC_MEMORY FALSE)
set(USE_CERT_BUFFERS_256 TRUE)
set(NO_RC4 TRUE)
set(NO_HC128 TRUE)
set(NO_RABBIT TRUE)
set(NO_DES3 TRUE)

FetchContent_MakeAvailable(wolfssl)

# Build the test executable
add_executable(wolfssl_dtls_server
    main.cpp
)

target_link_libraries(wolfssl_dtls_server
    PRIVATE wolfssl
)

# Print SPKI fingerprint for Chrome
target_compile_definitions(wolfssl_dtls_server
    PRIVATE
        WOLFSSL_DTLS
        HAVE_DTLS
)

# Enable verbose logging
target_compile_definitions(wolfssl_dtls_server
    PRIVATE
        DEBUG_WOLFSSL
)

# Copy certificates for testing
configure_file(
    ${WOLFSSL_SOURCE_DIR}/certs/client-cert.pem
    ${CMAKE_BINARY_DIR}/client-cert.pem
    COPYONLY
)
configure_file(
    ${WOLFSSL_SOURCE_DIR}/certs/client-key.pem
    ${CMAKE_BINARY_DIR}/client-key.pem
    COPYONLY
)
configure_file(
    ${WOLFSSL_SOURCE_DIR}/certs/ca-cert.pem
    ${CMAKE_BINARY_DIR}/ca-cert.pem
    COPYONLY
)
