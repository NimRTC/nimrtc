/**
 * @file src/modules/video_payload/tests/test_av1.cpp
 * @brief AV1 RTP payload unit tests (draft-ietf-payload-av1 /
 *        aomediacodec av1-rtp-spec v1.0.0).
 *
 * Covers the OBU header, the aggregation header, the LEB128 length
 * encoding, the Annex-B framing and the packetize / reassemble round
 * trip.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/video_payload/av1.hpp>

namespace av1 = nimrtc::video_payload::av1;
namespace core = nimrtc::core;

using av1::ObuType;
using av1::Obu;
using av1::ObuHeader;
using av1::AggregationHeader;

// -----------------------------------------------------------------------------
// The OBU header
// -----------------------------------------------------------------------------

TEST(Av1ObuHeader, ParsesASequenceHeader) {
    // obu_type = 1 (sequence header) → 1 << 3 = 0x08
    const std::uint8_t raw[] = {0x08};
    auto h = av1::parse_obu_header({raw, sizeof(raw)});
    ASSERT_TRUE(h.has_value());
    EXPECT_EQ(h->type, ObuType::kSequenceHeader);
    EXPECT_FALSE(h->has_extension);
    EXPECT_FALSE(h->has_size);
    EXPECT_EQ(h->header_size, 1u);
}

TEST(Av1ObuHeader, ParsesTheHasSizeFlag) {
    // obu_type = 6 (frame) + obu_has_size_field → 0x32
    const std::uint8_t raw[] = {0x32, 0x05};
    auto h = av1::parse_obu_header({raw, sizeof(raw)});
    ASSERT_TRUE(h.has_value());
    EXPECT_EQ(h->type, ObuType::kFrame);
    EXPECT_TRUE(h->has_size);
    // 1 header byte + 1 LEB128 byte
    EXPECT_EQ(h->header_size, 2u);
}

TEST(Av1ObuHeader, ParsesTheExtensionHeader) {
    // obu_type = 3 (frame header) + extension flag → 0x1C, then the
    // extension byte.
    const std::uint8_t raw[] = {0x1C, 0x00};
    auto h = av1::parse_obu_header({raw, sizeof(raw)});
    ASSERT_TRUE(h.has_value());
    EXPECT_EQ(h->type, ObuType::kFrameHeader);
    EXPECT_TRUE(h->has_extension);
    EXPECT_EQ(h->header_size, 2u);
}

TEST(Av1ObuHeader, RejectsAnEmptyPayload) {
    EXPECT_FALSE(av1::parse_obu_header({}).has_value());
}

TEST(Av1ObuHeader, RejectsATruncatedLeb128) {
    // has_size set, but the LEB128 continuation byte runs off the end.
    const std::uint8_t raw[] = {0x32, 0x80};
    EXPECT_FALSE(av1::parse_obu_header({raw, sizeof(raw)}).has_value());
}

TEST(Av1ObuHeader, Peek) {
    const std::uint8_t raw[] = {0x08};
    EXPECT_EQ(av1::peek_obu_type({raw, sizeof(raw)}), ObuType::kSequenceHeader);
}

// -----------------------------------------------------------------------------
// LEB128
// -----------------------------------------------------------------------------

TEST(Av1Leb128, SingleByte) {
    EXPECT_EQ(av1::leb128_size(0), 1u);
    EXPECT_EQ(av1::leb128_size(127), 1u);
    EXPECT_EQ(av1::leb128_size(128), 2u);
}

TEST(Av1Leb128, RoundTrip) {
    for (std::size_t v : {std::size_t{0}, std::size_t{1}, std::size_t{127},
                           std::size_t{128}, std::size_t{16383},
                           std::size_t{16384}, std::size_t{100000}}) {
        std::uint8_t buf[10] = {0};
        const std::size_t n = av1::write_leb128(
            v, core::MutableByteSpan{buf, sizeof(buf)});
        EXPECT_EQ(n, av1::leb128_size(v)) << "value " << v;
        std::size_t pos = 0;
        std::size_t out = 0;
        ASSERT_TRUE(av1::read_leb128({buf, n}, pos, out));
        EXPECT_EQ(out, v);
        EXPECT_EQ(pos, n);
    }
}

TEST(Av1Leb128, RejectsATruncatedValue) {
    const std::uint8_t raw[] = {0x80};
    std::size_t pos = 0;
    std::size_t out = 0;
    EXPECT_FALSE(av1::read_leb128({raw, sizeof(raw)}, pos, out));
}

// -----------------------------------------------------------------------------
// The aggregation header
// -----------------------------------------------------------------------------

TEST(Av1AggregationHeader, RoundTrip) {
    AggregationHeader h;
    h.starts_with_fragment = true;
    h.ends_with_fragment   = true;
    h.element_count        = 0;      // "every element has a length field"
    h.new_sequence         = true;

    std::uint8_t out[1] = {0};
    ASSERT_EQ(av1::write_aggregation_header(h, {out, 1}), 1u);
    // Z=1 Y=1 W=00 N=1
    EXPECT_EQ(out[0], 0xC8);

    auto parsed = av1::parse_aggregation_header({out, 1});
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->starts_with_fragment);
    EXPECT_TRUE(parsed->ends_with_fragment);
    EXPECT_EQ(parsed->element_count, 0u);
    EXPECT_TRUE(parsed->new_sequence);
}

TEST(Av1AggregationHeader, ElementCountOne) {
    AggregationHeader h;
    h.element_count = 1;
    std::uint8_t out[1] = {0};
    ASSERT_EQ(av1::write_aggregation_header(h, {out, 1}), 1u);
    // W = 1 → 1 << 4 = 0x10
    EXPECT_EQ(out[0], 0x10);

    auto parsed = av1::parse_aggregation_header({out, 1});
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->element_count, 1u);
}

// -----------------------------------------------------------------------------
// Annex-B framing
// -----------------------------------------------------------------------------

TEST(Av1AnnexB, ParsesTwoObus) {
    // sequence header OBU (type 1) + frame OBU (type 6)
    const std::uint8_t bs[] = {
        0x00, 0x00, 0x00, 0x01, 0x08, 0xAA, 0xBB,
        0x00, 0x00, 0x00, 0x01, 0x32, 0xCC,
    };
    std::vector<Obu> obus;
    ASSERT_TRUE(av1::parse_annex_b_obus({bs, sizeof(bs)}, obus));
    ASSERT_EQ(obus.size(), 2u);
    EXPECT_EQ(obus[0].type, ObuType::kSequenceHeader);
    EXPECT_EQ(obus[0].data.size(), 3u);
    EXPECT_EQ(obus[1].type, ObuType::kFrame);
    EXPECT_EQ(obus[1].data.size(), 2u);
}

TEST(Av1AnnexB, DetectsTheSequenceHeader) {
    const std::uint8_t with_seq[] = {
        0x00, 0x00, 0x00, 0x01, 0x08, 0xAA,
        0x00, 0x00, 0x00, 0x01, 0x32, 0xCC,
    };
    EXPECT_TRUE(av1::contains_sequence_header({with_seq, sizeof(with_seq)}));

    const std::uint8_t without_seq[] = {
        0x00, 0x00, 0x00, 0x01, 0x32, 0xCC,
    };
    EXPECT_FALSE(av1::contains_sequence_header({without_seq, sizeof(without_seq)}));
}

// -----------------------------------------------------------------------------
// Packetize / reassemble
// -----------------------------------------------------------------------------

TEST(Av1Packetize, SingleObuFitsInOnePacket) {
    const std::uint8_t seq[] = {0x08, 0xAA, 0xBB};
    std::vector<Obu> obus;
    obus.push_back({ObuType::kSequenceHeader, {seq, sizeof(seq)}});

    std::vector<av1::Packet> packets;
    ASSERT_TRUE(av1::packetize(obus, /*mtu=*/1200, /*new_sequence=*/true,
                                packets));
    ASSERT_EQ(packets.size(), 1u);
    EXPECT_TRUE(packets[0].marker);
    // Aggregation header + the OBU bytes, no length field (W = 1).
    EXPECT_EQ(packets[0].buffer.size(), 4u);
    EXPECT_EQ(packets[0].buffer[0], 0x18);   // W=1, N=1
}

