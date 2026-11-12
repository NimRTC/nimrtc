# modules/dtls/CMakeLists.txt
# ----------------------------------------------------------------------------
# DTLS (Datagram TLS, RFC 6347) — DTLS 1.2 handshake for WebRTC DTLS-SRTP
# keying material export (RFC 5764).
#
# Scope (P1):
#   - DTLS 1.2 handshake via wolfSSL (ClientHello / ServerHello /
#     Certificate / ServerKeyExchange / CertificateVerify / Finished)
#   - ECDHE-ECDSA-AES128-GCM-SHA256 (RFC 5764 §5 mandatory profile)
#   - Fingerprint generation + verification (SHA-256 over local SPKI)
#   - SRTP keying material export (RFC 5764 §4.2)
#
# Architecture:
#   DtlsSession (public name) — thin pImpl wrapper, defined in
#     src/dtls_session.cpp.  All public methods forward to:
#   DtlsSessionWolfSSL — wolfSSL-backed implementation, defined in
#     src/dtls_wolfssl_session.cpp / dtls_wolfssl_session.hpp.
#
#   The hand-written DTLS state machine that previously lived in
#   src/dtls.cpp was removed when DTLS moved to wolfSSL; the
#   DtlsSessionWolfSSL class is the only DTLS provider now.  See
#   CHANGELOG.md [0.9.0-rc1] for the migration history.
#
# Dependencies:
#   - core (always)
#   - nimrtc::vendor::wolfssl (always — same backend on Windows, Linux, macOS)
# ----------------------------------------------------------------------------
add_library(nimrtc_dtls STATIC
    src/dtls_session.cpp
    src/dtls_wolfssl_session.cpp
    src/dtls_prf.cpp     # RFC 5246 §5 PRF (wolfSSL HMAC-SHA-256) —
                         # exported so the KAT + verify_data paths can
                         # run independently of the live handshake.
    )

nimrtc_apply_options(nimrtc_dtls)

# wolfSSL upstream headers use C-style casts in their public macros
# (e.g. WC_LAST_E comparisons in error-crypt.h / error-ssl.h).  Suppress
# the resulting -Wold-style-cast warning for this module — we can't
# modify upstream vendored code.
if(NOT MSVC)
    target_compile_options(nimrtc_dtls PRIVATE
        -Wno-old-style-cast
        -Wno-double-promotion)
endif()

target_link_libraries(nimrtc_dtls
    PUBLIC
        nimrtc::core
        nimrtc::vendor::wolfssl
    )
target_compile_definitions(nimrtc_dtls
    PUBLIC
        NIMRTC_HAS_DTLS=1
        NIMRTC_USE_WOLFSSL_DTLS=1
    PRIVATE
        # Absolute path to the directory holding the vendored ECC cert/key.
        # The wolfSSL session resolves its certificate relative to the
        # process working directory first (so a build-dir override wins),
        # then falls back to this absolute source-tree path.  The fallback
        # matters because CTest runs each test with the *test-file* directory
        # as its working directory, which is one level above the
        # `<config>/certs/` directory the build's copy rule populates.
        # Without it every DTLS-dependent engine test fails with
        # "wolfSSL_CTX setup failed" on multi-config generators.
        #
        # The path is spelled relative to *this* directory (not
        # CMAKE_SOURCE_DIR) so it stays correct when NimRTC is consumed
        # via add_subdirectory() from a larger project.
        NIMRTC_WOLFSSL_CERT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/wolfssl/src/certs")

target_include_directories(nimrtc_dtls
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        # PAL Slice 4 (v0.11.0) DTLS seam — DtlsSessionWolfSSL now
        # inherits IDtlsSession from src/dtls/include/nimrtc/dtls/.
        # Adding it as a PUBLIC include lets the seam module
        # (src/dtls/) reach IDtlsSession via the existing dtls
        # module's transitive include path.  No circular link
        # dependency: src/dtls/CMakeLists.txt links nimrtc::dtls
        # but does NOT add src/dtls/include/ back into its own
        # interface.
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/dtls/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    # ensure a CTestTestfile.cmake gets generated at this level so ctest
    # can recurse into the tests/ subdir from src/modules/ (CMake 4.0
    # VS generator quirk -- without enable_testing() here the file is
    # silently skipped and the module's tests never register).
    enable_testing()
    add_subdirectory(tests)
endif()

add_library(nimrtc::dtls ALIAS nimrtc_dtls)
