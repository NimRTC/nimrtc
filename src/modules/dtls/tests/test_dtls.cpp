// modules/dtls/tests/test_dtls.cpp
// =============================================================================
// DTLS module unit tests.
//
// Covers:
//   - DtlsSession lifecycle (open/close)
//   - State machine: Closed -> Initial after open()
//   - Local fingerprint generation (SHA-256, 32 bytes, base64 + hex_colon)
//   - set_peer_fingerprint / set_role don't change advertised local FP
//   - Stats initialisation
//   - tls_prf_p_sha256 / tls12_prf against a known short vector
// =============================================================================

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <nimrtc/dtls/dtls.hpp>
#include <nimrtc/dtls/dtls_prf.hpp>

namespace {

using namespace nimrtc;
using namespace nimrtc::dtls;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
inline std::span<const std::uint8_t> as_bytes(std::vector<std::uint8_t>& v) {
    return std::span<const std::uint8_t>(v.data(), v.size());
}

} // namespace

// -----------------------------------------------------------------------------
// DtlsSession lifecycle
// -----------------------------------------------------------------------------
TEST(DtlsSessionTest, DefaultConstructorDoesNotOpen) {
    Config c;
    DtlsSession s(c);
    EXPECT_EQ(s.state(), DtlsState::Closed);
    EXPECT_FALSE(s.is_connected());
}

TEST(DtlsSessionTest, OpenTransitionsToInitial) {
    Config c;
    c.role = DtlsRole::Server;
    DtlsSession s(c);
    auto rc = s.open();
    ASSERT_TRUE(rc.ok()) << rc.error().message();
    // Initial state after open() with no inbound traffic.
    EXPECT_TRUE(s.state() == DtlsState::Initial ||
                s.state() == DtlsState::HelloSent)
        << "state=" << static_cast<int>(s.state());
}

TEST(DtlsSessionTest, LocalFingerprintGenerated) {
    Config c;
    c.role = DtlsRole::Server;
    DtlsSession s(c);
    ASSERT_TRUE(s.open().ok());

    const auto& fp = s.local_fingerprint();
    EXPECT_EQ(fp.algorithm, "sha-256");
    EXPECT_EQ(fp.bytes.size(), 32u);          // SHA-256 -> 32 bytes
    EXPECT_FALSE(fp.hex_colon.empty());      // colon-separated UPPER hex
    EXPECT_FALSE(fp.base64.empty());

    // hex_colon must contain only 0-9 A-F :
    for (char ch : fp.hex_colon) {
        const bool ok = (ch >= '0' && ch <= '9') ||
                        (ch >= 'A' && ch <= 'F') ||
                        (ch == ':');
        EXPECT_TRUE(ok) << "unexpected char: " << ch;
    }
}

TEST(DtlsSessionTest, StatsInitialisedToZero) {
    Config c;
    DtlsSession s(c);
    auto stats = s.stats();
    EXPECT_EQ(stats.records_in, 0u);
    EXPECT_EQ(stats.records_out, 0u);
    EXPECT_EQ(stats.handshake_ms, 0u);
    EXPECT_EQ(stats.retransmits, 0u);
    EXPECT_EQ(stats.alerts_in, 0u);
    EXPECT_EQ(stats.alerts_out, 0u);
    EXPECT_EQ(stats.errors, 0u);
}

TEST(DtlsSessionTest, SetPeerFingerprintDoesNotChangeLocal) {
    Config c;
    c.role = DtlsRole::Server;
    DtlsSession s(c);
    ASSERT_TRUE(s.open().ok());

    auto local_before = s.local_fingerprint().bytes;

    std::vector<std::uint8_t> peer_fp(32, 0xAB);
    s.set_peer_fingerprint("sha-256", peer_fp);

    auto local_after = s.local_fingerprint().bytes;
    EXPECT_EQ(local_before, local_after);
}

TEST(DtlsSessionTest, StateNameReturnsNonEmpty) {
    EXPECT_STREQ(DtlsSession::state_name(DtlsState::Closed),    "Closed");
    EXPECT_STREQ(DtlsSession::state_name(DtlsState::Initial),  "Initial");
    EXPECT_STREQ(DtlsSession::state_name(DtlsState::Connected),"Connected");
    EXPECT_STREQ(DtlsSession::state_name(DtlsState::Failed),   "Failed");
}

TEST(DtlsSessionTest, SrtpKeyingMaterialEmptyBeforeConnected) {
    Config c;
    DtlsSession s(c);
    ASSERT_TRUE(s.open().ok());
    // Without a real handshake, srtp_keying_material() must be empty.
    EXPECT_FALSE(s.srtp_keying_material().has_value());
}

TEST(DtlsSessionTest, FeedInboundReturnsConsumedCount) {
    Config c;
    c.role = DtlsRole::Server;
    DtlsSession s(c);
    ASSERT_TRUE(s.open().ok());

    // Garbage inbound bytes should be consumed (and dropped as bad data),
    // not crash. The exact return value depends on whether the parser
    // accepts anything; we just assert it's <= input size.
    std::vector<std::uint8_t> garbage(64, 0xFF);
    DtlsAddr from{"127.0.0.1", 1234};
    auto consumed = s.feed_inbound(as_bytes(garbage), from);
    EXPECT_LE(consumed, garbage.size());
}

// -----------------------------------------------------------------------------
// PRF — short-vector sanity check (not RFC KAT, just shape/size sanity).
// -----------------------------------------------------------------------------
TEST(DtlsPrfTest, PHashProducesExactLength) {
    std::vector<std::uint8_t> secret = {0x01, 0x02, 0x03, 0x04};
    std::vector<std::uint8_t> label  = {'t', 'e', 's', 't'};
    std::vector<std::uint8_t> seed1  = {0xAA, 0xBB};
    std::vector<std::uint8_t> seed2  = {0xCC, 0xDD};

    auto out = tls_prf_p_sha256(secret, label, seed1, seed2, 48);
    ASSERT_EQ(out.size(), 48u);

    // Deterministic for the same inputs.
    auto out2 = tls_prf_p_sha256(secret, label, seed1, seed2, 48);
    EXPECT_EQ(out, out2);

    // Different length truncates.
    auto out3 = tls_prf_p_sha256(secret, label, seed1, seed2, 16);
    ASSERT_EQ(out3.size(), 16u);
    EXPECT_EQ(std::vector<std::uint8_t>(out.begin(), out.begin() + 16), out3);
}

TEST(DtlsPrfTest, Tls12PrfProducesExactLength) {
    std::vector<std::uint8_t> secret = {0x01, 0x02, 0x03, 0x04};
    std::vector<std::uint8_t> seed   = {0xAA, 0xBB};

    auto out = tls12_prf(secret, "test label", seed, 32);
    EXPECT_EQ(out.size(), 32u);

    auto out2 = tls12_prf(secret, "test label", seed, 32);
    EXPECT_EQ(out, out2);

    // Different label -> different output.
    auto out3 = tls12_prf(secret, "other", seed, 32);
    EXPECT_NE(out, out3);
}