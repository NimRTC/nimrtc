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

// The seam takes a `plugins::SrtpCryptoSuite` id, NOT a libsrtp
// `srtp_profile_t`.  The two numbering schemes differ (see the table on
// `LibsrtpSessionBackend::init_outbound`), so the id is spelled out
// here rather than taken from `to_libsrtp_profile()` -- that function
// returns the *libsrtp* id and is a different thing entirely.
//
// Before this was made explicit, the test passed
// `to_libsrtp_profile(Aes128CmSha1_80)` (== 2) where the seam expects
// 0.  The backend's `to_lib_profile()` maps 0 -> AES-128-CM-SHA1-80 and
// 2 -> AES-128-GCM, so the session was built with the wrong protection
// profile -- and GCM rejects the 16-byte master key, so `init_outbound`
// failed outright.
constexpr std::uint32_t kSeamSuiteAes128CmSha1_80 = 0;

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
    const std::uint32_t suite = kSeamSuiteAes128CmSha1_80;
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
    EXPECT_FALSE(backend->init_outbound(empty, empty, kSeamSuiteAes128CmSha1_80).ok());
    EXPECT_FALSE(backend->init_inbound(empty, empty, kSeamSuiteAes128CmSha1_80).ok());
}

TEST(SrtpBackendSeamTest, AnInvalidSuiteIdIsRejected) {
    // The seam ids are 0..3.  Anything else -- most importantly a
    // libsrtp `srtp_profile_t` (6 or 10, which are *not* in the seam
    // range) -- must be refused, not silently mapped onto some other
    // protection profile.
    //
    // Ids 2 and 3 happen to coincide with libsrtp profile ids 2 and 3
    // (AES-128-CM-SHA1-80 and AES-128-CM-SHA1-32).  The numbering
    // schemes are different in general, but for those two values the
    // numbers coincide, so they cannot be rejected on range grounds --
    // they are still *valid* seam ids, and are covered by the
    // "each valid id installs" check below.
    LibsrtpBackendFactory factory;
    auto backend = factory.create();
    ASSERT_NE(backend, nullptr);

    std::vector<std::uint8_t> key(16, 0xAA);
    std::vector<std::uint8_t> salt(14, 0xBB);

    for (std::uint32_t bad : {4u, 5u, 6u, 10u, 0xFFFFFFFFu}) {
        EXPECT_FALSE(backend->init_outbound(key, salt, bad).ok())
            << "suite id " << bad << " must be rejected";
        EXPECT_FALSE(backend->init_inbound(key, salt, bad).ok())
            << "suite id " << bad << " must be rejected";
    }
}

TEST(SrtpBackendSeamTest, EveryValidSuiteIdInstalls) {
    // Each valid seam id must produce a working session with the key
    // and salt length its profile calls for:
    //
    //     id 0  AES-128-CM-SHA1-80   16-byte key, 14-byte salt
    //     id 1  AES-128-CM-SHA1-32   16-byte key, 14-byte salt
    //     id 2  AES-128-GCM          16-byte key, 12-byte salt
    //     id 3  AES-256-CM-SHA1-80   32-byte key, 14-byte salt
    //
    // AES-128-GCM is the exception: the vendored libsrtp is built
    // against its "internal" crypto backend, which ships AES-CM only.
    // The suite id is valid, but the build cannot provide the
    // profile -- so the backend must refuse it with a clear reason
    // rather than failing deep inside libsrtp.  (If the vendored build
    // is ever switched to a backend with GCM, this expectation flips
    // to "installs".)
    struct Case {
        std::uint32_t suite;
        std::size_t key_len;
        std::size_t salt_len;
    };
    const Case cases[] = {
        {0u, 16u, 14u},
        {1u, 16u, 14u},
        {3u, 32u, 14u},
    };

    LibsrtpBackendFactory factory;
    for (const Case& c : cases) {
        auto sender = factory.create();
        auto receiver = factory.create();
        ASSERT_NE(sender, nullptr);
        ASSERT_NE(receiver, nullptr);

        std::vector<std::uint8_t> key(c.key_len, 0xAA);
        std::vector<std::uint8_t> salt(c.salt_len, 0xBB);

        auto r1 = sender->init_outbound(key, salt, c.suite);
        auto r2 = receiver->init_inbound(key, salt, c.suite);
        EXPECT_TRUE(r1.ok()) << "suite id " << c.suite << ": " << r1.error().message();
        EXPECT_TRUE(r2.ok()) << "suite id " << c.suite << ": " << r2.error().message();
    }
}

