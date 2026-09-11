/**
 * @file src/modules/video_payload/tests/test_h264.cpp
 * @brief H.264 RTP payload unit tests (RFC 6184).
 *
 * Covers:
 *   1. Single NAL unit parse / build roundtrip
 *   2. STAP-A parse (multiple NAL units) and build
 *   3. FU-A parse (start/middle/end) and build
 *   4. FU-A reassembly across fragments (sanity)
 *   5. Top-level dispatcher correctly classifies payloads
 *   6. Keyframe / parameter-set type helpers
 *   7. Empty / malformed inputs rejected
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <numeric>
#include <vector>

#include <nimrtc/video_payload/h264.hpp>

namespace h264 = nimrtc::video_payload::h264;
namespace core = nimrtc::core;

using h264::NaluType;
using h264::parse;
using h264::parse_single;
using h264::parse_stap_a;
using h264::parse_fu_a_header;
using h264::build_single;
using h264::build_stap_a;
using h264::build_fu_a;
using h264::peek_nalu_type;
using h264::is_keyframe_nalu;
using h264::is_parameter_set_nalu;

// -----------------------------------------------------------------------------
// Type classification helpers
// -----------------------------------------------------------------------------

TEST(H264NaluType, IsKeyframe) {
    EXPECT_TRUE (is_keyframe_nalu(NaluType::kSliceIDR));
    EXPECT_FALSE(is_keyframe_nalu(NaluType::kSliceNonIDR));
    EXPECT_FALSE(is_keyframe_nalu(NaluType::kSPS));
}

TEST(H264NaluType, IsParameterSet) {
    EXPECT_TRUE (is_parameter_set_nalu(NaluType::kSPS));
    EXPECT_TRUE (is_parameter_set_nalu(NaluType::kPPS));
    EXPECT_TRUE (is_parameter_set_nalu(NaluType::kSPSExt));
    EXPECT_FALSE(is_parameter_set_nalu(NaluType::kSliceIDR));
}

// -----------------------------------------------------------------------------
// Peek
// -----------------------------------------------------------------------------

TEST(H264Peek, EmptyReturnsUnspecified) {
    EXPECT_EQ(peek_nalu_type({}), NaluType::kUnspecified);
}

TEST(H264Peek, SingleSlice) {
    const std::uint8_t b = 0x61;     // F=0 NRI=3 Type=1 (non-IDR slice)
    EXPECT_EQ(peek_nalu_type({&b, 1}), NaluType::kSliceNonIDR);
}

TEST(H264Peek, IdrSlice) {
    const std::uint8_t b = 0x65;     // NRI=3 Type=5 (IDR)
    EXPECT_EQ(peek_nalu_type({&b, 1}), NaluType::kSliceIDR);
}

// -----------------------------------------------------------------------------
// Single NAL Unit Packet
// -----------------------------------------------------------------------------

TEST(H264Single, ParseAndBuildRoundtrip) {
    const std::uint8_t raw[] = {0x65, 0xAA, 0xBB, 0xCC, 0xDD};   // IDR + payload
    auto n = parse_single({raw, sizeof(raw)});
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(n->type, NaluType::kSliceIDR);
    EXPECT_EQ(n->data.size(), sizeof(raw));
    EXPECT_EQ(n->data[0], 0x65);

    // Build back.
    std::uint8_t out[16] = {0};
    std::size_t n_bytes = build_single(NaluType::kSliceIDR,
                                       core::ByteSpan{raw, sizeof(raw)},
                                       {out, sizeof(out)});
    EXPECT_EQ(n_bytes, sizeof(raw));
    EXPECT_EQ(std::memcmp(out, raw, sizeof(raw)), 0);
}

TEST(H264Single, RejectsFuIndicator) {
    // FU-A indicator: type 28.
    const std::uint8_t raw[] = {0x7C, 0x85, 0xAA};   // 0111 1100 → type 28
    auto n = parse_single({raw, sizeof(raw)});
    EXPECT_FALSE(n.has_value());
}

TEST(H264Single, RejectsStapaIndicator) {
    const std::uint8_t raw[] = {0x78, 0x00, 0x02, 0xAA, 0xBB};
    auto n = parse_single({raw, sizeof(raw)});
    EXPECT_FALSE(n.has_value());
}

TEST(H264Single, RejectsEmpty) {
    auto n = parse_single({});
    EXPECT_FALSE(n.has_value());
}

TEST(H264Single, BuildTooSmallReturnsZero) {
    std::uint8_t tiny[2] = {0, 0};
    const std::uint8_t raw[] = {0x65, 0xAA, 0xBB, 0xCC, 0xDD};
    EXPECT_EQ(build_single(NaluType::kSliceIDR,
                           core::ByteSpan{raw, sizeof(raw)},
                           {tiny, sizeof(tiny)}), 0u);
}

// -----------------------------------------------------------------------------
// STAP-A
// -----------------------------------------------------------------------------

TEST(H264StapA, ParseMultipleNalus) {
    // STAP-A indicator (type 24, NRI=0) + 2 NALUs with length prefixes.
    // NALU1: 0x67 0x42 [SPS bytes]
    // NALU2: 0x68 0xCE [PPS bytes]
    const std::uint8_t raw[] = {
        0x78,                   // STAP-A indicator (NRI=0 type=24)
        0x00, 0x03,             // NALU1 length = 3
        0x67, 0x42, 0x00,       // SPS-ish
        0x00, 0x03,             // NALU2 length = 3
        0x68, 0xCE, 0x06,       // PPS-ish
    };
    auto r = parse_stap_a({raw, sizeof(raw)});
    ASSERT_TRUE(r.parsed_ok);
    ASSERT_EQ(r.nalus.size(), 2u);
    EXPECT_EQ(r.nalus[0].type, NaluType::kSPS);
    EXPECT_EQ(r.nalus[0].data.size(), 3u);
    EXPECT_EQ(r.nalus[1].type, NaluType::kPPS);
    EXPECT_EQ(r.nalus[1].data.size(), 3u);
}

TEST(H264StapA, BuildRoundtrip) {
    const std::uint8_t sps[] = {0x67, 0x42, 0x00};
    const std::uint8_t pps[] = {0x68, 0xCE, 0x06};

    core::ByteSpan nalu_ptrs[2] = {
        core::ByteSpan{sps, sizeof(sps)},
        core::ByteSpan{pps, sizeof(pps)},
    };

    std::uint8_t out[32] = {0};
    std::size_t n_bytes = build_stap_a(nalu_ptrs, 2, {out, sizeof(out)});
    EXPECT_EQ(n_bytes, 1 + 2 + 3 + 2 + 3);

    auto r = parse_stap_a({out, n_bytes});
    ASSERT_TRUE(r.parsed_ok);
    ASSERT_EQ(r.nalus.size(), 2u);
    EXPECT_EQ(r.nalus[0].type, NaluType::kSPS);
    EXPECT_EQ(r.nalus[1].type, NaluType::kPPS);
}

TEST(H264StapA, RejectsEmpty) {
    EXPECT_FALSE(parse_stap_a({}).parsed_ok);
}

TEST(H264StapA, BuildEmptyReturnsZero) {
    std::uint8_t out[8] = {0};
    EXPECT_EQ(build_stap_a(nullptr, 0, {out, sizeof(out)}), 0u);
}

// -----------------------------------------------------------------------------
// FU-A
// -----------------------------------------------------------------------------

TEST(H264FuA, ParseStartFragment) {
    const std::uint8_t raw[] = {
        0x7C,        // FU-A indicator (NRI=3 type=28)
        0x85,        // S=1 E=0 R=0 Type=5 (IDR)
        0xAA, 0xBB,  // fragment bytes
    };
    h264::FuAHeader h;
    ASSERT_TRUE(parse_fu_a_header({raw, sizeof(raw)}, h));
    EXPECT_EQ(h.fu_indicator, 0x7C);
    EXPECT_EQ(h.fu_header, 0x85);
    EXPECT_EQ(h.fragment.size(), 2u);
    EXPECT_EQ(h.fragment[0], 0xAA);
}

TEST(H264FuA, ParseMiddleFragment) {
    const std::uint8_t raw[] = {
        0x7C,        // FU-A indicator
        0x05,        // S=0 E=0 R=0 Type=5 (IDR middle)
        0xCC, 0xDD,
    };
    h264::FuAHeader h;
    ASSERT_TRUE(parse_fu_a_header({raw, sizeof(raw)}, h));
    EXPECT_EQ((h.fu_header & 0x80), 0u);   // S bit not set
    EXPECT_EQ((h.fu_header & 0x40), 0u);   // E bit not set
}

TEST(H264FuA, ParseEndFragment) {
    const std::uint8_t raw[] = {
        0x7C,        // FU-A indicator
        0x45,        // S=0 E=1 R=0 Type=5 (IDR end)
        0xEE,
    };
    h264::FuAHeader h;
    ASSERT_TRUE(parse_fu_a_header({raw, sizeof(raw)}, h));
    EXPECT_NE((h.fu_header & 0x40), 0u);   // E bit set
}

TEST(H264FuA, BuildStartFragment) {
    const std::uint8_t frag[] = {0xAA, 0xBB, 0xCC};
    std::uint8_t out[8] = {0};
    std::size_t n = build_fu_a(NaluType::kSliceIDR,
                               core::ByteSpan{frag, sizeof(frag)},
                               /*start=*/true, /*end=*/false,
                               {out, sizeof(out)});
    EXPECT_EQ(n, 2 + sizeof(frag));
    EXPECT_EQ(out[0] & 0x1F, 28);                  // FU-A indicator type
    EXPECT_NE(out[1] & 0x80, 0);                   // S bit set
    EXPECT_EQ(out[1] & 0x40, 0);                   // E bit clear
    EXPECT_EQ(out[1] & 0x1F, 5);                   // inner NALU type
    EXPECT_EQ(out[2], 0xAA);
}

