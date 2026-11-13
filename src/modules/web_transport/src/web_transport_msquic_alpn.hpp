/**
 * @file web_transport_msquic_alpn.hpp
 * @brief Single source of truth for the WebTransport ALPN token.
 *
 * WebTransport rides on HTTP/3 with the extended CONNECT method
 * (RFC 9297), so the transport ALPN is the HTTP/3 token "h3"
 * (RFC 9114 §3.1).  Both peers MUST offer exactly the same ALPN:
 * a client that offers anything else is rejected during the QUIC
 * handshake (the server's ALPN list does not contain the offered
 * token), so the two sides used to drift apart silently.
 *
 * This header is the only place that names the token.  The client
 * (MsQuicSession) and the server (MsQuicListener) each expose the
 * exact byte buffer they hand to msquic; the unit test in
 * `src/web_transport/tests/test_web_transport_alpn.cpp` asserts the
 * two buffers are identical, which is what keeps the handshake from
 * regressing.
 *
 * @note Internal header — not part of the public API surface.
 */
#pragma once

#include <cstdint>
#include <span>

namespace nimrtc::web_transport::detail {

/// HTTP/3 ALPN token (RFC 9114 §3.1) used by WebTransport.
inline constexpr char kAlpnToken[] = "h3";

/// Length of the ALPN token without the terminating NUL.
inline constexpr std::uint32_t kAlpnTokenLength = 2;  // sizeof("h3") - 1

/// The ALPN buffer the client side (session) passes to msquic.
std::span<const std::uint8_t> session_alpn();

/// The ALPN buffer the server side (listener) passes to msquic.
std::span<const std::uint8_t> listener_alpn();

}  // namespace nimrtc::web_transport::detail
