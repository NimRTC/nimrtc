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
 *   7. Stereo TOC byte (s field) boundaries: 0 (mono), 1 (LR).
 *   8. Capacity-too-small returns 0 and writes nothing.
 *   9. Mismatched TOC config / stereo bit across frames is rejected.
 *  10. Frames exceeding 120 ms total duration are rejected (RFC 6716 [R5]).
 *  11. depacketise() rejects malformed payloads (odd-length Code 1,
 *      truncated Code 2 length, Code 3 R = M * per divisibility, etc.).
 *  12. The packet's TOC comes from the frame's own TOC — the packetiser
 *      never invents a config field (defect F-14).
 *
 * The input frames are synthetic but *complete* Opus frames: a TOC byte
 * followed by a body, exactly the shape libopus produces.  The packetiser
 * is responsible for the framing, not for the codec bit-stream.
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

/** A complete Opus frame: the TOC byte libopus wrote + the frame's body. */
std::vector<std::uint8_t> make_frame(std::uint8_t config,
                                      std::uint8_t s,
                                      std::size_t body_size,
                                      std::uint8_t seed) {
    std::vector<std::uint8_t> frame(1 + body_size);
    frame[0] = make_toc_byte(config, s, /*code=*/0);
    if (body_size > 0) fill_pattern(frame.data() + 1, body_size, seed);
    return frame;
}

/** The body of a frame — what the RTP payload carries. */
std::size_t body_size(const std::vector<std::uint8_t>& frame) {
    return frame.size() - 1;
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
    // A 20 ms CELT FB frame (config 31), mono.
    const auto frame = make_frame(31, 0, 40, /*seed=*/0xA0);

    CodecFrame in;
    in.data = frame.data();
    in.size = frame.size();

    std::uint8_t payload[256];
    std::size_t written = packetise(&in, 1, 48000, /*cbr=*/false,
                                    payload, sizeof(payload));
    // The packet's TOC replaces the frame's TOC — same size.
    ASSERT_EQ(written, frame.size());
    // TOC = (31 << 3) | (0 << 2) | 0 = 0xF8
    EXPECT_EQ(payload[0], 0xF8);
    EXPECT_TRUE(bytes_equal(payload + 1, frame.data() + 1, body_size(frame)));

    // Round-trip through depacketise.
    PacketView views[4];
    std::size_t n = 0;
    std::size_t got = depacketise(payload, written, views, 4, &n);
    EXPECT_EQ(got, 1u);
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(views[0].size, body_size(frame));
    EXPECT_EQ(views[0].config, 31);
    EXPECT_EQ(views[0].s, 0);
    EXPECT_EQ(views[0].code, 0);
    EXPECT_EQ(views[0].frame_size_ms, 20);
    EXPECT_TRUE(bytes_equal(views[0].data, frame.data() + 1, body_size(frame)));
}

TEST(OpusPacketiseCode0, SingleFrameStereoRoundTrip) {
    // A 10 ms CELT FB frame (config 30), stereo.
    const auto frame = make_frame(30, 1, 60, /*seed=*/0x55);

    CodecFrame in;
    in.data = frame.data();
    in.size = frame.size();

    std::uint8_t payload[256];
    std::size_t written = packetise(&in, 1, 48000, false,
                                    payload, sizeof(payload));
    ASSERT_EQ(written, frame.size());
    // TOC = (30 << 3) | (1 << 2) | 0 = 0xF0 | 0x04 = 0xF4
    EXPECT_EQ(payload[0], 0xF4);
    EXPECT_TRUE(bytes_equal(payload + 1, frame.data() + 1, body_size(frame)));

    PacketView views[4];
    std::size_t n = 0;
    std::size_t got = depacketise(payload, written, views, 4, &n);
    EXPECT_EQ(got, 1u);
    EXPECT_EQ(views[0].s, 1);
    EXPECT_EQ(views[0].frame_size_ms, 10);
    EXPECT_EQ(views[0].size, body_size(frame));
}

