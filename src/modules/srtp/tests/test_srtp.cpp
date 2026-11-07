// modules/srtp/tests/test_srtp.cpp
// =============================================================================
// SRTP module unit tests.
//
// Covers:
//   - CryptoSuite <-> libsrtp profile id mapping (to_libsrtp_profile)
//   - KeyingMaterial::is_valid() contract
//   - SrtpSession default construction (no keys)
//   - SrtpSession::init_from_master_key rejects undersized keys
//   - SrtpSession::init_from_session_keys requires pre-derived keys
//   - SrtpContext default construction + get_session(nullptr for unknown SSRC)
//   - SrtpContext::remove_session is a no-op on unknown SSRC
//   - SrtpSession::Stats default values
//
// These tests only exercise the public API surface that does NOT depend
// on a live libsrtp2 context; round-trip tests are guarded by
// NIMRTC_USE_LIBSRTP at the call site if/when added.
// =============================================================================

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/srtp/srtp.hpp>
#include <nimrtc/srtp/libsrtp_backend.hpp>

namespace {

using namespace nimrtc;
using namespace nimrtc::srtp;

constexpr std::uint32_t kTestSsrc = 0x12345678u;

// Minimal 12-byte RTP header (V=2) + 32-byte payload.
std::vector<std::uint8_t> make_min_rtp() {
    std::vector<std::uint8_t> pkt(12 + 32, 0);
    pkt[0] = 0x80;
    pkt[1] = 111;
    pkt[2] = 0xAB;
    pkt[3] = 0xCD;
    pkt[4] = 0xDE;
    pkt[5] = 0xAD;
    pkt[6] = 0xBE;
    pkt[7] = 0xEF;
    pkt[8] = static_cast<std::uint8_t>((kTestSsrc >> 24) & 0xFFu);
    pkt[9] = static_cast<std::uint8_t>((kTestSsrc >> 16) & 0xFFu);
    pkt[10] = static_cast<std::uint8_t>((kTestSsrc >> 8) & 0xFFu);
    pkt[11] = static_cast<std::uint8_t>(kTestSsrc & 0xFFu);
    for (std::size_t i = 12; i < pkt.size(); ++i) pkt[i] = static_cast<std::uint8_t>(i);
    return pkt;
}

}  // namespace

// -----------------------------------------------------------------------------
// CryptoSuite mapping
// -----------------------------------------------------------------------------
TEST(SrtpCryptoSuiteTest, ToLibsrtpProfileDistinct) {
    auto aes128_80 = to_libsrtp_profile(CryptoSuite::Aes128CmSha1_80);
    auto aes128_32 = to_libsrtp_profile(CryptoSuite::Aes128CmSha1_32);
    auto aes128_gcm = to_libsrtp_profile(CryptoSuite::Aes128Gcm_128);
    auto aes256_80 = to_libsrtp_profile(CryptoSuite::Aes256CmSha1_80);

    EXPECT_NE(aes128_80, aes128_32);
    EXPECT_NE(aes128_80, aes128_gcm);
    EXPECT_NE(aes128_80, aes256_80);
    EXPECT_NE(aes128_32, aes128_gcm);
    EXPECT_NE(aes128_32, aes256_80);
    EXPECT_NE(aes128_gcm, aes256_80);

    // All non-zero.
    EXPECT_NE(aes128_80,  0u);
    EXPECT_NE(aes128_32,  0u);
    EXPECT_NE(aes128_gcm, 0u);
    EXPECT_NE(aes256_80,  0u);
}

// -----------------------------------------------------------------------------
// KeyingMaterial
// -----------------------------------------------------------------------------
TEST(SrtpKeyingMaterialTest, IsValidRequiresMasterKey) {
    KeyingMaterial km;
    EXPECT_FALSE(km.is_valid());

    km.master_key = {0x01, 0x02, 0x03};  // too short
    EXPECT_FALSE(km.is_valid());

    km.master_key = std::vector<std::uint8_t>(16, 0xAA);
    km.master_salt = std::vector<std::uint8_t>(14, 0xBB);
    EXPECT_TRUE(km.is_valid());
}

// -----------------------------------------------------------------------------
// SrtpSession
// -----------------------------------------------------------------------------
TEST(SrtpSessionTest, DefaultConstructedIsEmpty) {
    SrtpSession s;
    auto stats = s.stats();
    EXPECT_EQ(stats.rtp_packets_encrypted, 0u);
    EXPECT_EQ(stats.rtp_packets_decrypted, 0u);
    EXPECT_EQ(stats.rtcp_packets_encrypted, 0u);
    EXPECT_EQ(stats.rtcp_packets_decrypted, 0u);
    EXPECT_EQ(stats.decryption_failures, 0u);
    EXPECT_EQ(stats.replay_attacks_dropped, 0u);
}

