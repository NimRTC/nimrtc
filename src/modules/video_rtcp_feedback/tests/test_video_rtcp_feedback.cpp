/**
 * @file src/modules/video_rtcp_feedback/tests/test_video_rtcp_feedback.cpp
 * @brief Unit tests for the video_rtcp_feedback module.
 *
 * Covers:
 *   1.  PLI build      — size, V/P/FMT/PT/length, sender + media SSRCs.
 *   2.  FIR build (1)  — 20 bytes, seq_nr field encoded.
 *   3.  FIR build (2)  — 28 bytes for 2 entries.
 *   4.  SLI build (1)  — 20 bytes.
 *   5.  NACK build (1) — 16 bytes, PID+BLP encoded.
 *   6.  NACK build (3) — 24 bytes for 3 entries.
 *   7.  PLI roundtrip.
 *   8.  FIR roundtrip.
 *   9.  SLI roundtrip.
 *   10. NACK roundtrip.
 *   11. Parsers reject input < 12 bytes.
 *   12. FirCoalescer: rate-limit (250 ms).
 *   13. FirCoalescer: independent per media SSRC; mark_sent updates state.
 *   14. Builders return 0 on undersized output.
 */

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/error.hpp>
#include <nimrtc/video_rtcp_feedback/video_rtcp_feedback.hpp>

namespace {

using nimrtc::core::ByteSpan;
using nimrtc::core::MutableByteSpan;
using nimrtc::core::ErrorCode;
using nimrtc::video_rtcp_feedback::FirCoalescer;
using nimrtc::video_rtcp_feedback::FirEntry;
using nimrtc::video_rtcp_feedback::FirPacket;
using nimrtc::video_rtcp_feedback::kFirEntrySize;
using nimrtc::video_rtcp_feedback::kFmtFir;
using nimrtc::video_rtcp_feedback::kFmtNack;
using nimrtc::video_rtcp_feedback::kFmtPli;
using nimrtc::video_rtcp_feedback::kFmtSli;
using nimrtc::video_rtcp_feedback::kFirMinIntervalUs;
using nimrtc::video_rtcp_feedback::kNackEntrySize;
using nimrtc::video_rtcp_feedback::kNackMinSize;
using nimrtc::video_rtcp_feedback::kPliPacketSize;
using nimrtc::video_rtcp_feedback::kPsfbPt;
using nimrtc::video_rtcp_feedback::kRtpfbPt;
using nimrtc::video_rtcp_feedback::kSliEntrySize;
using nimrtc::video_rtcp_feedback::kSliMinSize;
using nimrtc::video_rtcp_feedback::NackEntry;
using nimrtc::video_rtcp_feedback::NackPacket;
using nimrtc::video_rtcp_feedback::PliPacket;
using nimrtc::video_rtcp_feedback::SliEntry;
using nimrtc::video_rtcp_feedback::SliPacket;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

inline std::uint16_t read_be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(p[0]) << 8) |
         static_cast<std::uint16_t>(p[1]));
}

inline std::uint32_t read_be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8)  |
            static_cast<std::uint32_t>(p[3]);
}

}  // namespace

// ===========================================================================
// Builders
// ===========================================================================

// Test 1: build_pli — 12 bytes, V=2 P=0 FMT=1 PT=206 length=2, SSRCs encoded.
TEST(VideoRtcpFeedback, BuildPliBasics) {
    PliPacket p;
    p.sender_ssrc = 0xAABBCCDDu;
    p.media_ssrc  = 0x11223344u;

    std::vector<std::uint8_t> buf(kPliPacketSize, 0);
    std::size_t written =
        nimrtc::video_rtcp_feedback::build_pli(buf, p);
    ASSERT_EQ(written, kPliPacketSize);

    // byte 0: V=2 (bits 6-7 = 10), P=0, FMT=1
    EXPECT_EQ(buf[0], 0x81u);
    // byte 1: PT=206 (PSFB)
    EXPECT_EQ(buf[1], kPsfbPt);
    // bytes 2-3: length = 2 (12 bytes = 3 words minus 1)
    EXPECT_EQ(read_be16(buf.data() + 2), 2u);

    // sender SSRC
    EXPECT_EQ(read_be32(buf.data() + 4),  0xAABBCCDDu);
    // media  SSRC
    EXPECT_EQ(read_be32(buf.data() + 8),  0x11223344u);
}