TEST(OpusPacketiseCode0, ThePacketTocComesFromTheFrame) {
    // The packetiser must not invent a config field: the TOC on the wire
    // has to be the one the frame was coded with (defect F-14).  A hybrid
    // 20 ms frame is config 15 — a config the "canonical" mapping for
    // 20 ms (config 31) would contradict.
    const auto frame = make_frame(15, 0, 32, /*seed=*/0x77);

    CodecFrame in;
    in.data = frame.data();
    in.size = frame.size();

    std::uint8_t payload[256];
    const std::size_t written = packetise(&in, 1, 48000, false,
                                          payload, sizeof(payload));
    ASSERT_EQ(written, frame.size());
    EXPECT_EQ(payload[0], make_toc_byte(15, 0, 0));
    EXPECT_EQ(payload[0], 0x78);

    PacketView views[4];
    std::size_t n = 0;
    EXPECT_EQ(depacketise(payload, written, views, 4, &n), 1u);
    EXPECT_EQ(views[0].config, 15);
    EXPECT_EQ(views[0].frame_size_ms, 20);
}

TEST(OpusPacketiseCode0, DtxFrameKeepsItsSingleByte) {
    // libopus returns a one-byte packet for a silent (DTX) frame: the TOC
    // and nothing else.  The packetiser must pass it through.
    const auto frame = make_frame(31, 0, 0, /*seed=*/0);

    CodecFrame in;
    in.data = frame.data();
    in.size = frame.size();

    std::uint8_t payload[16];
    const std::size_t written = packetise(&in, 1, 48000, false,
                                          payload, sizeof(payload));
    ASSERT_EQ(written, 1u);
    EXPECT_EQ(payload[0], make_toc_byte(31, 0, 0));
}

TEST(OpusPacketiseCode0, MultiFrameTocRejected) {
    // A frame whose TOC carries a multi-frame code describes a *packet*,
    // not a frame — the packetiser refuses it.
    std::uint8_t frame[8] = { make_toc_byte(31, 0, /*code=*/1), 1, 2, 3, 4, 5, 6, 7 };
    CodecFrame in;
    in.data = frame;
    in.size = sizeof(frame);

    std::uint8_t payload[64];
    EXPECT_EQ(packetise(&in, 1, 48000, false, payload, sizeof(payload)), 0u);
}

// ---------------------------------------------------------------------------
// Code 1: two frames, equal compressed size (RFC 6716 §3.2.3)
// ---------------------------------------------------------------------------