TEST(SrtpSessionTest, InitFromMasterKeyRejectsUndersizedKey) {
    Config c;
    c.suite = CryptoSuite::Aes128CmSha1_80;
    SrtpSession s;

    std::vector<std::uint8_t> tiny_key(8, 0xAA);          // < 16
    std::vector<std::uint8_t> salt(14, 0xBB);
    auto rc = s.init_from_master_key(c, tiny_key, salt);
    EXPECT_FALSE(rc.ok());
}

TEST(SrtpSessionTest, InitFromSessionKeysRequiresConfig) {
    Config c;
    // No session_key_rtp filled in -> init must fail.
    SrtpSession s;
    auto rc = s.init_from_session_keys(c);
    EXPECT_FALSE(rc.ok());
}

// -----------------------------------------------------------------------------
// SrtpContext
// -----------------------------------------------------------------------------
TEST(SrtpContextTest, GetSessionReturnsNullForUnknownSsrc) {
    SrtpContext ctx;
    EXPECT_EQ(ctx.get_session(0xDEADBEEF, /*outgoing=*/true), nullptr);
}

TEST(SrtpContextTest, RemoveSessionIsNoopForUnknownSsrc) {
    SrtpContext ctx;
    ctx.remove_session(0xCAFEBABE);   // must not crash
    EXPECT_EQ(ctx.get_session(0xCAFEBABE, /*outgoing=*/true), nullptr);
}

TEST(SrtpContextTest, TotalStatsStartAtZero) {
    SrtpContext ctx;
    auto stats = ctx.total_stats();
    EXPECT_EQ(stats.rtp_packets_encrypted, 0u);
    EXPECT_EQ(stats.rtp_packets_decrypted, 0u);
    EXPECT_EQ(stats.decryption_failures, 0u);
}

// -----------------------------------------------------------------------------
// Backend seam (v1.1 X2): libsrtp routed via ISrtpSessionBackend.
// Behaviour must match SrtpSession; SM4-GCM stays out of scope.
// -----------------------------------------------------------------------------
TEST(SrtpBackendSeamTest, FactoryIdIsLibsrtp) {
    LibsrtpBackendFactory factory;
    EXPECT_EQ(factory.id(), "libsrtp");
    EXPECT_FALSE(factory.name().empty());
    auto backend = factory.create();
    ASSERT_NE(backend, nullptr);
}

TEST(SrtpBackendSeamTest, RoundTripThroughSeam) {
    LibsrtpBackendFactory factory;
    auto sender = factory.create();
    auto receiver = factory.create();
    ASSERT_NE(sender, nullptr);
    ASSERT_NE(receiver, nullptr);

    std::vector<std::uint8_t> key(16, 0xAA);
    std::vector<std::uint8_t> salt(14, 0xBB);
    const std::uint32_t suite = to_libsrtp_profile(CryptoSuite::Aes128CmSha1_80);
    auto r1 = sender->init_outbound(key, salt, suite);
    auto r2 = receiver->init_inbound(key, salt, suite);
    ASSERT_TRUE(r1.ok());
    ASSERT_TRUE(r2.ok());

    auto pkt = make_min_rtp();
    auto enc = sender->protect_rtp(core::ByteSpan(pkt.data(), pkt.size()));
    ASSERT_TRUE(enc.ok());
    std::vector<std::uint8_t> wire(enc.value().begin(), enc.value().end());
    EXPECT_GT(wire.size(), pkt.size());

    auto dec = receiver->unprotect_rtp(core::ByteSpan(wire.data(), wire.size()));
    ASSERT_TRUE(dec.ok());
    EXPECT_EQ(dec.value().size(), pkt.size());
    EXPECT_EQ(0, std::memcmp(dec.value().data(), pkt.data(), pkt.size()));
}

TEST(SrtpBackendSeamTest, EmptyKeyFails) {
    LibsrtpBackendFactory factory;
    auto backend = factory.create();
    ASSERT_NE(backend, nullptr);
    std::vector<std::uint8_t> empty;
    const std::uint32_t suite = to_libsrtp_profile(CryptoSuite::Aes128CmSha1_80);
    EXPECT_FALSE(backend->init_outbound(empty, empty, suite).ok());
    EXPECT_FALSE(backend->init_inbound(empty, empty, suite).ok());
}