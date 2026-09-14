/**
 * @file src/modules/video_payload/tests/test_vp9.cpp
 * @brief VP9 RTP payload unit tests (RFC 9559).
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <nimrtc/video_payload/vp9.hpp>

namespace vp9 = nimrtc::video_payload::vp9;

using vp9::Descriptor;
using vp9::parse;
using vp9::build;
using vp9::descriptor_size;

// -----------------------------------------------------------------------------
// Descriptor size
// -----------------------------------------------------------------------------

TEST(VP9Descriptor, SizeBase) {
    Descriptor d;
    EXPECT_EQ(descriptor_size(d), 1u);
}

TEST(VP9Descriptor, SizeWith7BitPicId) {
    Descriptor d;
    d.has_picture_id   = true;
    d.picture_id_16bit = false;
    EXPECT_EQ(descriptor_size(d), 2u);
}

TEST(VP9Descriptor, SizeWith16BitPicId) {
    Descriptor d;
    d.has_picture_id   = true;
    d.picture_id_16bit = true;
    EXPECT_EQ(descriptor_size(d), 3u);
}

TEST(VP9Descriptor, SizeWithPicIdAndLayerInfo) {
    Descriptor d;
    d.has_picture_id = true;
    d.has_layer_info = true;
    EXPECT_EQ(descriptor_size(d), 3u);
}

// -----------------------------------------------------------------------------
// Parse
// -----------------------------------------------------------------------------

TEST(VP9Parse, BaseDescriptorOnly) {
    const std::uint8_t raw[] = {0x81, 0xAA};   // Z=1? No — 0x81 = 1000_0001 = Z=1 actually
    // Actually 0x81 = 1000_0001: Z=1 Y=0 F=0 ID=0 N=0 D=1 → has picture id + inter
    auto r = parse({raw, sizeof(raw)});
    // The byte is too short for a picture id, so it should fail.
    EXPECT_FALSE(r.parsed_ok);
}

TEST(VP9Parse, BaseDescriptorWithKeyframeBit) {
    // 0x88 = 1000_1000 → Z=1 Y=0 F=0 N=0 D=0 (intra / key)
    const std::uint8_t raw[] = {
        0x88,            // Z=1
        0x2A,            // 7-bit picture ID = 42
        0xAA, 0xBB,      // bitstream
    };
    auto r = parse({raw, sizeof(raw)});
    ASSERT_TRUE(r.parsed_ok);
    EXPECT_TRUE(r.desc.has_picture_id);
    EXPECT_FALSE(r.desc.picture_id_16bit);
    EXPECT_EQ(r.desc.picture_id, 42);
    EXPECT_FALSE(r.desc.inter_predicted);   // D=0 → intra
    EXPECT_EQ(r.bitstream.size(), 2u);
}

TEST(VP9Parse, LayerInfoPresent) {
    // Z=1 Y=1 F=0 N=0 D=0
    const std::uint8_t raw[] = {
        0xC8,                          // 1100_1000 = Z|Y
        0x10,                          // 7-bit pic id = 16
        (3 << 5) | (2 << 3),           // TID=3 SID=2
        0xAA, 0xBB,
    };
    auto r = parse({raw, sizeof(raw)});
    ASSERT_TRUE(r.parsed_ok);
    EXPECT_TRUE(r.desc.has_layer_info);
    EXPECT_EQ(r.desc.temporal_id, 3u);
    EXPECT_EQ(r.desc.spatial_id,  2u);
    EXPECT_EQ(r.bitstream.size(), 2u);
}

TEST(VP9Parse, RejectsEmpty) {
    auto r = parse({});
    EXPECT_FALSE(r.parsed_ok);
}

// -----------------------------------------------------------------------------
// Build roundtrip
// -----------------------------------------------------------------------------

TEST(VP9Build, RoundtripBaseDescriptor) {
    Descriptor d;
    d.inter_predicted = true;       // D=1

    const std::uint8_t bs[] = {0xAA, 0xBB, 0xCC};
    std::uint8_t out[16];
    std::size_t n = build(d, {bs, sizeof(bs)}, {out, sizeof(out)});
    EXPECT_EQ(n, 1 + sizeof(bs));
    EXPECT_EQ(out[0] & 0x01, 0x01);   // D bit set

    auto r = parse({out, n});
    ASSERT_TRUE(r.parsed_ok);
    EXPECT_TRUE(r.desc.inter_predicted);
    EXPECT_EQ(r.bitstream.size(), 3u);
}

TEST(VP9Build, RoundtripWithPicIdAndLayerInfo) {
    Descriptor d;
    d.has_picture_id = true;
    d.picture_id     = 100;
    d.has_layer_info = true;
    d.temporal_id    = 1;
    d.spatial_id     = 0;

    const std::uint8_t bs[] = {0xAA};
    std::uint8_t out[16];
    std::size_t n = build(d, {bs, sizeof(bs)}, {out, sizeof(out)});
    EXPECT_EQ(n, 3 + 1);   // 1 base + 1 picid + 1 layer + 1 bitstream

    auto r = parse({out, n});
    ASSERT_TRUE(r.parsed_ok);
    EXPECT_EQ(r.desc.picture_id, 100);
    EXPECT_TRUE(r.desc.has_layer_info);
    EXPECT_EQ(r.desc.temporal_id, 1u);
    EXPECT_EQ(r.desc.spatial_id,  0u);
}

TEST(VP9Build, BufferTooSmallReturnsZero) {
    Descriptor d;
    d.has_picture_id   = true;
    d.picture_id_16bit = true;
    std::uint8_t tiny[1] = {0};
    EXPECT_EQ(build(d, {}, {tiny, sizeof(tiny)}), 0u);
}
