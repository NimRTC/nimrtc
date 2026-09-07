/**
 * @file src/modules/opus/tests/test_opus_packetise.cpp
 * @brief Unit tests for RFC 7587 §4 RTP packetisation (packetise/depacketise).
 *
 * Verifies:
 *   1. TOC config field selection per RFC 6716 §3.1 Table 2.
 *   2. Code 0 (single frame) round-trip + TOC byte layout.
 *   3. Code 1 (two equal frames) round-trip + [R3] even-length rule.
 *   4. Code 2 (two unequal frames) round-trip + 1- and 2-byte length encoding
 *      per RFC 6716 §3.2.1.
 *   5. Code 3 VBR (M >= 3 frames) round-trip + length table.
 *   6. Code 3 CBR (M frames, equal per-frame size) round-trip.
 *   7. Stereo TOC byte (s field) boundaries: 0 (mono), 3 (LR).
 *   8. Capacity-too-small returns 0 and writes nothing.
 *   9. Mismatched frame_size_ms / s_field across frames is rejected.
 *  10. Frames exceeding 120 ms total duration are rejected (RFC 6716 [R5]).
 *  11. depacketise() rejects malformed payloads (odd-length Code 1,
 *      truncated Code 2 length, Code 3 R = M * per divisibility, etc.).
 *
 * Tests use synthetic byte payloads (not real Opus-encoded audio) — the
 * packetiser is responsible only for framing, not the codec bitstream.
 */

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include <nimrtc/opus/opus.hpp>