TEST(OpusPacketiseCode1, TwoEqualFramesRoundTrip) {
    const auto f0 = make_frame(31, 0, 30, /*seed=*/0x10);
    const auto f1 = make_frame(31, 0, 30, /*seed=*/0x80);

    CodecFrame frames[2] = {
        { f0.data(), f0.size() },
        { f1.data(), f1.size() },
    };

    std::uint8_t payload[256];
    std::size_t written = packetise(frames, 2, 48000, /*cbr=*/true,
                                    payload, sizeof(payload));
    ASSERT_EQ(written, 1 + 2 * body_size(f0));
    // TOC code 1 → bits 1:0 = 0b01
    EXPECT_EQ(payload[0] & 0x03, 1);
    EXPECT_TRUE(bytes_equal(payload + 1,         f0.data() + 1, body_size(f0)));
    EXPECT_TRUE(bytes_equal(payload + 1 + body_size(f0), f1.data() + 1, body_size(f1)));

    PacketView views[4];
    std::size_t n = 0;
    std::size_t got = depacketise(payload, written, views, 4, &n);
    EXPECT_EQ(got, 2u);
    EXPECT_EQ(views[0].size, body_size(f0));
    EXPECT_EQ(views[1].size, body_size(f1));
    EXPECT_EQ(views[0].code, 1);
    EXPECT_EQ(views[1].code, 1);
    EXPECT_TRUE(bytes_equal(views[0].data, f0.data() + 1, body_size(f0)));
    EXPECT_TRUE(bytes_equal(views[1].data, f1.data() + 1, body_size(f1)));
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
    // Body 4 bytes (fits in a 1-byte length ≤ 251), body 19 bytes.
    const auto f0 = make_frame(31, 0, 4, /*seed=*/0xC0);
    const auto f1 = make_frame(31, 0, 19, /*seed=*/0xD0);

    CodecFrame frames[2] = {
        { f0.data(), f0.size() },
        { f1.data(), f1.size() },
    };

    std::uint8_t payload[256];
    std::size_t written = packetise(frames, 2, 48000, /*cbr=*/false,
                                    payload, sizeof(payload));
    ASSERT_EQ(written, 1 + 1 + body_size(f0) + body_size(f1));
    EXPECT_EQ(payload[0] & 0x03, 2);
    EXPECT_EQ(payload[1], body_size(f0));   // 1-byte length = 4

    PacketView views[4];
    std::size_t n = 0;
    std::size_t got = depacketise(payload, written, views, 4, &n);
    EXPECT_EQ(got, 2u);
    EXPECT_EQ(views[0].size, body_size(f0));
    EXPECT_EQ(views[1].size, body_size(f1));
    EXPECT_EQ(views[0].code, 2);
    EXPECT_EQ(views[1].code, 2);
    EXPECT_TRUE(bytes_equal(views[0].data, f0.data() + 1, body_size(f0)));
    EXPECT_TRUE(bytes_equal(views[1].data, f1.data() + 1, body_size(f1)));
}

TEST(OpusPacketiseCode2, TwoUnequalFramesLargeLength) {
    // Body 599 bytes (>251, requires a 2-byte length).
    const auto f0 = make_frame(31, 0, 599, /*seed=*/0x11);
    const auto f1 = make_frame(31, 0, 39, /*seed=*/0x22);

    CodecFrame frames[2] = {
        { f0.data(), f0.size() },
        { f1.data(), f1.size() },
    };

    std::uint8_t payload[1024];
    std::size_t written = packetise(frames, 2, 48000, false,
                                    payload, sizeof(payload));
    ASSERT_EQ(written, 1 + 2 + body_size(f0) + body_size(f1));
    EXPECT_EQ(payload[0] & 0x03, 2);
    // First byte in [252..255], second byte ∈ [0..255]; total = b0 + 4*b1 = 599.
    // offset = 599 - 252 = 347; b1 = 347 >> 2 = 86; b0 = 347 & 3 = 3.
    // first byte = 252 + 3 = 255. second byte = 86.
    EXPECT_EQ(payload[1], 255);
    EXPECT_EQ(payload[2], 86);

    PacketView views[4];
    std::size_t n = 0;
    std::size_t got = depacketise(payload, written, views, 4, &n);
    EXPECT_EQ(got, 2u);
    EXPECT_EQ(views[0].size, body_size(f0));
    EXPECT_EQ(views[1].size, body_size(f1));
    EXPECT_TRUE(bytes_equal(views[0].data, f0.data() + 1, body_size(f0)));
    EXPECT_TRUE(bytes_equal(views[1].data, f1.data() + 1, body_size(f1)));
}

TEST(OpusPacketiseCode2, MaxFrameLengthBoundary) {
    // The largest 2-byte length is 1275 bytes.
    const auto f0 = make_frame(31, 0, 1274, /*seed=*/0x33);
    const auto f1 = make_frame(31, 0, 9, /*seed=*/0x44);

    CodecFrame frames[2] = {
        { f0.data(), f0.size() },
        { f1.data(), f1.size() },
    };

    std::uint8_t payload[2048];
    std::size_t written = packetise(frames, 2, 48000, false,
                                    payload, sizeof(payload));
    EXPECT_EQ(written, 1 + 2 + body_size(f0) + body_size(f1));
    // 1274 - 252 = 1022. b1 = 1022 / 4 = 255. b0 = 1022 & 3 = 2.
    EXPECT_EQ(payload[1], 252 + 2);   // 254
    EXPECT_EQ(payload[2], 255);

    PacketView views[4];
    std::size_t n = 0;
    std::size_t got = depacketise(payload, written, views, 4, &n);
    EXPECT_EQ(got, 2u);
    EXPECT_EQ(views[0].size, body_size(f0));
    EXPECT_EQ(views[1].size, body_size(f1));
}

