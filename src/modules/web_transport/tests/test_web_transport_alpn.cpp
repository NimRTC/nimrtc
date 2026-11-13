/**
 * @file test_web_transport_alpn.cpp
 * @brief Regression test: the WebTransport client and server must offer the
 *        same ALPN.
 *
 * History: v1.0.1 shipped a listener that advertised the HTTP/3 token "h3"
 * (RFC 9114 §3.1) while the session advertised the legacy draft token "sm".
 * The two never matched, so every NimRTC-to-NimRTC WebTransport handshake was
 * rejected during the QUIC/TLS negotiation (Chrome interop was unaffected —
 * Chrome offers "h3").  The two sides now share one definition
 * (web_transport_msquic_alpn.hpp); this test locks that invariant in place so
 * the tokens cannot drift apart again.
 *
 * The test needs no network, no certificate and no msquic peer: it compares
 * the exact byte buffers the two sides hand to msquic.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>

#include "web_transport_msquic_alpn.hpp"

namespace {

std::string to_string(std::span<const std::uint8_t> bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

}  // namespace

TEST(WebTransportAlpn, ClientAndServerOfferTheSameToken) {
    // Compared as strings: the vendored GoogleTest cannot use std::span's
    // operator== (its EqHelper only knows the container/iterator overloads).
    EXPECT_EQ(to_string(nimrtc::web_transport::detail::session_alpn()),
              to_string(nimrtc::web_transport::detail::listener_alpn()));
}

TEST(WebTransportAlpn, TokenIsHttp3) {
    EXPECT_EQ(to_string(nimrtc::web_transport::detail::session_alpn()), "h3");
    EXPECT_EQ(to_string(nimrtc::web_transport::detail::listener_alpn()), "h3");
}

TEST(WebTransportAlpn, TokenLengthExcludesTerminator) {
    // "h3" is two bytes; the NUL terminator must not be part of the ALPN.
    EXPECT_EQ(nimrtc::web_transport::detail::session_alpn().size(), 2u);
    EXPECT_EQ(nimrtc::web_transport::detail::listener_alpn().size(), 2u);
    EXPECT_EQ(nimrtc::web_transport::detail::kAlpnTokenLength, 2u);
}