// Test 2: build_fir with 1 entry — 20 bytes, FMT=4, PT=206, length=4, seq_nr.
TEST(VideoRtcpFeedback, BuildFirOneEntry) {
    FirPacket p;
    p.sender_ssrc = 0xDEADBEEFu;
    p.entries.push_back({/*ssrc*/ 0xCAFEBABEu, /*seq_nr*/ 7});

    std::vector<std::uint8_t> buf(kPliPacketSize + kFirEntrySize, 0);
    std::size_t written =
        nimrtc::video_rtcp_feedback::build_fir(buf, p);
    ASSERT_EQ(written, 20u);

    EXPECT_EQ(buf[0], 0x84u);              // V=2 P=0 FMT=4
    EXPECT_EQ(buf[1], kPsfbPt);            // PT=206
    EXPECT_EQ(read_be16(buf.data() + 2), 4u);
    EXPECT_EQ(read_be32(buf.data() + 4), 0xDEADBEEFu);
    EXPECT_EQ(read_be32(buf.data() + 8), 0u);   // not used by FIR (no media SSRC)

    // FCI entry @ offset 12:
    EXPECT_EQ(read_be32(buf.data() + 12), 0xCAFEBABEu);
    EXPECT_EQ(buf[16], 0u);                // reserved
    EXPECT_EQ(buf[17], 7u);                // seq_nr
    EXPECT_EQ(buf[18], 0u);                // reserved
    EXPECT_EQ(buf[19], 0u);                // reserved
}

// Test 3: build_fir with 2 entries — 28 bytes.
TEST(VideoRtcpFeedback, BuildFirTwoEntries) {
    FirPacket p;
    p.sender_ssrc = 0x11111111u;
    p.entries.push_back({0x22222222u, 1});
    p.entries.push_back({0x33333333u, 2});

    const std::size_t expected = kPliPacketSize + 2 * kFirEntrySize;
    std::vector<std::uint8_t> buf(expected, 0);
    std::size_t written =
        nimrtc::video_rtcp_feedback::build_fir(buf, p);
    ASSERT_EQ(written, 28u);
    EXPECT_EQ(expected, 28u);

    EXPECT_EQ(buf[0], 0x84u);
    EXPECT_EQ(read_be16(buf.data() + 2), 6u);   // 28B = 7 words, length = 6

    // entry 0
    EXPECT_EQ(read_be32(buf.data() + 12), 0x22222222u);
    EXPECT_EQ(buf[17], 1u);
    // entry 1
    EXPECT_EQ(read_be32(buf.data() + 20), 0x33333333u);
    EXPECT_EQ(buf[25], 2u);
}

// Test 4: build_sli with 1 entry — 20 bytes.
TEST(VideoRtcpFeedback, BuildSliOneEntry) {
    SliPacket p;
    p.sender_ssrc = 0xAAAAAAAAu;
    p.media_ssrc  = 0xBBBBBBBBu;
    p.entries.push_back({/*first*/ 100, /*number*/ 50, /*picture_id*/ 0x42});

    std::vector<std::uint8_t> buf(kSliMinSize + kSliEntrySize, 0);
    std::size_t written =
        nimrtc::video_rtcp_feedback::build_sli(buf, p);
    ASSERT_EQ(written, 20u);

    EXPECT_EQ(buf[0], 0x82u);              // V=2 P=0 FMT=2
    EXPECT_EQ(buf[1], kPsfbPt);
    EXPECT_EQ(read_be16(buf.data() + 2), 4u);

    EXPECT_EQ(read_be32(buf.data() + 4), 0xAAAAAAAAu);
    EXPECT_EQ(read_be32(buf.data() + 8), 0xBBBBBBBBu);

    // FCI @ offset 12
    EXPECT_EQ(read_be16(buf.data() + 12), 100u);
    EXPECT_EQ(read_be16(buf.data() + 14), 50u);
    EXPECT_EQ(read_be32(buf.data() + 16), 0x42u);
}

