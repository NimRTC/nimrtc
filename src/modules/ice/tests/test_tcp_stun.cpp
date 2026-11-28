// modules/ice/tests/test_tcp_stun.cpp
// =============================================================================
// STUN wire-format tests for the ICE-over-TCP connectivity-check driver.
//
// The driver carries its own STUN implementation (RFC 8489 §6, RFC 8445
// §7.1.2) because the TCP path shares no wire code with libjuice's UDP
// STUN.  These tests pin the message layout field by field — the slice's
// stated risk 1 — and the MESSAGE-INTEGRITY keying (RFC 5389 §15.4):
//   - the request carries USERNAME, PRIORITY, ICE-CONTROLLING, and
//     USE-CANDIDATE before the MESSAGE-INTEGRITY;
//   - the header length includes the MESSAGE-INTEGRITY attribute;
//   - verification keys the HMAC with the *remote* password (request) or
//     the *local* password (response), so the wrong key must fail.
// =============================================================================

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "tcp_ice.hpp"

namespace {

using nimrtc::ice::StunRequestInfo;
using nimrtc::ice::stun_build_binding_request;
using nimrtc::ice::stun_build_binding_response;
using nimrtc::ice::stun_verify_binding_request;
using nimrtc::ice::stun_verify_binding_response;

std::uint16_t be_u16(const std::vector<std::uint8_t>& v, std::size_t off) {
    return static_cast<std::uint16_t>((v[off] << 8) | v[off + 1]);
}

std::uint32_t be_u32(const std::vector<std::uint8_t>& v, std::size_t off) {
    return (static_cast<std::uint32_t>(v[off]) << 24) |
           (static_cast<std::uint32_t>(v[off + 1]) << 16) |
           (static_cast<std::uint32_t>(v[off + 2]) << 8) |
           static_cast<std::uint32_t>(v[off + 3]);
}

constexpr std::uint32_t kCookie = 0x2112A442u;

// 12 bytes so the transaction id can be asserted byte for byte.
std::array<std::uint8_t, 12> kTxn() {
    std::array<std::uint8_t, 12> txn{};
    for (std::size_t i = 0; i < txn.size(); ++i) txn[i] = static_cast<std::uint8_t>(i);
    return txn;
}

TEST(TcpStun, BindingRequestWireLayout) {
    const auto txn = kTxn();
    const auto msg = stun_build_binding_request(
        "aa:bb", "remote-secret", 1853882113u /* active host TCP priority */,
        0x1122334455667788ull, /*use_candidate=*/true, txn);

    ASSERT_EQ(msg.size(), 80u);  // 20 header + USERNAME(8) + PRIORITY(8) +
                                 // ICE-CONTROLLING(12) + USE-CANDIDATE(4) + MI(24)
    // Header: type, length (incl. MI), magic cookie, transaction id.
    EXPECT_EQ(be_u16(msg, 0), 0x0001);           // Binding Request
    EXPECT_EQ(be_u16(msg, 2), msg.size() - 20);  // length counts the MI attr
    EXPECT_EQ(be_u32(msg, 4), kCookie);
    EXPECT_TRUE(std::equal(txn.begin(), txn.end(), msg.begin() + 8));

    // USERNAME: type 0x0006, "aa:bb".
    EXPECT_EQ(be_u16(msg, 20), 0x0006);
    EXPECT_EQ(be_u16(msg, 22), 5u);
    EXPECT_EQ(std::string(msg.begin() + 24, msg.begin() + 29), "aa:bb");

    // PRIORITY: type 0x0024, big-endian value.
    EXPECT_EQ(be_u16(msg, 32), 0x0024);
    EXPECT_EQ(be_u16(msg, 34), 4u);
    EXPECT_EQ(be_u32(msg, 36), 1853882113u);

    // ICE-CONTROLLING: type 0x802A, 8-byte tiebreaker big-endian.
    EXPECT_EQ(be_u16(msg, 40), 0x802A);
    EXPECT_EQ(be_u16(msg, 42), 8u);
    EXPECT_EQ(msg[44], 0x11);
    EXPECT_EQ(msg[45], 0x22);
    EXPECT_EQ(msg[50], 0x77);
    EXPECT_EQ(msg[51], 0x88);

    // USE-CANDIDATE: type 0x0025, zero length.
    EXPECT_EQ(be_u16(msg, 52), 0x0025);
    EXPECT_EQ(be_u16(msg, 54), 0u);

    // MESSAGE-INTEGRITY last: type 0x0008, 20-byte digest.
    EXPECT_EQ(be_u16(msg, 56), 0x0008);
    EXPECT_EQ(be_u16(msg, 58), 20u);
}

TEST(TcpStun, BindingRequestOmitsUseCandidateWhenNotRequested) {
    const auto txn = kTxn();
    const auto msg = stun_build_binding_request(
        "aa:bb", "remote-secret", 1853882113u, 1ull, /*use_candidate=*/false, txn);

    // USERNAME(8) + PRIORITY(8) + ICE-CONTROLLING(12) + MI(24): 52 attrs.
    ASSERT_EQ(msg.size(), 76u);
    EXPECT_EQ(be_u16(msg, 40), 0x802A);            // ICE-CONTROLLING right after PRIORITY
    EXPECT_EQ(be_u16(msg, 52), 0x0008);            // MI immediately follows
}

TEST(TcpStun, VerifyRequestAcceptsRightKeyRejectsWrongKey) {
    const auto txn = kTxn();
    const auto msg = stun_build_binding_request(
        "aa:bb", "remote-secret", 1853882113u, 0x99ull, true, txn);

    const auto ok = stun_verify_binding_request(msg, "remote-secret");
    EXPECT_TRUE(ok.valid);
    EXPECT_EQ(ok.username, "aa:bb");
    EXPECT_TRUE(std::equal(ok.transaction_id.begin(), ok.transaction_id.end(),
                           txn.begin()));

    // The HMAC is keyed with the remote password; a wrong key must fail.
    const auto bad = stun_verify_binding_request(msg, "wrong-secret");
    EXPECT_FALSE(bad.valid);
}

TEST(TcpStun, VerifyRequestRejectsGarbage) {
    const auto bad = stun_verify_binding_request({}, "secret");
    EXPECT_FALSE(bad.valid);

    // Right type + cookie but truncated (length field beyond the buffer).
    std::vector<std::uint8_t> truncated(20, 0x00);
    truncated[0] = 0x00;
    truncated[1] = 0x01;
    truncated[2] = 0x00;
    truncated[3] = 0xFF;
    const auto t2 = stun_verify_binding_request(truncated, "secret");
    EXPECT_FALSE(t2.valid);
}

TEST(TcpStun, BindingResponseWireLayout) {
    const auto txn = kTxn();
    const auto msg = stun_build_binding_response("aa:bb", "local-secret",
                                                 txn, "127.0.0.1", 5000);

    // Header + USERNAME(8) + XOR-MAPPED-ADDRESS(12) + MI(24) = 68.
    ASSERT_EQ(msg.size(), 68u);
    EXPECT_EQ(be_u16(msg, 0), 0x0101);           // Binding Success
    EXPECT_EQ(be_u16(msg, 2), msg.size() - 20);  // length includes the MI attr
    EXPECT_EQ(be_u32(msg, 4), kCookie);
    EXPECT_TRUE(std::equal(txn.begin(), txn.end(), msg.begin() + 8));

    // XOR-MAPPED-ADDRESS: type 0x0020, IPv4 family, port and address XORed
    // with the magic cookie (RFC 5389 §15.2).
    EXPECT_EQ(be_u16(msg, 32), 0x0020);
    EXPECT_EQ(be_u16(msg, 34), 8u);
    EXPECT_EQ(msg[36], 0x00);
    EXPECT_EQ(msg[37], 0x01);  // family = IPv4
    const std::uint16_t xport = be_u16(msg, 38);
    EXPECT_EQ(xport ^ 0x2112, 5000u);
    EXPECT_EQ(msg[40], static_cast<std::uint8_t>(0x7F ^ 0x21));  // 127.0.0.1
    EXPECT_EQ(msg[41], static_cast<std::uint8_t>(0x00 ^ 0x12));
    EXPECT_EQ(msg[42], static_cast<std::uint8_t>(0x00 ^ 0xA4));
    EXPECT_EQ(msg[43], static_cast<std::uint8_t>(0x01 ^ 0x42));

    // MESSAGE-INTEGRITY last.
    EXPECT_EQ(be_u16(msg, 44), 0x0008);
    EXPECT_EQ(be_u16(msg, 46), 20u);
}

TEST(TcpStun, VerifyResponseAcceptsRightKeyAndTransaction) {
    const auto txn = kTxn();
    const auto msg = stun_build_binding_response("aa:bb", "local-secret",
                                                 txn, "192.0.2.1", 9000);

    const auto mapped = stun_verify_binding_response(msg, "local-secret", txn);
    ASSERT_TRUE(mapped.has_value());
    EXPECT_EQ(*mapped, "192.0.2.1:9000");
}

TEST(TcpStun, VerifyResponseRejectsWrongKeyOrTransaction) {
    const auto txn = kTxn();
    const auto msg = stun_build_binding_response("aa:bb", "local-secret",
                                                 txn, "127.0.0.1", 5000);

    EXPECT_FALSE(stun_verify_binding_response(msg, "wrong-secret", txn).has_value());

    std::array<std::uint8_t, 12> other_txn = kTxn();
    other_txn[0] ^= 0xFF;
    EXPECT_FALSE(stun_verify_binding_response(msg, "local-secret", other_txn).has_value());
}

}  // namespace
