# modules/dtls/CMakeLists.txt
# ----------------------------------------------------------------------------
# DTLS (Datagram TLS, RFC 6347) — DTLS 1.2 handshake for WebRTC DTLS-SRTP
# keying material export (RFC 5764).
#
# Scope (P1):
#   - DTLS 1.2 handshake state machine (ClientHello / ServerHello /
#     ECDHE-ECDSA-AES128-SHA256 / Finished)
#   - Fingerprint generation + verification (SHA-256 over local ECDH pubkey)
#   - SRTP keying material export (RFC 5764 §4.2)
#
# Dependencies:
#   - core (always)
#   - Windows BCrypt (Win32 only; on other platforms a stub is compiled)
# ----------------------------------------------------------------------------
add_library(nimrtc_dtls STATIC
    src/dtls.cpp
    )

nimrtc_apply_options(nimrtc_dtls)

if(WIN32)
    target_link_libraries(nimrtc_dtls
        PUBLIC
            nimrtc::core
            bcrypt
        )
    target_compile_definitions(nimrtc_dtls PUBLIC NIMRTC_HAS_DTLS=1)
else()
    target_link_libraries(nimrtc_dtls
        PUBLIC
            nimrtc::core
        )
    target_compile_definitions(nimrtc_dtls PUBLIC NIMRTC_HAS_DTLS=1)
    message(STATUS "DTLS: non-Windows build, compiled without BCrypt (stub crypto)")
endif()

target_include_directories(nimrtc_dtls
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

add_library(nimrtc::dtls ALIAS nimrtc_dtls)