TEST(SrtpBackendSeamTest, TheGcmSuiteIsRefusedWithAReason) {
    // AES-128-GCM cannot be provided by this build.  The refusal must
    // say *why*, so an operator can tell a missing capability apart
    // from a broken key.
    LibsrtpBackendFactory factory;
    auto backend = factory.create();
    ASSERT_NE(backend, nullptr);

    std::vector<std::uint8_t> key(16, 0xAA);
    std::vector<std::uint8_t> salt(12, 0xBB);

    auto r = backend->init_outbound(key, salt, 2u);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.error().message().find("GCM"), std::string::npos)
        << "the error must name the suite: " << r.error().message();
}

// -----------------------------------------------------------------------------
// The scratch buffers (Phase 3 / C5)
//
// The libsrtp backend keeps one scratch per direction and grows it to a
// high-water mark.  Two properties matter and are easy to break:
//
//   1. The returned span's length must be the *actual* crypto output
//      length, never the scratch's capacity.  When the scratch was
//      grown by a previous, larger packet, returning its size would
//      hand the caller trailing garbage -- and for `unprotect_rtp` it
//      would hand back ciphertext-tag bytes as if they were plaintext.
//
//   2. A smaller packet after a larger one must still work: the
//      scratch is not shrunk, so the length has to come from the
//      crypto call, not from the vector.
//
// These are the exact regressions the high-water-mark rewrite could
// introduce, so they are pinned here.
// -----------------------------------------------------------------------------
TEST(SrtpScratchTest, ProtectReturnsTheExactCiphertextLength) {
    LibsrtpBackendFactory factory;
    auto sender = factory.create();
    auto receiver = factory.create();
    ASSERT_NE(sender, nullptr);
    ASSERT_NE(receiver, nullptr);

    std::vector<std::uint8_t> key(16, 0xAA);
    std::vector<std::uint8_t> salt(14, 0xBB);
    const std::uint32_t suite = kSeamSuiteAes128CmSha1_80;
    ASSERT_TRUE(sender->init_outbound(key, salt, suite).ok());
    ASSERT_TRUE(receiver->init_inbound(key, salt, suite).ok());

    // A big packet first: this grows the scratch to its high-water mark.
    std::vector<std::uint8_t> big(12 + 512, 0x5A);
    big[0] = 0x80;
    auto enc_big = sender->protect_rtp(core::ByteSpan(big.data(), big.size()));
    ASSERT_TRUE(enc_big.ok());
    EXPECT_EQ(enc_big.value().size(), big.size() + 10u);   // + the auth tag

    // Now a small packet through the *same* backend: the scratch is
    // still large, so a length taken from the scratch would be wrong.
    auto small = make_min_rtp();
    auto enc_small = sender->protect_rtp(core::ByteSpan(small.data(), small.size()));
    ASSERT_TRUE(enc_small.ok());
    EXPECT_EQ(enc_small.value().size(), small.size() + 10u);

    // And it must still decrypt to the original plaintext.
    std::vector<std::uint8_t> wire(enc_small.value().begin(), enc_small.value().end());
    auto dec = receiver->unprotect_rtp(core::ByteSpan(wire.data(), wire.size()));
    ASSERT_TRUE(dec.ok());
    EXPECT_EQ(dec.value().size(), small.size());
    EXPECT_EQ(0, std::memcmp(dec.value().data(), small.data(), small.size()));
}