// ---------------------------------------------------------------------------
// Code 3: signalled M frames (RFC 6716 §3.2.5)
// ---------------------------------------------------------------------------

TEST(OpusPacketiseCode3, VbrThreeFramesRoundTrip) {
    const auto f0 = make_frame(31, 0, 9, /*seed=*/0x99);
    const auto f1 = make_frame(31, 0, 19, /*seed=*/0xAA);
    const auto f2 = make_frame(31, 0, 29, /*seed=*/0xBB);

    CodecFrame frames[3] = {
        { f0.data(), f0.size() },
        { f1.data(), f1.size() },
        { f2.data(), f2.size() },
    };

    std::uint8_t payload[256];
    std::size_t written = packetise(frames, 3, 48000, /*cbr=*/false,
                                    payload, sizeof(payload));
    // Code 3 VBR layout: TOC + fc byte + (M-1) length entries + sum(bodies)
    // = 1 + 1 + 2 + (9 + 19 + 29) = 61 bytes.
    ASSERT_EQ(written, 1u + 1u + 2u + body_size(f0) + body_size(f1) + body_size(f2));
    EXPECT_EQ(payload[0] & 0x03, 3);
    // fc byte: v=1, p=0, M=3 → (1) | (0<<1) | (3<<2) = 1 | 12 = 0x0D
    EXPECT_EQ(payload[1], 0x0D);
    // Length table (VBR): len(f0), len(f1) — each ≤ 251 → 1-byte entries.
    EXPECT_EQ(payload[2], body_size(f0));
    EXPECT_EQ(payload[3], body_size(f1));

    PacketView views[8];
    std::size_t n = 0;
    std::size_t got = depacketise(payload, written, views, 8, &n);
    EXPECT_EQ(got, 3u);
    EXPECT_EQ(n, 3u);
    EXPECT_EQ(views[0].size, body_size(f0));
    EXPECT_EQ(views[1].size, body_size(f1));
    EXPECT_EQ(views[2].size, body_size(f2));
    EXPECT_EQ(views[0].code, 3);
    EXPECT_TRUE(bytes_equal(views[0].data, f0.data() + 1, body_size(f0)));
    EXPECT_TRUE(bytes_equal(views[1].data, f1.data() + 1, body_size(f1)));
    EXPECT_TRUE(bytes_equal(views[2].data, f2.data() + 1, body_size(f2)));
}

TEST(OpusPacketiseCode3, CbrFourFramesRoundTrip) {
    // 4 frames with equal bodies — equal-size CBR.
    std::vector<std::vector<std::uint8_t>> f;
    for (int i = 0; i < 4; ++i) {
        f.push_back(make_frame(31, 0, 24,
                               static_cast<std::uint8_t>(0x10 + i)));
    }

    CodecFrame frames[4];
    for (int i = 0; i < 4; ++i) {
        frames[i] = { f[static_cast<std::size_t>(i)].data(),
                      f[static_cast<std::size_t>(i)].size() };
    }

    std::uint8_t payload[256];
    std::size_t written = packetise(frames, 4, 48000, /*cbr=*/true,
                                    payload, sizeof(payload));
    ASSERT_EQ(written, 1 + 1 + 4 * body_size(f[0]));
    EXPECT_EQ(payload[0] & 0x03, 3);
    // fc byte: v=0, p=0, M=4 → (0) | (0<<1) | (4<<2) = 16 = 0x10
    EXPECT_EQ(payload[1], 0x10);
    // 4 frames back-to-back, no length table.
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(bytes_equal(payload + 2 + static_cast<std::size_t>(i) * body_size(f[0]),
                                f[static_cast<std::size_t>(i)].data() + 1,
                                body_size(f[0])));
    }

    PacketView views[8];
    std::size_t n = 0;
    std::size_t got = depacketise(payload, written, views, 8, &n);
    EXPECT_EQ(got, 4u);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(views[i].size, body_size(f[static_cast<std::size_t>(i)]));
        EXPECT_TRUE(bytes_equal(views[i].data,
                                f[static_cast<std::size_t>(i)].data() + 1,
                                body_size(f[static_cast<std::size_t>(i)])));
    }
}