namespace nimrtc::opus::test {

using ::testing::Test;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

// Fill a frame buffer with a deterministic pattern so we can compare.
void fill_pattern(std::uint8_t* dst, std::size_t n, std::uint8_t seed) {
    for (std::size_t i = 0; i < n; ++i) {
        dst[i] = static_cast<std::uint8_t>((seed + i) & 0xFF);
    }
}

bool bytes_equal(const std::uint8_t* a, const std::uint8_t* b, std::size_t n) {
    return std::memcmp(a, b, n) == 0;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// TOC config field selection (RFC 6716 §3.1 Table 2)
// ---------------------------------------------------------------------------

TEST(OpusPacketiseToc, TocConfigForFrameSize) {
    EXPECT_EQ(toc_config_for_frame_size_ms(3).value_or(99),  28);  // 2.5 ms → FB CELT
    EXPECT_EQ(toc_config_for_frame_size_ms(5).value_or(99),  29);
    EXPECT_EQ(toc_config_for_frame_size_ms(10).value_or(99), 30);
    EXPECT_EQ(toc_config_for_frame_size_ms(20).value_or(99), 31);
    EXPECT_EQ(toc_config_for_frame_size_ms(40).value_or(99), 10);
    EXPECT_EQ(toc_config_for_frame_size_ms(60).value_or(99), 11);
    EXPECT_FALSE(toc_config_for_frame_size_ms(7).has_value());
    EXPECT_FALSE(toc_config_for_frame_size_ms(0).has_value());
    EXPECT_FALSE(toc_config_for_frame_size_ms(123).has_value());
}

TEST(OpusPacketiseToc, TocByteLayout) {
    // config=31, s=1, code=0 → 0b11111_1_00 = 0xFC
    EXPECT_EQ(make_toc_byte(31, 1, 0), 0xFC);
    // config=0, s=0, code=0 → 0b00000_0_00 = 0x00
    EXPECT_EQ(make_toc_byte(0, 0, 0), 0x00);
    // config=31, s=1, code=3 → 0b11111_1_11 = 0xFF
    EXPECT_EQ(make_toc_byte(31, 1, 3), 0xFF);
    // config=10, s=0, code=0 → 0b01010_0_00 = 0x50 (SILK WB 40 ms, mono)
    EXPECT_EQ(make_toc_byte(10, 0, 0), 0x50);
    // Verify field isolation: changing code only flips bits 1:0.
    EXPECT_EQ(make_toc_byte(31, 0, 0) & 0xFC, make_toc_byte(31, 0, 3) & 0xFC);
    EXPECT_EQ(make_toc_byte(31, 0, 0) & 0x03, 0);
    EXPECT_EQ(make_toc_byte(31, 0, 3) & 0x03, 3);
}

// ---------------------------------------------------------------------------
// Code 0: single frame (RFC 6716 §3.2.2)
// ---------------------------------------------------------------------------

TEST(OpusPacketiseCode0, SingleFrameMonoRoundTrip) {
    std::uint8_t frame[64];
    fill_pattern(frame, sizeof(frame), /*seed=*/0xA0);

    CodecFrame in;
    in.data         = frame;
    in.size         = sizeof(frame);
    in.frame_size_ms = 20;
    in.is_stereo    = 0;

    std::uint8_t payload[256];
    std::size_t written = packetise(&in, 1, 48000, /*cbr=*/false,
                                    payload, sizeof(payload));
    ASSERT_EQ(written, 1 + sizeof(frame));
    // TOC = (31 << 3) | (0 << 2) | 0 = 0xF8
    EXPECT_EQ(payload[0], 0xF8);
    EXPECT_TRUE(bytes_equal(payload + 1, frame, sizeof(frame)));

    // Round-trip through depacketise.
    PacketView views[4];
    std::size_t n = 0;
    std::size_t got = depacketise(payload, written, views, 4, &n);
    EXPECT_EQ(got, 1u);
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(views[0].size, sizeof(frame));
    EXPECT_EQ(views[0].config, 31);
    EXPECT_EQ(views[0].s, 0);
    EXPECT_EQ(views[0].code, 0);
    EXPECT_EQ(views[0].frame_size_ms, 20);
    EXPECT_TRUE(bytes_equal(views[0].data, frame, sizeof(frame)));
}

TEST(OpusPacketiseCode0, SingleFrameStereoRoundTrip) {
    std::uint8_t frame[80];
    fill_pattern(frame, sizeof(frame), 0x55);

    CodecFrame in;
    in.data          = frame;
    in.size          = sizeof(frame);
    in.frame_size_ms = 10;
    in.is_stereo     = 1;  // TOC s = 1 (interleaved L/R stereo)

    std::uint8_t payload[256];
    std::size_t written = packetise(&in, 1, 48000, false,
                                    payload, sizeof(payload));
    ASSERT_EQ(written, 1 + sizeof(frame));
    // TOC = (30 << 3) | (1 << 2) | 0 = 0xF0 | 0x04 | 0 = 0xF4
    EXPECT_EQ(payload[0], 0xF4);
    EXPECT_TRUE(bytes_equal(payload + 1, frame, sizeof(frame)));

    PacketView views[4];
    std::size_t n = 0;
    std::size_t got = depacketise(payload, written, views, 4, &n);
    EXPECT_EQ(got, 1u);
    EXPECT_EQ(views[0].s, 1);
    EXPECT_EQ(views[0].frame_size_ms, 10);
    EXPECT_EQ(views[0].size, sizeof(frame));
}

// ---------------------------------------------------------------------------
// Code 1: two frames, equal compressed size (RFC 6716 §3.2.3)
// ---------------------------------------------------------------------------

TEST(OpusPacketiseCode1, TwoEqualFramesRoundTrip) {
    std::uint8_t f0[40];
    std::uint8_t f1[40];
    fill_pattern(f0, sizeof(f0), 0x10);
    fill_pattern(f1, sizeof(f1), 0x80);

    CodecFrame frames[2] = {
        { f0, sizeof(f0), 20, 0 },
        { f1, sizeof(f1), 20, 0 },
    };

    std::uint8_t payload[256];
    std::size_t written = packetise(frames, 2, 48000, /*cbr=*/true,
                                    payload, sizeof(payload));
    ASSERT_EQ(written, 1 + 2 * sizeof(f0));
    // TOC code 1 → bits 1:0 = 0b01
    EXPECT_EQ(payload[0] & 0x03, 1);
    EXPECT_TRUE(bytes_equal(payload + 1,         f0, sizeof(f0)));
    EXPECT_TRUE(bytes_equal(payload + 1 + sizeof(f0), f1, sizeof(f1)));

    PacketView views[4];
    std::size_t n = 0;
    std::size_t got = depacketise(payload, written, views, 4, &n);
    EXPECT_EQ(got, 2u);
    EXPECT_EQ(views[0].size, sizeof(f0));
    EXPECT_EQ(views[1].size, sizeof(f1));
    EXPECT_EQ(views[0].code, 1);
    EXPECT_EQ(views[1].code, 1);
    EXPECT_TRUE(bytes_equal(views[0].data, f0, sizeof(f0)));
    EXPECT_TRUE(bytes_equal(views[1].data, f1, sizeof(f1)));
}

TEST(OpusPacketiseCode1, OddPayloadLengthRejected) {
    // Craft a Code 1 packet with an odd total length — must be rejected [R3].
    std::uint8_t payload[11] = { 0xF9 };  // TOC: config=31, s=1, code=1
    // payload_len = 11 → after_toc = 10 → even, so this passes [R3]. Good.
    // Now craft an odd-length one:
    std::uint8_t odd[8] = { 0xF9 };
    PacketView views[4];
    std::size_t n = 0;
    // after_toc = 7 → odd → reject.
    EXPECT_EQ(depacketise(odd, sizeof(odd), views, 4, &n), 0u);
    EXPECT_EQ(n, 0u);
    (void)payload;
}

// ---------------------------------------------------------------------------
// Code 2: two frames, different compressed sizes (RFC 6716 §3.2.4)
// ---------------------------------------------------------------------------

TEST(OpusPacketiseCode2, TwoUnequalFramesSmallLength) {
    // Frame 0 size 5 (fits in 1-byte length ≤ 251), frame 1 size 20.
    std::uint8_t f0[5];
    std::uint8_t f1[20];
    fill_pattern(f0, sizeof(f0), 0xC0);
    fill_pattern(f1, sizeof(f1), 0xD0);

    CodecFrame frames[2] = {
        { f0, sizeof(f0), 20, 0 },
        { f1, sizeof(f1), 20, 0 },
    };

    std::uint8_t payload[256];
    std::size_t written = packetise(frames, 2, 48000, /*cbr=*/false,
                                    payload, sizeof(payload));
    ASSERT_EQ(written, 1 + 1 + sizeof(f0) + sizeof(f1));
    EXPECT_EQ(payload[0] & 0x03, 2);
    EXPECT_EQ(payload[1], sizeof(f0));   // 1-byte length = 5

    PacketView views[4];
    std::size_t n = 0;
    std::size_t got = depacketise(payload, written, views, 4, &n);
    EXPECT_EQ(got, 2u);
    EXPECT_EQ(views[0].size, sizeof(f0));
    EXPECT_EQ(views[1].size, sizeof(f1));
    EXPECT_EQ(views[0].code, 2);
    EXPECT_EQ(views[1].code, 2);
    EXPECT_TRUE(bytes_equal(views[0].data, f0, sizeof(f0)));
    EXPECT_TRUE(bytes_equal(views[1].data, f1, sizeof(f1)));
}

TEST(OpusPacketiseCode2, TwoUnequalFramesLargeLength) {
    // Frame 0 size 600 (>251, requires 2-byte length).
    std::uint8_t f0[600];
    std::uint8_t f1[40];
    fill_pattern(f0, sizeof(f0), 0x11);
    fill_pattern(f1, sizeof(f1), 0x22);

    CodecFrame frames[2] = {
        { f0, sizeof(f0), 20, 0 },
        { f1, sizeof(f1), 20, 0 },
    };

    std::uint8_t payload[1024];
    std::size_t written = packetise(frames, 2, 48000, false,
                                    payload, sizeof(payload));
    ASSERT_EQ(written, 1 + 2 + sizeof(f0) + sizeof(f1));
    EXPECT_EQ(payload[0] & 0x03, 2);
    // First byte in [252..255], second byte ∈ [0..255]; total = b0 + 4*b1 = 600.
    // offset = 600 - 252 = 348; b1 = 348 >> 2 = 87; b0 = 348 & 3 = 0.
    // first byte = 252 + 0 = 252. second byte = 87.
    EXPECT_EQ(payload[1], 252);
    EXPECT_EQ(payload[2], 87);

    PacketView views[4];
    std::size_t n = 0;
    std::size_t got = depacketise(payload, written, views, 4, &n);
    EXPECT_EQ(got, 2u);
    EXPECT_EQ(views[0].size, sizeof(f0));
    EXPECT_EQ(views[1].size, sizeof(f1));
    EXPECT_TRUE(bytes_equal(views[0].data, f0, sizeof(f0)));
    EXPECT_TRUE(bytes_equal(views[1].data, f1, sizeof(f1)));
}

TEST(OpusPacketiseCode2, MaxFrameLengthBoundary) {
    // Largest 2-byte length is 1275 bytes.
    std::uint8_t f0[1275];
    std::uint8_t f1[10];
    fill_pattern(f0, sizeof(f0), 0x33);
    fill_pattern(f1, sizeof(f1), 0x44);

    CodecFrame frames[2] = {
        { f0, sizeof(f0), 20, 0 },
        { f1, sizeof(f1), 20, 0 },
    };

    std::uint8_t payload[2048];
    std::size_t written = packetise(frames, 2, 48000, false,
                                    payload, sizeof(payload));
    EXPECT_EQ(written, 1 + 2 + sizeof(f0) + sizeof(f1));
    // 1275 - 252 = 1023. b1 = 1023 / 4 = 255. b0 = 1023 & 3 = 3.
    EXPECT_EQ(payload[1], 252 + 3);   // 255
    EXPECT_EQ(payload[2], 255);

    PacketView views[4];
    std::size_t n = 0;
    std::size_t got = depacketise(payload, written, views, 4, &n);
    EXPECT_EQ(got, 2u);
    EXPECT_EQ(views[0].size, sizeof(f0));
    EXPECT_EQ(views[1].size, sizeof(f1));
}

// ---------------------------------------------------------------------------
// Code 3: signalled M frames (RFC 6716 §3.2.5)
// ---------------------------------------------------------------------------

TEST(OpusPacketiseCode3, VbrThreeFramesRoundTrip) {
    std::uint8_t f0[10], f1[20], f2[30];
    fill_pattern(f0, sizeof(f0), 0x99);
    fill_pattern(f1, sizeof(f1), 0xAA);
    fill_pattern(f2, sizeof(f2), 0xBB);

    CodecFrame frames[3] = {
        { f0, sizeof(f0), 20, 0 },
        { f1, sizeof(f1), 20, 0 },
        { f2, sizeof(f2), 20, 0 },
    };

    std::uint8_t payload[256];
    std::size_t written = packetise(frames, 3, 48000, /*cbr=*/false,
                                    payload, sizeof(payload));
    // Code 3 VBR layout: TOC + fc byte + (M-1) length entries + sum(frame sizes)
    // = 1 + 1 + 2 + (10 + 20 + 30) = 64 bytes.
    ASSERT_EQ(written, 1u + 1u + 2u + sizeof(f0) + sizeof(f1) + sizeof(f2));
    EXPECT_EQ(payload[0] & 0x03, 3);
    // fc byte: v=1, p=0, M=3 → (1) | (0<<1) | (3<<2) = 1 | 12 = 0x0D
    EXPECT_EQ(payload[1], 0x0D);
    // Length table (VBR): len(f0), len(f1) — each ≤ 251 → 1-byte entries.
    EXPECT_EQ(payload[2], sizeof(f0));
    EXPECT_EQ(payload[3], sizeof(f1));

    PacketView views[8];
    std::size_t n = 0;
    std::size_t got = depacketise(payload, written, views, 8, &n);
    EXPECT_EQ(got, 3u);
    EXPECT_EQ(n, 3u);
    EXPECT_EQ(views[0].size, sizeof(f0));
    EXPECT_EQ(views[1].size, sizeof(f1));
    EXPECT_EQ(views[2].size, sizeof(f2));
    EXPECT_EQ(views[0].code, 3);
    EXPECT_TRUE(bytes_equal(views[0].data, f0, sizeof(f0)));
    EXPECT_TRUE(bytes_equal(views[1].data, f1, sizeof(f1)));
    EXPECT_TRUE(bytes_equal(views[2].data, f2, sizeof(f2)));
}

TEST(OpusPacketiseCode3, CbrFourFramesRoundTrip) {
    // 4 frames of 25 bytes each — equal-size CBR.
    std::uint8_t f[4][25];
    for (int i = 0; i < 4; ++i) fill_pattern(f[i], sizeof(f[i]),
                                             static_cast<std::uint8_t>(0x10 + i));

    CodecFrame frames[4];
    for (int i = 0; i < 4; ++i) {
        frames[i] = { f[i], sizeof(f[i]), 20, 0 };
    }

    std::uint8_t payload[256];
    std::size_t written = packetise(frames, 4, 48000, /*cbr=*/true,
                                    payload, sizeof(payload));
    ASSERT_EQ(written, 1 + 1 + 4 * sizeof(f[0]));
    EXPECT_EQ(payload[0] & 0x03, 3);
    // fc byte: v=0, p=0, M=4 → (0) | (0<<1) | (4<<2) = 16 = 0x10
    EXPECT_EQ(payload[1], 0x10);
    // 4 frames back-to-back, no length table.
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(bytes_equal(payload + 2 + i * sizeof(f[0]),
                                f[i], sizeof(f[i])));
    }

    PacketView views[8];
    std::size_t n = 0;
    std::size_t got = depacketise(payload, written, views, 8, &n);
    EXPECT_EQ(got, 4u);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(views[i].size, sizeof(f[i]));
        EXPECT_TRUE(bytes_equal(views[i].data, f[i], sizeof(f[i])));
    }
}