// Test 5: build_nack with 1 entry — 16 bytes, PID+BLP encoded.
TEST(VideoRtcpFeedback, BuildNackOneEntry) {
    NackPacket p;
    p.sender_ssrc = 0x99887766u;
    p.media_ssrc  = 0x55443322u;
    p.entries.push_back({/*pid*/ 1000, /*blp*/ 0xAAAAu});

    std::vector<std::uint8_t> buf(kNackMinSize + kNackEntrySize, 0);
    std::size_t written =
        nimrtc::video_rtcp_feedback::build_nack(buf, p);
    ASSERT_EQ(written, 16u);

    EXPECT_EQ(buf[0], 0x81u);              // V=2 P=0 FMT=1
    EXPECT_EQ(buf[1], kRtpfbPt);           // PT=205
    EXPECT_EQ(read_be16(buf.data() + 2), 3u);   // 16B = 4 words, length = 3

    EXPECT_EQ(read_be32(buf.data() + 4), 0x99887766u);
    EXPECT_EQ(read_be32(buf.data() + 8), 0x55443322u);

    EXPECT_EQ(read_be16(buf.data() + 12), 1000u);
    EXPECT_EQ(read_be16(buf.data() + 14), 0xAAAAu);
}

// Test 6: build_nack with 3 entries — 24 bytes.
TEST(VideoRtcpFeedback, BuildNackThreeEntries) {
    NackPacket p;
    p.sender_ssrc = 0x01020304u;
    p.media_ssrc  = 0x05060708u;
    p.entries.push_back({100, 0x0001u});
    p.entries.push_back({200, 0xFFFFu});
    p.entries.push_back({300, 0x0000u});

    const std::size_t expected = kNackMinSize + 3 * kNackEntrySize;
    std::vector<std::uint8_t> buf(expected, 0);
    std::size_t written =
        nimrtc::video_rtcp_feedback::build_nack(buf, p);
    ASSERT_EQ(written, 24u);
    EXPECT_EQ(expected, 24u);

    EXPECT_EQ(buf[0], 0x81u);
    EXPECT_EQ(buf[1], kRtpfbPt);
    EXPECT_EQ(read_be16(buf.data() + 2), 5u);   // 24B = 6 words, length = 5

    EXPECT_EQ(read_be16(buf.data() + 12), 100u);
    EXPECT_EQ(read_be16(buf.data() + 14), 0x0001u);
    EXPECT_EQ(read_be16(buf.data() + 16), 200u);
    EXPECT_EQ(read_be16(buf.data() + 18), 0xFFFFu);
    EXPECT_EQ(read_be16(buf.data() + 20), 300u);
    EXPECT_EQ(read_be16(buf.data() + 22), 0x0000u);
}

// ===========================================================================
// Round-trips
// ===========================================================================

TEST(VideoRtcpFeedback, ParsePliRoundtrip) {
    PliPacket src;
    src.sender_ssrc = 0xFEEDFACEu;
    src.media_ssrc  = 0x00C0FFEEu;

    std::vector<std::uint8_t> buf(kPliPacketSize, 0);
    ASSERT_EQ(nimrtc::video_rtcp_feedback::build_pli(buf, src), kPliPacketSize);

    auto r = nimrtc::video_rtcp_feedback::parse_pli(buf);
    ASSERT_TRUE(r.ok()) << r.error().message();
    EXPECT_EQ(r.value().sender_ssrc, 0xFEEDFACEu);
    EXPECT_EQ(r.value().media_ssrc,  0x00C0FFEEu);
}

