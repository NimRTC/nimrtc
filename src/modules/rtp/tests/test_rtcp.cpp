/**
 * @file tests/test_rtcp.cpp
 * @brief Tests for the RTCP compound demultiplexer and the REMB message.
 *
 * Covers:
 *   D1  - demux: an empty datagram yields no blocks
 *   D2  - demux: a single SR is classified as kSenderReport
 *   D3  - demux: a compound (SR + RR + SDES) yields three blocks with the
 *        right kinds, and each block's span is the right size
 *   D4  - demux: the FMT field is carried through
 *   D5  - demux: an unknown payload type is kUnknown, not an error
 *   D6  - demux: a truncated tail ends the walk but keeps the head
 *   D7  - demux: a bad version ends the walk but keeps the head
 *   D8  - demux: the block span points into the caller's buffer
 *   R1  - remb: a well-formed message decodes to the expected bitrate
 *   R2  - remb: the mantissa/exponent split is honoured (mantissa << exp)
 *   R3  - remb: the SSRC list is decoded
 *   R4  - remb: a message without the 'REMB' identifier is rejected
 *   R5  - remb: a wrong payload type is rejected
 *   R6  - remb: a wrong FMT is rejected
 *   R7  - remb: a truncated message is rejected
 *   R8  - remb: a bitrate that overflows 64 bits is rejected
 *
 * The REMB wire format is the one libwebrtc implements in
 * `modules/rtp_rtcp/source/rtcp_packet/remb.cc`
 * (draft-alvestrand-rmcat-remb-03): the 'R''E''M''B' identifier sits at
 * payload offset 8, the SSRC count at 12, and the bitrate is
 * `mantissa << exponent` with the exponent in the top 6 bits of byte 13
 * and the 18-bit mantissa split across byte 13 (low 2 bits) and bytes
 * 14..15.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/rtp/rtcp.hpp>

using nimrtc::core::ByteBuffer;
using nimrtc::core::ByteSpan;
using namespace nimrtc::rtp;

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Append a 32-bit big-endian value.
void put_be32(ByteBuffer& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

/// Append a 16-bit big-endian value.
void put_be16(ByteBuffer& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

/// Append a raw byte.
void put_u8(ByteBuffer& out, std::uint8_t v) {
    out.push_back(v);
}

/// Build an RTCP sub-packet header: version 2, `fmt`, `pt`, and a body of
/// `body_len` bytes (the length field counts 32-bit words minus one).
ByteBuffer make_subpacket(std::uint8_t fmt, std::uint8_t pt,
                           const ByteBuffer& body) {
    ByteBuffer out;
    put_u8(out, static_cast<std::uint8_t>(0x80u | (fmt & 0x1Fu)));
    put_u8(out, pt);
    const std::size_t total = 4 + body.size();
    put_be16(out, static_cast<std::uint16_t>(total / 4 - 1));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

/// Build a REMB message (PSFB, FMT=15) the way libwebrtc does.
ByteBuffer make_remb(std::uint32_t sender_ssrc, std::uint64_t bitrate_bps,
                     const std::vector<std::uint32_t>& ssrcs) {
    // libwebrtc's encoder reduces the bitrate to an 18-bit mantissa and
    // shifts the remainder into a 6-bit exponent.
    constexpr std::uint32_t kMaxMantissa = 0x3FFFF;
    std::uint64_t mantissa = bitrate_bps;
    std::uint8_t exponent = 0;
    while (mantissa > kMaxMantissa) {
        mantissa >>= 1;
        ++exponent;
    }

    ByteBuffer body;
    put_be32(body, sender_ssrc);   // SSRC of packet sender
    put_be32(body, 0);              // media source, unused
    put_be32(body, 0x52454D42u);    // 'R' 'E' 'M' 'B'
    put_u8(body, static_cast<std::uint8_t>(ssrcs.size()));
    put_u8(body, static_cast<std::uint8_t>((exponent << 2) | (mantissa >> 16)));
    put_be16(body, static_cast<std::uint16_t>(mantissa & 0xFFFF));
    for (std::uint32_t s : ssrcs) {
        put_be32(body, s);
    }
    return make_subpacket(15, 206, body);
}

}  // namespace

// ---------------------------------------------------------------------------
// Demultiplexer
// ---------------------------------------------------------------------------

TEST(RtcpDemux, EmptyDatagramYieldsNoBlocks) {
    auto blocks = demux_rtcp(ByteSpan{});
    EXPECT_TRUE(blocks.empty());
}

TEST(RtcpDemux, SingleSenderReport) {
    ByteBuffer body;
    put_be32(body, 0x11223344u);
    ByteBuffer pkt = make_subpacket(0, 200, body);

    auto blocks = demux_rtcp(ByteSpan{pkt.data(), pkt.size()});
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].kind, RtcpBlockKind::kSenderReport);
    EXPECT_EQ(blocks[0].payload_type, 200u);
    EXPECT_EQ(blocks[0].fmt, 0u);
    EXPECT_EQ(blocks[0].packet.size(), pkt.size());
}

TEST(RtcpDemux, CompoundPacketIsSplit) {
    ByteBuffer sr_body;
    put_be32(sr_body, 0x11111111u);
    ByteBuffer sr = make_subpacket(0, 200, sr_body);

    ByteBuffer rr_body;
    put_be32(rr_body, 0x22222222u);
    ByteBuffer rr = make_subpacket(0, 201, rr_body);

    ByteBuffer sdes_body;
    put_be32(sdes_body, 0x33333333u);
    ByteBuffer sdes = make_subpacket(0, 202, sdes_body);

    ByteBuffer compound;
    compound.insert(compound.end(), sr.begin(), sr.end());
    compound.insert(compound.end(), rr.begin(), rr.end());
    compound.insert(compound.end(), sdes.begin(), sdes.end());

    auto blocks = demux_rtcp(ByteSpan{compound.data(), compound.size()});
    ASSERT_EQ(blocks.size(), 3u);
    EXPECT_EQ(blocks[0].kind, RtcpBlockKind::kSenderReport);
    EXPECT_EQ(blocks[1].kind, RtcpBlockKind::kReceiverReport);
    EXPECT_EQ(blocks[2].kind, RtcpBlockKind::kSourceDescription);

    // Each block's span is exactly its own sub-packet.
    EXPECT_EQ(blocks[0].packet.size(), sr.size());
    EXPECT_EQ(blocks[1].packet.size(), rr.size());
    EXPECT_EQ(blocks[2].packet.size(), sdes.size());
}

TEST(RtcpDemux, FmtIsCarriedThrough) {
    // PSFB with FMT=15 is REMB; FMT=1 is PLI.  The demuxer classifies by
    // payload type and leaves the refinement to the FMT field.
    ByteBuffer body;
    put_be32(body, 0);
    ByteBuffer pkt = make_subpacket(15, 206, body);

    auto blocks = demux_rtcp(ByteSpan{pkt.data(), pkt.size()});
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].kind, RtcpBlockKind::kPayloadSpecificFeedback);
    EXPECT_EQ(blocks[0].fmt, 15u);
}

TEST(RtcpDemux, UnknownPayloadTypeIsNotAnError) {
    ByteBuffer body;
    put_be32(body, 0);
    ByteBuffer pkt = make_subpacket(0, 99, body);

    auto blocks = demux_rtcp(ByteSpan{pkt.data(), pkt.size()});
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].kind, RtcpBlockKind::kUnknown);
    EXPECT_EQ(blocks[0].payload_type, 99u);
}

TEST(RtcpDemux, TruncatedTailKeepsTheHead) {
    ByteBuffer body;
    put_be32(body, 0x11111111u);
    ByteBuffer sr = make_subpacket(0, 200, body);

    ByteBuffer compound;
    compound.insert(compound.end(), sr.begin(), sr.end());
    // A second sub-packet that claims to be longer than what follows.
    compound.push_back(0x80);
    compound.push_back(201);
    compound.push_back(0x00);
    compound.push_back(0x40);  // claims 260 bytes, only 0 are present

    auto blocks = demux_rtcp(ByteSpan{compound.data(), compound.size()});
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].kind, RtcpBlockKind::kSenderReport);
}

TEST(RtcpDemux, BadVersionEndsTheWalk) {
    ByteBuffer body;
    put_be32(body, 0x11111111u);
    ByteBuffer sr = make_subpacket(0, 200, body);

    ByteBuffer compound;
    compound.insert(compound.end(), sr.begin(), sr.end());
    compound.push_back(0x40);  // version 1
    compound.push_back(201);
    compound.push_back(0x00);
    compound.push_back(0x00);

    auto blocks = demux_rtcp(ByteSpan{compound.data(), compound.size()});
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].kind, RtcpBlockKind::kSenderReport);
}

TEST(RtcpDemux, BlockSpanPointsIntoTheCallerBuffer) {
    ByteBuffer body;
    put_be32(body, 0xAABBCCDDu);
    ByteBuffer pkt = make_subpacket(0, 200, body);

    auto blocks = demux_rtcp(ByteSpan{pkt.data(), pkt.size()});
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].packet.data(), pkt.data());
}

// ---------------------------------------------------------------------------
// REMB
// ---------------------------------------------------------------------------

TEST(RtcpRemb, DecodesABitrate) {
    ByteBuffer pkt = make_remb(0xAABBCCDDu, 1'000'000, {0x11111111u});

    auto r = parse_remb(ByteSpan{pkt.data(), pkt.size()});
    ASSERT_TRUE(r.ok()) << r.error().message();
    EXPECT_EQ(r.value().sender_ssrc, 0xAABBCCDDu);
    EXPECT_EQ(r.value().bitrate_bps, 1'000'000u);
}

TEST(RtcpRemb, HonoursTheMantissaExponentSplit) {
    // 1 Mbit/s does not fit in the 18-bit mantissa, so the encoder shifts
    // it right and records the shift in the exponent.  The decoded value
    // must come back exactly.
    constexpr std::uint64_t kBitrate = 1'000'000;
    ByteBuffer pkt = make_remb(1, kBitrate, {});

    auto r = parse_remb(ByteSpan{pkt.data(), pkt.size()});
    ASSERT_TRUE(r.ok()) << r.error().message();
    EXPECT_EQ(r.value().bitrate_bps, kBitrate);
}

TEST(RtcpRemb, DecodesTheSsrcList) {
    ByteBuffer pkt = make_remb(7, 500'000, {0x11111111u, 0x22222222u});

    auto r = parse_remb(ByteSpan{pkt.data(), pkt.size()});
    ASSERT_TRUE(r.ok()) << r.error().message();
    ASSERT_EQ(r.value().ssrcs.size(), 2u);
    EXPECT_EQ(r.value().ssrcs[0], 0x11111111u);
    EXPECT_EQ(r.value().ssrcs[1], 0x22222222u);
}

TEST(RtcpRemb, RejectsAMissingIdentifier) {
    ByteBuffer pkt = make_remb(1, 500'000, {});
    pkt[12] = 'X';  // break the 'REMB' magic

    auto r = parse_remb(ByteSpan{pkt.data(), pkt.size()});
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), nimrtc::core::ErrorCode::ProtocolError);
}

TEST(RtcpRemb, RejectsAWrongPayloadType) {
    ByteBuffer pkt = make_remb(1, 500'000, {});
    pkt[1] = 205;  // RTPFB instead of PSFB

    auto r = parse_remb(ByteSpan{pkt.data(), pkt.size()});
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), nimrtc::core::ErrorCode::ProtocolError);
}

TEST(RtcpRemb, RejectsAWrongFmt) {
    ByteBuffer pkt = make_remb(1, 500'000, {});
    pkt[0] = static_cast<std::uint8_t>(0x80u | 1u);  // FMT=1 is PLI

    auto r = parse_remb(ByteSpan{pkt.data(), pkt.size()});
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), nimrtc::core::ErrorCode::ProtocolError);
}

TEST(RtcpRemb, RejectsATruncatedMessage) {
    ByteBuffer pkt = make_remb(1, 500'000, {0x11111111u});
    pkt.resize(8);  // shorter than the mandatory fields

    auto r = parse_remb(ByteSpan{pkt.data(), pkt.size()});
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), nimrtc::core::ErrorCode::ProtocolError);
}

TEST(RtcpRemb, RejectsATruncatedSsrcList) {
    ByteBuffer pkt = make_remb(1, 500'000, {0x11111111u, 0x22222222u});
    // Claim two SSRCs but truncate the packet after the first one.
    pkt.resize(pkt.size() - 4);

    auto r = parse_remb(ByteSpan{pkt.data(), pkt.size()});
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), nimrtc::core::ErrorCode::ProtocolError);
}

TEST(RtcpRemb, RejectsAnOverflowingBitrate) {
    // A 6-bit exponent with an 18-bit mantissa can shift past 64 bits.
    ByteBuffer pkt = make_remb(1, 500'000, {});
    pkt[13] = static_cast<std::uint8_t>((63u << 2) | 0x03u);
    pkt[14] = 0xFF;
    pkt[15] = 0xFF;

    auto r = parse_remb(ByteSpan{pkt.data(), pkt.size()});
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), nimrtc::core::ErrorCode::ProtocolError);
}