// ---------------------------------------------------------------------------
// Capacity / input validation
// ---------------------------------------------------------------------------

TEST(OpusPacketiseValidation, OutputTooSmallReturnsZero) {
    std::uint8_t frame[10];
    fill_pattern(frame, sizeof(frame), 0);

    CodecFrame in;
    in.data          = frame;
    in.size          = sizeof(frame);
    in.frame_size_ms = 20;
    in.is_stereo     = 0;

    std::uint8_t small[5];
    EXPECT_EQ(packetise(&in, 1, 48000, false, small, sizeof(small)), 0u);
}

TEST(OpusPacketiseValidation, MixedFrameSizeRejected) {
    // A 20 ms frame and a 10 ms frame in the same packet — must reject.
    std::uint8_t f0[5], f1[5];
    CodecFrame frames[2] = {
        { f0, sizeof(f0), 20, 0 },
        { f1, sizeof(f1), 10, 0 },
    };
    std::uint8_t payload[64];
    EXPECT_EQ(packetise(frames, 2, 48000, false, payload, sizeof(payload)), 0u);
}

TEST(OpusPacketiseValidation, MixedStereoRejected) {
    // Mono frame + stereo frame in the same packet — must reject because all
    // frames in a single Opus RTP packet share channel count (RFC 7587 §4.2).
    std::uint8_t f0[5], f1[5];
    CodecFrame frames[2] = {
        { f0, sizeof(f0), 20, 0 },  // mono
        { f1, sizeof(f1), 20, 1 },  // stereo
    };
    std::uint8_t payload[64];
    EXPECT_EQ(packetise(frames, 2, 48000, false, payload, sizeof(payload)), 0u);
}

