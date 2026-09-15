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
    )

nimrtc_apply_options(nimrtc_dtls)

target_link_libraries(nimrtc_dtls
    PUBLIC
        nimrtc::core
        nimrtc::vendor::wolfssl
    )
target_compile_definitions(nimrtc_dtls
    PUBLIC
        NIMRTC_HAS_DTLS=1
        NIMRTC_USE_WOLFSSL_DTLS=1
    )

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
    add_subdirectory(tests)
endif()

add_library(nimrtc::dtls ALIAS nimrtc_dtls)
