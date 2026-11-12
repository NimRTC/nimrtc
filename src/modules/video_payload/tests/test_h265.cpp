/**
 * @file src/modules/video_payload/tests/test_h265.cpp
 * @brief H.265 / HEVC RTP payload unit tests (RFC 7798).
 *
 * Covers the two-byte NAL unit header, the single-NAL-unit packet, the
 * AP (aggregation packet, type 48), the FU (fragmentation unit, type 49)
 * and the type predicates.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <nimrtc/video_payload/h265.hpp>

namespace h265 = nimrtc::video_payload::h265;

using h265::NaluType;
using h265::NaluHeader;
using h265::Nalu;

// -----------------------------------------------------------------------------
// NAL unit header
// -----------------------------------------------------------------------------

TEST(H265NaluHeader, ParsesATwoByteHeader) {
    // byte 0: F(1) | Type(6) | LayerId MSB(1)
    // byte 1: LayerId low(5)   | TID(3)
    // Type = 19 (IDR_W_RADL), LayerId = 0, TID = 1.
    const std::uint8_t raw[] = {0x26, 0x00};
    auto h = h265::parse_nalu_header({raw, sizeof(raw)});
    ASSERT_TRUE(h.has_value());
    EXPECT_EQ(h->forbidden, 0u);
    EXPECT_EQ(h->type, NaluType::kSliceIDR_W_RADL);
    EXPECT_EQ(h->layer_id, 0u);
    EXPECT_EQ(h->tid, 1u);
}

TEST(H265NaluHeader, ParsesAVpsHeader) {
    // Type = 32 (VPS): byte0 = 32 << 1 = 0x40.
    const std::uint8_t raw[] = {0x40, 0x00};
    auto h = h265::parse_nalu_header({raw, sizeof(raw)});
    ASSERT_TRUE(h.has_value());
    EXPECT_EQ(h->type, NaluType::kVPS);
}

TEST(H265NaluHeader, ParsesTheLayerId) {
    // LayerId = 1 → byte0 bit0 = 0, byte1 bits 7..3 = 0x08.
    const std::uint8_t raw[] = {0x26, 0x08};
    auto h = h265::parse_nalu_header({raw, sizeof(raw)});
    ASSERT_TRUE(h.has_value());
    EXPECT_EQ(h->layer_id, 1u);
    EXPECT_EQ(h->tid, 1u);
}

TEST(H265NaluHeader, RejectsATooShortPayload) {
    const std::uint8_t raw[] = {0x26};
    EXPECT_FALSE(h265::parse_nalu_header({raw, sizeof(raw)}).has_value());
}

TEST(H265NaluHeader, BuildsTheHeader) {
    std::uint8_t out[2] = {0, 0};
    const std::size_t n = h265::build_payload_header(
        NaluType::kSliceIDR_W_RADL, /*layer_id=*/0, /*tid=*/1,
        {out, sizeof(out)});
    EXPECT_EQ(n, 2u);
    EXPECT_EQ(out[0], 0x26);
    EXPECT_EQ(out[1], 0x00);
}

TEST(H265NaluHeader, BuildRejectsATooSmallBuffer) {
    std::uint8_t out[1] = {0};
    EXPECT_EQ(h265::build_payload_header(NaluType::kVPS, 0, 1, {out, 1}), 0u);
}

// -----------------------------------------------------------------------------
// Type predicates
// -----------------------------------------------------------------------------

TEST(H265NaluTypes, IrapTypes) {
    EXPECT_TRUE(h265::is_irap_nalu(NaluType::kSliceBLA_W_LP));
    EXPECT_TRUE(h265::is_irap_nalu(NaluType::kSliceIDR_W_RADL));
    EXPECT_TRUE(h265::is_irap_nalu(NaluType::kSliceCRA_NUT));
    EXPECT_FALSE(h265::is_irap_nalu(NaluType::kSliceTRAIL_R));
    EXPECT_FALSE(h265::is_irap_nalu(NaluType::kVPS));
}

TEST(H265NaluTypes, ParameterSets) {
    EXPECT_TRUE(h265::is_parameter_set_nalu(NaluType::kVPS));
    EXPECT_TRUE(h265::is_parameter_set_nalu(NaluType::kSPS));
    EXPECT_TRUE(h265::is_parameter_set_nalu(NaluType::kPPS));
    EXPECT_FALSE(h265::is_parameter_set_nalu(NaluType::kSliceTRAIL_N));
}

TEST(H265NaluTypes, Vcl) {
    EXPECT_TRUE(h265::is_vcl_nalu(NaluType::kSliceTRAIL_N));
    EXPECT_TRUE(h265::is_vcl_nalu(NaluType::kSliceIDR_N_LP));
    EXPECT_FALSE(h265::is_vcl_nalu(NaluType::kPrefixSEI));
}

TEST(H265NaluTypes, Peek) {
    const std::uint8_t idr[] = {0x26, 0x01, 0xAA};
    EXPECT_EQ(h265::peek_nalu_type({idr, sizeof(idr)}),
              NaluType::kSliceIDR_W_RADL);
    EXPECT_EQ(h265::peek_payload_type({idr, sizeof(idr)}),
              NaluType::kSliceIDR_W_RADL);

    // An AP packet (type 48) and an FU packet (type 49).
    const std::uint8_t ap[] = {0x60, 0x00};
    EXPECT_EQ(h265::peek_payload_type({ap, sizeof(ap)}), NaluType::kAP);
    const std::uint8_t fu[] = {0x62, 0x00, 0x01};
    EXPECT_EQ(h265::peek_payload_type({fu, sizeof(fu)}), NaluType::kFU);
}

// -----------------------------------------------------------------------------
// Single NAL unit packet
// -----------------------------------------------------------------------------