TEST(SrtpScratchTest, UnprotectReturnsTheExactPlaintextLength) {
    LibsrtpBackendFactory factory;
    auto sender = factory.create();
    auto receiver = factory.create();
    ASSERT_NE(sender, nullptr);
    ASSERT_NE(receiver, nullptr);

    std::vector<std::uint8_t> key(16, 0xAA);
    std::vector<std::uint8_t> salt(14, 0xBB);
    const std::uint32_t suite = kSeamSuiteAes128CmSha1_80;
    ASSERT_TRUE(sender->init_outbound(key, salt, suite).ok());
    ASSERT_TRUE(receiver->init_inbound(key, salt, suite).ok());

    // Grow the receiver's scratch with a large packet.
    std::vector<std::uint8_t> big(12 + 512, 0x5A);
    big[0] = 0x80;
    auto enc_big = sender->protect_rtp(core::ByteSpan(big.data(), big.size()));
    ASSERT_TRUE(enc_big.ok());
    std::vector<std::uint8_t> wire_big(enc_big.value().begin(), enc_big.value().end());
    auto dec_big = receiver->unprotect_rtp(
        core::ByteSpan(wire_big.data(), wire_big.size()));
    ASSERT_TRUE(dec_big.ok());
    EXPECT_EQ(dec_big.value().size(), big.size());

    // A small packet next.  The scratch is still 512+ bytes, so the
    // returned length must be the plaintext length -- not the scratch.
    auto small = make_min_rtp();
    auto enc_small = sender->protect_rtp(core::ByteSpan(small.data(), small.size()));
    ASSERT_TRUE(enc_small.ok());
    std::vector<std::uint8_t> wire(enc_small.value().begin(), enc_small.value().end());
    auto dec = receiver->unprotect_rtp(core::ByteSpan(wire.data(), wire.size()));
    ASSERT_TRUE(dec.ok());
    EXPECT_EQ(dec.value().size(), small.size());
    EXPECT_EQ(0, std::memcmp(dec.value().data(), small.data(), small.size()));
}

TEST(SrtpScratchTest, RtcpProtectAndUnprotectRoundTrip) {
    LibsrtpBackendFactory factory;
    auto sender = factory.create();
    auto receiver = factory.create();
    ASSERT_NE(sender, nullptr);
    ASSERT_NE(receiver, nullptr);

    std::vector<std::uint8_t> key(16, 0xAA);
    std::vector<std::uint8_t> salt(14, 0xBB);
    const std::uint32_t suite = kSeamSuiteAes128CmSha1_80;
    ASSERT_TRUE(sender->init_outbound(key, salt, suite).ok());
    ASSERT_TRUE(receiver->init_inbound(key, salt, suite).ok());

    // A receiver-report-shaped RTCP packet: 8-byte header + 4 bytes.
    // The length field counts 32-bit words MINUS ONE, so a 12-byte
    // packet carries 0x0002.  libsrtp validates that field, and an
    // inconsistent one is rejected as a malformed packet.
    std::vector<std::uint8_t> rtcp = {0x81, 200, 0x00, 0x02, 0x11, 0x22, 0x33, 0x44,
                                      0x55, 0x66, 0x77, 0x88};

    auto enc = sender->protect_rtcp(core::ByteSpan(rtcp.data(), rtcp.size()));
    ASSERT_TRUE(enc.ok());
    // SRTCP = the RTCP packet + a 4-byte index trailer + the 10-byte
    // HMAC-SHA1-80 auth tag.
    EXPECT_EQ(enc.value().size(), rtcp.size() + 4u + 10u);

    std::vector<std::uint8_t> wire(enc.value().begin(), enc.value().end());
    auto dec = receiver->unprotect_rtcp(core::ByteSpan(wire.data(), wire.size()));
    ASSERT_TRUE(dec.ok());
    EXPECT_EQ(dec.value().size(), rtcp.size());
    EXPECT_EQ(0, std::memcmp(dec.value().data(), rtcp.data(), rtcp.size()));
}