TEST(VideoRtcpFeedback, ParseFirRoundtrip) {
    FirPacket src;
    src.sender_ssrc = 0x12345678u;
    src.entries.push_back({0xAABBCCDDu, 42});
    src.entries.push_back({0x99887766u, 17});

    std::vector<std::uint8_t> buf(kPliPacketSize + 2 * kFirEntrySize, 0);
    ASSERT_EQ(nimrtc::video_rtcp_feedback::build_fir(buf, src),
              kPliPacketSize + 2 * kFirEntrySize);

    auto r = nimrtc::video_rtcp_feedback::parse_fir(buf);
    ASSERT_TRUE(r.ok()) << r.error().message();
    const auto& got = r.value();
    EXPECT_EQ(got.sender_ssrc, 0x12345678u);
    ASSERT_EQ(got.entries.size(), 2u);
    EXPECT_EQ(got.entries[0].ssrc,   0xAABBCCDDu);
    EXPECT_EQ(got.entries[0].seq_nr, 42u);
    EXPECT_EQ(got.entries[1].ssrc,   0x99887766u);
    EXPECT_EQ(got.entries[1].seq_nr, 17u);
}

TEST(VideoRtcpFeedback, ParseSliRoundtrip) {
    SliPacket src;
    src.sender_ssrc = 0x01010101u;
    src.media_ssrc  = 0x02020202u;
    src.entries.push_back({/*first*/ 7, /*number*/ 11, /*picture_id*/ 0x3Fu});
    src.entries.push_back({/*first*/ 0, /*number*/ 0, /*picture_id*/ 0});

    std::vector<std::uint8_t> buf(kSliMinSize + 2 * kSliEntrySize, 0);
    ASSERT_EQ(nimrtc::video_rtcp_feedback::build_sli(buf, src),
              kSliMinSize + 2 * kSliEntrySize);

    auto r = nimrtc::video_rtcp_feedback::parse_sli(buf);
    ASSERT_TRUE(r.ok()) << r.error().message();
    const auto& got = r.value();
    EXPECT_EQ(got.sender_ssrc, 0x01010101u);
    EXPECT_EQ(got.media_ssrc,  0x02020202u);
    ASSERT_EQ(got.entries.size(), 2u);
    EXPECT_EQ(got.entries[0].first,      7u);
    EXPECT_EQ(got.entries[0].number,     11u);
    EXPECT_EQ(got.entries[0].picture_id, 0x3Fu);
    EXPECT_EQ(got.entries[1].first,      0u);
    EXPECT_EQ(got.entries[1].number,     0u);
    EXPECT_EQ(got.entries[1].picture_id, 0u);
}

TEST(VideoRtcpFeedback, ParseNackRoundtrip) {
    NackPacket src;
    src.sender_ssrc = 0xCAFEF00Du;
    src.media_ssrc  = 0xBADDCAFEu;
    src.entries.push_back({/*pid*/ 0x1000, /*blp*/ 0x8001u});
    src.entries.push_back({/*pid*/ 0x2000, /*blp*/ 0x0000u});
    src.entries.push_back({/*pid*/ 0xFFFF, /*blp*/ 0xFFFFu});

    std::vector<std::uint8_t> buf(kNackMinSize + 3 * kNackEntrySize, 0);
    ASSERT_EQ(nimrtc::video_rtcp_feedback::build_nack(buf, src),
              kNackMinSize + 3 * kNackEntrySize);

    auto r = nimrtc::video_rtcp_feedback::parse_nack(buf);
    ASSERT_TRUE(r.ok()) << r.error().message();
    const auto& got = r.value();
    EXPECT_EQ(got.sender_ssrc, 0xCAFEF00Du);
    EXPECT_EQ(got.media_ssrc,  0xBADDCAFEu);
    ASSERT_EQ(got.entries.size(), 3u);
    EXPECT_EQ(got.entries[0].pid, 0x1000u); EXPECT_EQ(got.entries[0].blp, 0x8001u);
    EXPECT_EQ(got.entries[1].pid, 0x2000u); EXPECT_EQ(got.entries[1].blp, 0x0000u);
    EXPECT_EQ(got.entries[2].pid, 0xFFFFu); EXPECT_EQ(got.entries[2].blp, 0xFFFFu);
}

// ===========================================================================
// Error paths
// ===========================================================================