TEST(Av1Packetize, AggregatesSmallObus) {
    const std::uint8_t seq[] = {0x08, 0xAA};
    const std::uint8_t frm[] = {0x32, 0xBB};
    std::vector<Obu> obus;
    obus.push_back({ObuType::kSequenceHeader, {seq, sizeof(seq)}});
    obus.push_back({ObuType::kFrame, {frm, sizeof(frm)}});

    std::vector<av1::Packet> packets;
    ASSERT_TRUE(av1::packetize(obus, /*mtu=*/1200, /*new_sequence=*/true,
                                packets));
    ASSERT_EQ(packets.size(), 1u);
    // W = 0 → every element carries a LEB128 length field.
    EXPECT_EQ(packets[0].buffer[0], 0x08);
    // 1 (agg header) + 1 (len) + 2 (obu) + 1 (len) + 2 (obu)
    EXPECT_EQ(packets[0].buffer.size(), 7u);
}

TEST(Av1Packetize, FragmentsALargeObu) {
    // A sequence header OBU whose payload is far larger than the MTU.
    std::vector<std::uint8_t> big;
    big.push_back(0x32);          // frame OBU with a size field
    big.push_back(0x00);          // LEB128 size = 0 (patched below)
    big.insert(big.end(), 400, 0xAB);

    std::vector<Obu> obus;
    obus.push_back({ObuType::kFrame, {big.data(), big.size()}});

    std::vector<av1::Packet> packets;
    ASSERT_TRUE(av1::packetize(obus, /*mtu=*/100, /*new_sequence=*/true,
                                packets));
    ASSERT_GT(packets.size(), 1u);

    // The first fragment carries the OBU header (Z = 0, Y = 1).
    auto first = av1::parse_aggregation_header(packets[0].payload);
    ASSERT_TRUE(first.has_value());
    EXPECT_FALSE(first->starts_with_fragment);
    EXPECT_TRUE(first->ends_with_fragment);

    // The last fragment completes the OBU (Z = 1, Y = 0).
    auto last = av1::parse_aggregation_header(packets.back().payload);
    ASSERT_TRUE(last.has_value());
    EXPECT_TRUE(last->starts_with_fragment);
    EXPECT_FALSE(last->ends_with_fragment);
}