TEST(OpusPacketiseValidation, Exceeds120msRejected) {
    // 40 ms × 4 frames = 160 ms — over the 120 ms cap.
    std::uint8_t f0[5], f1[5], f2[5], f3[5];
    CodecFrame frames[4] = {
        { f0, sizeof(f0), 40, 0 },
        { f1, sizeof(f1), 40, 0 },
        { f2, sizeof(f2), 40, 0 },
        { f3, sizeof(f3), 40, 0 },
    };
    std::uint8_t payload[64];
    EXPECT_EQ(packetise(frames, 4, 48000, false, payload, sizeof(payload)), 0u);
}

TEST(OpusPacketiseValidation, CbrMixedSizesRejected) {
    // CBR Code 3 requires all M frames to share the same compressed size.
    std::uint8_t f0[5], f1[10], f2[15];
    CodecFrame frames[3] = {
        { f0, sizeof(f0), 20, 0 },
        { f1, sizeof(f1), 20, 0 },
        { f2, sizeof(f2), 20, 0 },
    };
    std::uint8_t payload[64];
    EXPECT_EQ(packetise(frames, 3, 48000, /*cbr=*/true,
                        payload, sizeof(payload)), 0u);
}

TEST(OpusPacketiseValidation, ZeroFramesRejected) {
    std::uint8_t payload[16];
    EXPECT_EQ(packetise(nullptr, 0, 48000, false, payload, sizeof(payload)), 0u);
}