TEST(VideoRtcpFeedback, ParseRejectsTooShort) {
    // 11 bytes — one less than the minimum 12-byte header.
    std::array<std::uint8_t, 11> short_buf = {
        0x81, 0xCE, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00
    };

    auto pli = nimrtc::video_rtcp_feedback::parse_pli(short_buf);
    ASSERT_FALSE(pli.ok());
    EXPECT_EQ(pli.error().code(), ErrorCode::ProtocolError);

    auto fir = nimrtc::video_rtcp_feedback::parse_fir(short_buf);
    ASSERT_FALSE(fir.ok());
    EXPECT_EQ(fir.error().code(), ErrorCode::ProtocolError);

    auto sli = nimrtc::video_rtcp_feedback::parse_sli(short_buf);
    ASSERT_FALSE(sli.ok());
    EXPECT_EQ(sli.error().code(), ErrorCode::ProtocolError);

    auto nack = nimrtc::video_rtcp_feedback::parse_nack(short_buf);
    ASSERT_FALSE(nack.ok());
    EXPECT_EQ(nack.error().code(), ErrorCode::ProtocolError);

    // Empty input is also rejected.
    std::vector<std::uint8_t> empty;
    auto empty_pli = nimrtc::video_rtcp_feedback::parse_pli(empty);
    ASSERT_FALSE(empty_pli.ok());
    EXPECT_EQ(empty_pli.error().code(), ErrorCode::ProtocolError);
}

// ===========================================================================
// FirCoalescer
// ===========================================================================