TEST(Av1Reassemble, RoundTripsASingleObu) {
    const std::uint8_t seq[] = {0x08, 0xAA, 0xBB};
    std::vector<Obu> obus;
    obus.push_back({ObuType::kSequenceHeader, {seq, sizeof(seq)}});

    std::vector<av1::Packet> packets;
    ASSERT_TRUE(av1::packetize(obus, 1200, true, packets));
    ASSERT_EQ(packets.size(), 1u);

    av1::ObuReassembler rx;
    std::vector<std::uint8_t> annex_b;
    ASSERT_TRUE(rx.push(packets[0].payload, annex_b));
    EXPECT_TRUE(rx.has_sequence_header());

    // Annex-B framing: a four-byte start code + the OBU bytes.
    ASSERT_EQ(annex_b.size(), 4u + 3u);
    EXPECT_EQ(annex_b[3], 0x01);
    EXPECT_EQ(annex_b[4], 0x08);
    EXPECT_EQ(annex_b[5], 0xAA);
    EXPECT_EQ(annex_b[6], 0xBB);
}

TEST(Av1Reassemble, RoundTripsAnAggregatedFrame) {
    const std::uint8_t seq[] = {0x08, 0xAA};
    const std::uint8_t frm[] = {0x32, 0xBB};
    std::vector<Obu> obus;
    obus.push_back({ObuType::kSequenceHeader, {seq, sizeof(seq)}});
    obus.push_back({ObuType::kFrame, {frm, sizeof(frm)}});

    std::vector<av1::Packet> packets;
    ASSERT_TRUE(av1::packetize(obus, 1200, true, packets));
    ASSERT_EQ(packets.size(), 1u);

    av1::ObuReassembler rx;
    std::vector<std::uint8_t> annex_b;
    ASSERT_TRUE(rx.push(packets[0].payload, annex_b));
    EXPECT_TRUE(rx.has_sequence_header());

    // Two OBUs, each with its own four-byte start code.
    EXPECT_EQ(annex_b.size(), (4u + 2u) * 2);
    EXPECT_EQ(annex_b[4], 0x08);
    EXPECT_EQ(annex_b[4 + 4 + 2], 0x32);
}

TEST(Av1Reassemble, RoundTripsAFragmentedObu) {
    std::vector<std::uint8_t> big;
    big.push_back(0x32);
    big.push_back(0x00);
    big.insert(big.end(), 400, 0xAB);

    std::vector<Obu> obus;
    obus.push_back({ObuType::kFrame, {big.data(), big.size()}});

    std::vector<av1::Packet> packets;
    ASSERT_TRUE(av1::packetize(obus, /*mtu=*/100, false, packets));
    ASSERT_GT(packets.size(), 1u);

    av1::ObuReassembler rx;
    std::vector<std::uint8_t> annex_b;
    for (const auto& p : packets) {
        ASSERT_TRUE(rx.push(p.payload, annex_b));
    }
    // The reassembled OBU carries the full original bytes.
    EXPECT_EQ(annex_b.size(), 4u + big.size());
    EXPECT_EQ(annex_b[4], 0x32);
}

TEST(Av1Reassemble, ResetForgetsTheSequenceHeader) {
    const std::uint8_t seq[] = {0x08, 0xAA};
    std::vector<Obu> obus;
    obus.push_back({ObuType::kSequenceHeader, {seq, sizeof(seq)}});

    std::vector<av1::Packet> packets;
    ASSERT_TRUE(av1::packetize(obus, 1200, true, packets));

    av1::ObuReassembler rx;
    std::vector<std::uint8_t> annex_b;
    ASSERT_TRUE(rx.push(packets[0].payload, annex_b));
    EXPECT_TRUE(rx.has_sequence_header());
    rx.reset();
    EXPECT_FALSE(rx.has_sequence_header());
}