TEST(H265SingleNal, ParsesAVclNal) {
    const std::uint8_t raw[] = {0x26, 0x01, 0xAA, 0xBB};
    auto n = h265::parse_single({raw, sizeof(raw)});
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(n->type, NaluType::kSliceIDR_W_RADL);
    EXPECT_EQ(n->data.size(), 4u);
}

TEST(H265SingleNal, RejectsAWrapperType) {
    // Type 49 is an FU — not a single NAL unit packet.
    const std::uint8_t raw[] = {0x62, 0x00, 0xAA};
    EXPECT_FALSE(h265::parse_single({raw, sizeof(raw)}).has_value());
}

TEST(H265SingleNal, RejectsAnEmptyPayload) {
    const std::uint8_t raw[] = {0x00};
    EXPECT_FALSE(h265::parse_single({raw, sizeof(raw)}).has_value());
}

// -----------------------------------------------------------------------------
// FU — fragmentation unit (RFC 7798 §4.3.3)
// -----------------------------------------------------------------------------

TEST(H265Fu, ParsesTheFuHeader) {
    // payload header (type 49) + FU header: S=1 E=0 FuType=19
    const std::uint8_t raw[] = {0x62, 0x00, 0x93};
    h265::FuHeader h{};
    ASSERT_TRUE(h265::parse_fu_header({raw, sizeof(raw)}, h));
    EXPECT_TRUE(h.start);
    EXPECT_FALSE(h.end);
    EXPECT_EQ(h.fu_type, NaluType::kSliceIDR_W_RADL);
}

TEST(H265Fu, BuildsAFragment) {
    const std::uint8_t frag[] = {0xAA, 0xBB};
    std::uint8_t out[8] = {0};
    const std::size_t n = h265::build_fu(
        NaluType::kSliceIDR_W_RADL, /*layer_id=*/0, /*tid=*/1,
        {frag, sizeof(frag)}, /*start=*/true, /*end=*/true,
        {out, sizeof(out)});
    ASSERT_EQ(n, 5u);
    // payload header: type 49
    EXPECT_EQ(out[0], 0x62);
    EXPECT_EQ(out[1], 0x00);
    // FU header: S=1 E=1 FuType=19 → 0b110_010011 = 0xD3
    // (RFC 7798 §4.3.3: S(1) | E(1) | FuType(6))
    EXPECT_EQ(out[2], 0xD3);
    EXPECT_EQ(out[3], 0xAA);
    EXPECT_EQ(out[4], 0xBB);
}

TEST(H265Fu, RoundTrip) {
    const std::uint8_t frag[] = {0x01, 0x02, 0x03};
    std::uint8_t out[16] = {0};
    const std::size_t n = h265::build_fu(
        NaluType::kSliceTRAIL_R, 0, 1, {frag, sizeof(frag)},
        /*start=*/false, /*end=*/true, {out, sizeof(out)});
    ASSERT_EQ(n, 6u);
    h265::FuHeader h{};
    ASSERT_TRUE(h265::parse_fu_header({out, n}, h));
    EXPECT_FALSE(h.start);
    EXPECT_TRUE(h.end);
    EXPECT_EQ(h.fu_type, NaluType::kSliceTRAIL_R);
}

// -----------------------------------------------------------------------------
// AP — aggregation packet (RFC 7798 §4.3.2)
// -----------------------------------------------------------------------------

TEST(H265Ap, RoundTrip) {
    // Two NAL units, each with its two-byte header.
    const std::uint8_t vps[] = {0x40, 0x00, 0x0C};
    const std::uint8_t sps[] = {0x42, 0x00, 0x01};
    std::vector<h265::Nalu> nalus;
    nalus.push_back({NaluType::kVPS, {vps, sizeof(vps)}, false});
    nalus.push_back({NaluType::kSPS, {sps, sizeof(sps)}, false});

    std::uint8_t out[64] = {0};
    const std::size_t n = h265::build_ap(nalus, /*layer_id=*/0, /*tid=*/1,
                                         {out, sizeof(out)});
    ASSERT_GT(n, 0u);

    // The payload header names the AP type (48).
    EXPECT_EQ(out[0] >> 1, 48);

    std::vector<h265::Nalu> parsed;
    ASSERT_TRUE(h265::parse_ap({out, n}, /*has_donl=*/false, parsed));
    ASSERT_EQ(parsed.size(), 2u);
    EXPECT_EQ(parsed[0].type, NaluType::kVPS);
    EXPECT_EQ(parsed[0].data.size(), 3u);
    EXPECT_EQ(parsed[1].type, NaluType::kSPS);
    EXPECT_EQ(parsed[1].data.size(), 3u);
}

TEST(H265Ap, ParsesADonlPrefixedPacket) {
    // payload header (2) + DONL (2) + size (2) + one NAL unit (3)
    const std::uint8_t raw[] = {
        0x60, 0x00,             // payload header: type 48
        0x00, 0x07,             // DONL
        0x00, 0x03,             // size
        0x26, 0x00, 0xAA,       // the NAL unit
    };
    std::vector<h265::Nalu> parsed;
    ASSERT_TRUE(h265::parse_ap({raw, sizeof(raw)}, /*has_donl=*/true, parsed));
    ASSERT_EQ(parsed.size(), 1u);
    EXPECT_EQ(parsed[0].type, NaluType::kSliceIDR_W_RADL);
    EXPECT_EQ(parsed[0].data.size(), 3u);
}

TEST(H265Ap, RejectsATruncatedPacket) {
    const std::uint8_t raw[] = {0x60, 0x00, 0x00, 0x10, 0xAA};
    std::vector<h265::Nalu> parsed;
    EXPECT_FALSE(h265::parse_ap({raw, sizeof(raw)}, /*has_donl=*/false, parsed));
}