TEST(OpusPacketiseValidation, TooManyFramesRejected) {
    // 49 frames of 2.5 ms each would be 122.5 ms — over 48-frame cap.
    std::uint8_t f0[5];
    CodecFrame frames[49];
    for (int i = 0; i < 49; ++i) {
        frames[i] = { f0, sizeof(f0), 3, 0 };
    }
    std::uint8_t payload[256];
    EXPECT_EQ(packetise(frames, 49, 48000, false, payload, sizeof(payload)), 0u);
}

// ---------------------------------------------------------------------------
// depacketise() edge cases
// ---------------------------------------------------------------------------

TEST(OpusPacketiseDepacketise, EmptyPayloadRejected) {
    PacketView views[4];
    std::size_t n = 0;
    EXPECT_EQ(depacketise(nullptr, 0, views, 4, &n), 0u);
}

TEST(OpusPacketiseDepacketise, TruncatedCode2Rejected) {
    // Code 2 with only 1 byte after TOC: length read fails.
    std::uint8_t payload[2] = { 0xFA, 0xFF };  // code 2, b0=0xFF → needs second byte
    PacketView views[4];
    std::size_t n = 0;
    EXPECT_EQ(depacketise(payload, sizeof(payload), views, 4, &n), 0u);
}

TEST(OpusPacketiseDepacketise, CbrNonDivisibleRejected) {
    // Code 3 CBR with M=3 and R=8 (not divisible by 3).
    // TOC: config=31, s=0, code=3 = 0xFB
    // fc byte: v=0, p=0, M=3 = (3<<2) = 0x0C
    // frame data: 8 bytes
    std::uint8_t payload[10] = {
        0xFB, 0x0C,
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22,
    };
    PacketView views[4];
    std::size_t n = 0;
    EXPECT_EQ(depacketise(payload, sizeof(payload), views, 4, &n), 0u);
}