// Test 12: should_send returns true the first time, false within 250ms,
// true after the window.
TEST(VideoRtcpFeedback, FirCoalescerRateLimit) {
    FirCoalescer c(/*sender_ssrc*/ 0x10000000u);
    constexpr std::uint32_t kMedia = 0x20000000u;

    // First call for this SSRC — must be allowed.
    EXPECT_TRUE(c.should_send(kMedia, /*now_us*/ 1'000'000));

    // Mark it as sent at t=1s.
    c.mark_sent(kMedia, 1'000'000);

    // Within 250ms — must be denied.
    EXPECT_FALSE(c.should_send(kMedia, 1'000'000 + 100'000));   // +100ms
    EXPECT_FALSE(c.should_send(kMedia, 1'000'000 + 249'999));   // +249.999ms
    EXPECT_FALSE(c.should_send(kMedia, 1'000'000 + kFirMinIntervalUs - 1));

    // Exactly at +250ms — allowed (>= interval).
    EXPECT_TRUE(c.should_send(kMedia, 1'000'000 + kFirMinIntervalUs));
    // After the window — allowed.
    EXPECT_TRUE(c.should_send(kMedia, 1'000'000 + kFirMinIntervalUs + 1));
    EXPECT_TRUE(c.should_send(kMedia, 1'000'000 + 10'000'000));
}

// Test 13: mark_sent updates timestamp; rate-limit state is independent per
// media SSRC.
TEST(VideoRtcpFeedback, FirCoalescerPerSsrcIndependence) {
    FirCoalescer c(/*sender_ssrc*/ 0x30000000u);

    constexpr std::uint32_t kMediaA = 0xA0000000u;
    constexpr std::uint32_t kMediaB = 0xB0000000u;

    // Both initially allowed.
    EXPECT_TRUE(c.should_send(kMediaA, /*t*/ 5'000'000));
    EXPECT_TRUE(c.should_send(kMediaB, /*t*/ 5'000'000));

    // Send to A only.
    c.mark_sent(kMediaA, 5'000'000);

    // A is throttled, B is fresh.
    EXPECT_FALSE(c.should_send(kMediaA, /*t*/ 5'000'000 + 50'000));
    EXPECT_TRUE (c.should_send(kMediaB, /*t*/ 5'000'000 + 50'000));

    // After A's window passes it becomes available again; B is unaffected
    // (B was never marked, so it's still available).
    EXPECT_TRUE(c.should_send(kMediaA, /*t*/ 5'000'000 + kFirMinIntervalUs));
    EXPECT_TRUE(c.should_send(kMediaB, /*t*/ 5'000'000 + kFirMinIntervalUs));

    // make_entry() auto-increments seq_nr per sender.
    FirEntry e0 = c.make_entry(kMediaA);
    FirEntry e1 = c.make_entry(kMediaA);
    FirEntry e2 = c.make_entry(kMediaB);
    EXPECT_EQ(e0.seq_nr, 0u);
    EXPECT_EQ(e1.seq_nr, 1u);
    EXPECT_EQ(e2.seq_nr, 2u);
    EXPECT_EQ(e0.ssrc,   kMediaA);
    EXPECT_EQ(e2.ssrc,   kMediaB);

    // sender_ssrc() reflects what was passed in.
    EXPECT_EQ(c.sender_ssrc(), 0x30000000u);
    // next_seq_nr() reflects the most-recently-issued value + 1 (mod 256).
    EXPECT_EQ(c.next_seq_nr(), 3u);
}

// ===========================================================================
// Buffer-too-small builders
// ===========================================================================

// Test 14: builders return 0 (and do not write past the end) when the
// destination is undersized.
TEST(VideoRtcpFeedback, BuildReturnsZeroOnUndersizedBuffer) {
    // Sentinel: 0xCDCDCDCD lets us detect any partial writes that overrun.
    std::vector<std::uint8_t> sentinel(32, 0xCDu);

    // PLI needs 12 bytes; give it 11.
    {
        PliPacket p{1u, 2u};
        std::vector<std::uint8_t> dst(11, 0xCDu);
        std::size_t n =
            nimrtc::video_rtcp_feedback::build_pli(dst, p);
        EXPECT_EQ(n, 0u);
        // Buffer must be untouched.
        for (auto b : dst) EXPECT_EQ(b, 0xCDu);
    }

    // FIR (1 entry) needs 20 bytes; give it 19.
    {
        FirPacket p; p.sender_ssrc = 1u;
        p.entries.push_back({2u, 3u});
        std::vector<std::uint8_t> dst(19, 0xCDu);
        std::size_t n =
            nimrtc::video_rtcp_feedback::build_fir(dst, p);
        EXPECT_EQ(n, 0u);
        for (auto b : dst) EXPECT_EQ(b, 0xCDu);
    }

    // SLI (1 entry) needs 20 bytes; give it 19.
    {
        SliPacket p; p.sender_ssrc = 1u; p.media_ssrc = 2u;
        p.entries.push_back({/*first*/ 1, /*number*/ 1, /*picture_id*/ 1});
        std::vector<std::uint8_t> dst(19, 0xCDu);
        std::size_t n =
            nimrtc::video_rtcp_feedback::build_sli(dst, p);
        EXPECT_EQ(n, 0u);
        for (auto b : dst) EXPECT_EQ(b, 0xCDu);
    }

    // NACK (1 entry) needs 16 bytes; give it 15.
    {
        NackPacket p; p.sender_ssrc = 1u; p.media_ssrc = 2u;
        p.entries.push_back({/*pid*/ 1, /*blp*/ 0});
        std::vector<std::uint8_t> dst(15, 0xCDu);
        std::size_t n =
            nimrtc::video_rtcp_feedback::build_nack(dst, p);
        EXPECT_EQ(n, 0u);
        for (auto b : dst) EXPECT_EQ(b, 0xCDu);
    }

    // Exactly the right size succeeds (boundary check).
    {
        PliPacket p{0xAAu, 0xBBu};
        std::vector<std::uint8_t> dst(kPliPacketSize, 0xCDu);
        std::size_t n =
            nimrtc::video_rtcp_feedback::build_pli(dst, p);
        EXPECT_EQ(n, kPliPacketSize);
        // Last 4 bytes are the media SSRC (0xBB) big-endian: 0,0,0,0xBB
        // at positions [8..11].
        EXPECT_EQ(dst[8],  0u);
        EXPECT_EQ(dst[9],  0u);
        EXPECT_EQ(dst[10], 0u);
        EXPECT_EQ(dst[11], 0xBBu);
    }
}