TEST(H264FuA, BuildEndFragment) {
    const std::uint8_t frag[] = {0xEE};
    std::uint8_t out[8] = {0};
    std::size_t n = build_fu_a(NaluType::kSliceIDR,
                               core::ByteSpan{frag, sizeof(frag)},
                               /*start=*/false, /*end=*/true,
                               {out, sizeof(out)});
    EXPECT_EQ(n, 2 + sizeof(frag));
    EXPECT_EQ(out[1] & 0x80, 0);                   // S bit clear
    EXPECT_NE(out[1] & 0x40, 0);                   // E bit set
}

TEST(H264FuA, RejectsTooShort) {
    const std::uint8_t raw[] = {0x7C};
    h264::FuAHeader h;
    EXPECT_FALSE(parse_fu_a_header({raw, sizeof(raw)}, h));
}

TEST(H264FuA, RejectsNonFuIndicator) {
    const std::uint8_t raw[] = {0x65, 0xAA, 0xBB};   // Single NALU
    h264::FuAHeader h;
    EXPECT_FALSE(parse_fu_a_header({raw, sizeof(raw)}, h));
}

// -----------------------------------------------------------------------------
// Reassembly sanity (parse + concat)
// -----------------------------------------------------------------------------

TEST(H264FuA, ReassembleAcrossFragments) {
    // 6-byte IDR slice split into 3 fragments of 2 bytes each.
    const std::uint8_t original[6] = {0x65, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    // Header byte (0x65) is removed from the original body for FU-A.
    const std::uint8_t* body = original + 1;
    const std::size_t   body_len = 5;

    std::uint8_t buf[3][16];
    std::size_t lens[3];
    lens[0] = build_fu_a(NaluType::kSliceIDR, {body,      2}, true,  false, {buf[0], sizeof(buf[0])});
    lens[1] = build_fu_a(NaluType::kSliceIDR, {body + 2,  2}, false, false, {buf[1], sizeof(buf[1])});
    lens[2] = build_fu_a(NaluType::kSliceIDR, {body + 4,  1}, false, true,  {buf[2], sizeof(buf[2])});

    std::vector<std::uint8_t> reassembled;
    for (int i = 0; i < 3; ++i) {
        h264::FuAHeader h;
        ASSERT_TRUE(parse_fu_a_header({buf[i], lens[i]}, h));
        reassembled.insert(reassembled.end(), h.fragment.begin(), h.fragment.end());
    }
    EXPECT_EQ(reassembled.size(), body_len);
    EXPECT_EQ(std::memcmp(reassembled.data(), body, body_len), 0);
}

// -----------------------------------------------------------------------------
// Top-level dispatcher
// -----------------------------------------------------------------------------

TEST(H264Dispatch, ClassifiesSingle) {
    const std::uint8_t raw[] = {0x65, 0xAA, 0xBB};
    auto r = parse({raw, sizeof(raw)});
    ASSERT_TRUE(r.parsed_ok);
    EXPECT_EQ(r.kind, h264::ParseResult::Kind::kSingle);
    EXPECT_EQ(r.primary.type, NaluType::kSliceIDR);
}

TEST(H264Dispatch, ClassifiesStapA) {
    const std::uint8_t raw[] = {0x78, 0x00, 0x01, 0x67, 0x00, 0x01, 0x68};
    auto r = parse({raw, sizeof(raw)});
    ASSERT_TRUE(r.parsed_ok);
    EXPECT_EQ(r.kind, h264::ParseResult::Kind::kStapA);
    EXPECT_EQ(r.primary.type, NaluType::kSPS);
}

TEST(H264Dispatch, ClassifiesFuA) {
    const std::uint8_t raw[] = {0x7C, 0x85, 0xAA, 0xBB};
    auto r = parse({raw, sizeof(raw)});
    ASSERT_TRUE(r.parsed_ok);
    EXPECT_EQ(r.kind, h264::ParseResult::Kind::kFuAFragment);
    EXPECT_TRUE(r.fu_a_start);
    EXPECT_FALSE(r.fu_a_end);
    EXPECT_EQ(r.primary.type, NaluType::kSliceIDR);
}

TEST(H264Dispatch, RejectsEmpty) {
    auto r = parse({});
    EXPECT_FALSE(r.parsed_ok);
    EXPECT_EQ(r.kind, h264::ParseResult::Kind::kUnsupported);
}