TEST(OpusPacketiseDepacketise, Code3ZeroMRejected) {
    // TOC code=3, fc byte = v=0,p=0,M=0 → RFC 6716 [R5]: M != 0
    std::uint8_t payload[4] = { 0xFB, 0x00, 0xAA, 0xBB };
    PacketView views[4];
    std::size_t n = 0;
    EXPECT_EQ(depacketise(payload, sizeof(payload), views, 4, &n), 0u);
}

// ---------------------------------------------------------------------------
// Multi-frame round-trip at 20 ms (60 ms ptime = 3 × 20 ms)
// ---------------------------------------------------------------------------

TEST(OpusPacketiseRoundtrip, ThreeFramesAt60msPtime) {
    // Realistic use case: 3 × 20 ms frames of equal compressed size in one
    // RTP packet (CBR Code 3 framing).
    constexpr std::size_t kPerFrame = 15;
    std::uint8_t f0[kPerFrame], f1[kPerFrame], f2[kPerFrame];
    fill_pattern(f0, sizeof(f0), 0xE0);
    fill_pattern(f1, sizeof(f1), 0xE1);
    fill_pattern(f2, sizeof(f2), 0xE2);

    CodecFrame frames[3] = {
        { f0, sizeof(f0), 20, 0 },
        { f1, sizeof(f1), 20, 0 },
        { f2, sizeof(f2), 20, 0 },
    };

    std::uint8_t payload[256];
    std::size_t written = packetise(frames, 3, 48000, /*cbr=*/true,
                                    payload, sizeof(payload));
    ASSERT_GT(written, 0u);

    PacketView views[8];
    std::size_t n = 0;
    EXPECT_EQ(depacketise(payload, written, views, 8, &n), 3u);
    EXPECT_EQ(n, 3u);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(views[i].size, frames[i].size);
        EXPECT_EQ(views[i].frame_size_ms, 20);
        EXPECT_TRUE(bytes_equal(views[i].data, frames[i].data, frames[i].size));
    }
}

} // namespace nimrtc::opus::test
