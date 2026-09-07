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
#include <span>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/srtp/srtp.hpp>

namespace {

using namespace nimrtc;
using namespace nimrtc::srtp;

} // namespace

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
    EXPECT_EQ(ctx.get_session(0xDEADBEEF), nullptr);
}

TEST(SrtpContextTest, RemoveSessionIsNoopForUnknownSsrc) {
    SrtpContext ctx;
    ctx.remove_session(0xCAFEBABE);   // must not crash
    EXPECT_EQ(ctx.get_session(0xCAFEBABE), nullptr);
}

TEST(SrtpContextTest, TotalStatsStartAtZero) {
    SrtpContext ctx;
    auto stats = ctx.total_stats();
    EXPECT_EQ(stats.rtp_packets_encrypted, 0u);
    EXPECT_EQ(stats.rtp_packets_decrypted, 0u);
    EXPECT_EQ(stats.decryption_failures, 0u);
}