// ---------------------------------------------------------------------------
// Capacity / input validation
// ---------------------------------------------------------------------------

TEST(OpusPacketiseValidation, OutputTooSmallReturnsZero) {
    const auto frame = make_frame(31, 0, 9, /*seed=*/0);

    CodecFrame in;
    in.data = frame.data();
    in.size = frame.size();

    std::uint8_t small[5];
    EXPECT_EQ(packetise(&in, 1, 48000, false, small, sizeof(small)), 0u);
}

TEST(OpusPacketiseValidation, MixedFrameSizeRejected) {
    // A 20 ms frame (config 31) and a 10 ms frame (config 30) in the same
    // packet — the packet has one TOC, so the frames must agree.
    const auto f0 = make_frame(31, 0, 4, /*seed=*/0);
    const auto f1 = make_frame(30, 0, 4, /*seed=*/0);
    CodecFrame frames[2] = {
        { f0.data(), f0.size() },
        { f1.data(), f1.size() },
    };
    std::uint8_t payload[64];
    EXPECT_EQ(packetise(frames, 2, 48000, false, payload, sizeof(payload)), 0u);
}

TEST(OpusPacketiseValidation, MixedStereoRejected) {
    // Mono frame + stereo frame in the same packet — must reject because all
    // frames in a single Opus RTP packet share channel count (RFC 7587 §4.2).
    const auto f0 = make_frame(31, 0, 4, /*seed=*/0);
    const auto f1 = make_frame(31, 1, 4, /*seed=*/0);
    CodecFrame frames[2] = {
        { f0.data(), f0.size() },
        { f1.data(), f1.size() },
    };
    std::uint8_t payload[64];
    EXPECT_EQ(packetise(frames, 2, 48000, false, payload, sizeof(payload)), 0u);
}

TEST(OpusPacketiseValidation, Exceeds120msRejected) {
    // 40 ms × 4 frames = 160 ms — over the 120 ms cap (config 10 = SILK WB).
    const auto f0 = make_frame(10, 0, 4, /*seed=*/0);
    CodecFrame frames[4];
    for (int i = 0; i < 4; ++i) {
        frames[i] = { f0.data(), f0.size() };
    }
    std::uint8_t payload[64];
    EXPECT_EQ(packetise(frames, 4, 48000, false, payload, sizeof(payload)), 0u);
}

TEST(OpusPacketiseValidation, CbrMixedSizesRejected) {
    // CBR Code 3 requires all M frames to share the same compressed size.
    const auto f0 = make_frame(31, 0, 4, /*seed=*/0);
    const auto f1 = make_frame(31, 0, 9, /*seed=*/0);
    const auto f2 = make_frame(31, 0, 14, /*seed=*/0);
    CodecFrame frames[3] = {
        { f0.data(), f0.size() },
        { f1.data(), f1.size() },
        { f2.data(), f2.size() },
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
    // 49 frames — over the 48-frame cap.
    const auto f0 = make_frame(28, 0, 4, /*seed=*/0);
    CodecFrame frames[49];
    for (int i = 0; i < 49; ++i) {
        frames[i] = { f0.data(), f0.size() };
    }
    std::uint8_t payload[256];
    EXPECT_EQ(packetise(frames, 49, 48000, false, payload, sizeof(payload)), 0u);
}

} // namespace nimrtc::opus::test
