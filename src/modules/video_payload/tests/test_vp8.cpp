/**
 * @file src/modules/video_payload/tests/test_vp8.cpp
 * @brief VP8 RTP payload unit tests (RFC 7741).
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <nimrtc/video_payload/vp8.hpp>

namespace vp8 = nimrtc::video_payload::vp8;

using vp8::Descriptor;
using vp8::parse;
using vp8::build_descriptor;
using vp8::build;
using vp8::bitstream_is_keyframe;
using vp8::descriptor_size;

// -----------------------------------------------------------------------------
// Descriptor size
// -----------------------------------------------------------------------------

TEST(VP8Descriptor, SizeBase) {
    Descriptor d;
    EXPECT_EQ(descriptor_size(d), 1u);
}

TEST(VP8Descriptor, SizeWith7BitPicId) {
    Descriptor d;
    d.has_picture_id   = true;
    d.picture_id_16bit = false;
    EXPECT_EQ(descriptor_size(d), 3u);
}

TEST(VP8Descriptor, SizeWith16BitPicId) {
    Descriptor d;
    d.has_picture_id   = true;
    d.picture_id_16bit = true;
    EXPECT_EQ(descriptor_size(d), 4u);
}

// -----------------------------------------------------------------------------
// Parse
// -----------------------------------------------------------------------------

TEST(VP8Parse, BaseDescriptorOnly) {
    const std::uint8_t raw[] = {
        0x10,         // S=1, PID=0
        0xDE, 0xAD,   // bitstream
    };
    auto r = parse({raw, sizeof(raw)});
    ASSERT_TRUE(r.parsed_ok);
    EXPECT_EQ(r.desc.partition_index, 0u);
    EXPECT_TRUE(r.desc.start_of_partition);
    EXPECT_FALSE(r.desc.non_reference);
    EXPECT_FALSE(r.desc.has_picture_id);
    EXPECT_EQ(r.bitstream.size(), 2u);
    EXPECT_EQ(r.bitstream[0], 0xDE);
}

TEST(VP8Parse, With7BitPictureId) {
    const std::uint8_t raw[] = {
        0x90,                       // X=1, S=1, PID=0
        0x80,                       // I=1
        0x2A,                       // M=0 → 7-bit picture ID = 0x2A (42)
        0xDE, 0xAD,
    };
    auto r = parse({raw, sizeof(raw)});
    ASSERT_TRUE(r.parsed_ok);
    EXPECT_TRUE(r.desc.has_picture_id);
    EXPECT_FALSE(r.desc.picture_id_16bit);
    EXPECT_EQ(r.desc.picture_id, 42);
    EXPECT_EQ(r.bitstream.size(), 2u);
}

TEST(VP8Parse, With16BitPictureId) {
    const std::uint8_t raw[] = {
        0x90,                       // X=1, S=1, PID=0
        0x80,                       // I=1
        0x80 | 0x01,                // M=1 → 16-bit picture ID follows
        0x23,                       // low byte
        0xDE, 0xAD,
    };
    auto r = parse({raw, sizeof(raw)});
    ASSERT_TRUE(r.parsed_ok);
    EXPECT_TRUE(r.desc.picture_id_16bit);
    EXPECT_EQ(r.desc.picture_id, 0x0123);
}

TEST(VP8Parse, NonReferenceFlag) {
    const std::uint8_t raw[] = {0x20, 0xFF};   // N=1
    auto r = parse({raw, sizeof(raw)});
    ASSERT_TRUE(r.parsed_ok);
    EXPECT_TRUE(r.desc.non_reference);
}

TEST(VP8Parse, RejectsEmpty) {
    auto r = parse({});
    EXPECT_FALSE(r.parsed_ok);
}

TEST(VP8Parse, RejectsTruncatedExt) {
    const std::uint8_t raw[] = {0x90};        // X=1 but no ext byte
    auto r = parse({raw, sizeof(raw)});
    EXPECT_FALSE(r.parsed_ok);
}

// -----------------------------------------------------------------------------
// Build roundtrip
// -----------------------------------------------------------------------------

TEST(VP8Build, RoundtripBaseDescriptor) {
    Descriptor d;
    d.partition_index    = 0;
    d.start_of_partition = true;
    d.non_reference      = false;
    // Bitstream: pretend VP8 key frame (first byte LSB=1).
    const std::uint8_t bs[] = {0x9D, 0x01, 0x2A};

    std::uint8_t out[16];
    std::size_t n = build(d, {bs, sizeof(bs)}, {out, sizeof(out)});
    EXPECT_EQ(n, 1 + sizeof(bs));

    auto r = parse({out, n});
    ASSERT_TRUE(r.parsed_ok);
    EXPECT_TRUE(r.desc.start_of_partition);
    EXPECT_EQ(r.bitstream.size(), sizeof(bs));
    EXPECT_EQ(r.bitstream[0], 0x9D);
    EXPECT_TRUE(bitstream_is_keyframe(r.bitstream));
}

TEST(VP8Build, RoundtripWithPicId) {
    Descriptor d;
    d.has_picture_id = true;
    d.picture_id     = 100;
    const std::uint8_t bs[] = {0xAA, 0xBB};

    std::uint8_t out[16];
    std::size_t n = build(d, {bs, sizeof(bs)}, {out, sizeof(out)});
    auto r = parse({out, n});
    ASSERT_TRUE(r.parsed_ok);
    EXPECT_TRUE(r.desc.has_picture_id);
    EXPECT_EQ(r.desc.picture_id, 100);
    EXPECT_EQ(r.bitstream.size(), 2u);
}

TEST(VP8Build, BufferTooSmallReturnsZero) {
    Descriptor d;
    d.has_picture_id   = true;
    d.picture_id_16bit = true;
    std::uint8_t tiny[2] = {0};
    EXPECT_EQ(build(d, {}, {tiny, sizeof(tiny)}), 0u);
}

// -----------------------------------------------------------------------------
// Keyframe detection
// -----------------------------------------------------------------------------

TEST(VP8Bitstream, IsKeyframeDetected) {
    // VP8 frame tag byte 0: bit 0 = key-frame flag (1 = keyframe).
    const std::uint8_t keyframe[]  = {0x9D, 0x01, 0x2A};   // bit 0 = 1 → keyframe
    EXPECT_TRUE(bitstream_is_keyframe({keyframe, sizeof(keyframe)}));

    // P-frame (interframe): bit 0 must be 0.
    const std::uint8_t p_frame[] = {0x30, 0x00, 0x00};    // bit 0 = 0 → interframe
    EXPECT_FALSE(bitstream_is_keyframe({p_frame, sizeof(p_frame)}));
}
