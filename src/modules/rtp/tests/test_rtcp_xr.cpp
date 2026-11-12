/**
 * @file tests/test_rtcp_xr.cpp
 * @brief Tests for the RTCP Extended Report blocks (RFC 3611) and the
 *        TMMBR / TMMBN messages (RFC 5104 §4.2).
 *
 * Covers (session capability plan X1 / X2 / X3):
 *   X1  - the Receiver Reference Time block (BT=4) round-trips
 *   X2  - the DLRR block (BT=5) round-trips, sub-block by sub-block
 *   X3  - the Statistics Summary block (BT=6) round-trips, flags and all
 *   X4  - a block type this build does not parse is skipped, not an error
 *   X5  - a truncated block is a protocol error
 *   X6  - a block whose length exceeds the packet is a protocol error
 *   X7  - the XR packet's own header is validated (version, payload type)
 *   X8  - a TMMBR round-trips through the FCI encoding
 *   X9  - the MxTBR exponent / mantissa split survives the round-trip
 *   X10 - a TMMBN with no FCI entry is legal
 *   X11 - a TMMBR with no FCI entry builds nothing (RFC 5104 §4.2.1.1)
 *   X12 - a wrong FMT / payload type is rejected
 *
 * The block numbering is the IANA "RTCP XR Block Type" registry of
 * RFC 3611 §6.2: BT 1 = Loss RLE, BT 4 = Receiver Reference Time,
 * BT 5 = DLRR, BT 6 = Statistics Summary, BT 7 = VoIP Metrics.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/rtp/rtcp_tmmbr.hpp>
#include <nimrtc/rtp/rtcp_xr.hpp>

using nimrtc::core::ByteBuffer;
using nimrtc::core::ByteSpan;
using namespace nimrtc::rtp;

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Build a raw XR packet around the given block bytes. */
ByteBuffer make_xr_packet(std::uint32_t ssrc, const ByteBuffer& body) {
    ByteBuffer out(8, 0);
    out[0] = 0x80;   // V=2, no padding
    out[1] = 207;   // PT=XR
    out[4] = static_cast<std::uint8_t>(ssrc >> 24);
    out[5] = static_cast<std::uint8_t>(ssrc >> 16);
    out[6] = static_cast<std::uint8_t>(ssrc >> 8);
    out[7] = static_cast<std::uint8_t>(ssrc);
    out.insert(out.end(), body.begin(), body.end());
    // The length field counts 32-bit words minus one (RFC 3611 §2).
    const std::uint16_t words =
        static_cast<std::uint16_t>(out.size() / 4u - 1u);
    out[2] = static_cast<std::uint8_t>(words >> 8);
    out[3] = static_cast<std::uint8_t>(words & 0xFF);
    return out;
}

/** A raw block with an arbitrary type, for the "unknown block" cases. */
ByteBuffer make_raw_block(std::uint8_t bt, std::uint8_t type_specific,
                           std::size_t words_of_content) {
    ByteBuffer out(4 + words_of_content * 4u, 0);
    out[0] = bt;
    out[1] = type_specific;
    const std::uint16_t length =
        static_cast<std::uint16_t>(out.size() / 4u - 1u);
    out[2] = static_cast<std::uint8_t>(length >> 8);
    out[3] = static_cast<std::uint8_t>(length & 0xFF);
    return out;
}

ByteSpan span_of(const ByteBuffer& b) {
    return ByteSpan(b.data(), b.size());
}

}  // anonymous namespace


// ---------------------------------------------------------------------------
// X1 — the Receiver Reference Time block (BT=4)
// ---------------------------------------------------------------------------

TEST(RtcpXr, ReceiverReferenceTimeRoundTrips) {
    const std::uint64_t ntp = 0x1234567890ABCDEFull;
    const ByteBuffer block = build_xr_receiver_reference_time(ntp);

    // The block is 3 words: the header plus the 64-bit timestamp.
    ASSERT_EQ(block.size(), 12u);
    EXPECT_EQ(block[0], 4);                       // BT
    // The length field counts 32-bit words minus one: 3 - 1 = 2.
    EXPECT_EQ(block[2], 0);
    EXPECT_EQ(block[3], 2);

    const ByteBuffer packet = make_xr_packet(0xDEADBEEF, block);
    auto parsed = parse_xr(span_of(packet));
    ASSERT_TRUE(parsed.ok());
    ASSERT_EQ(parsed.value().size(), 1u);

    const XrBlock& b = parsed.value()[0];
    EXPECT_EQ(b.type, XrBlockType::kReceiverReferenceTime);
    EXPECT_EQ(b.receiver_reference_time.ntp_timestamp, ntp);
}


// ---------------------------------------------------------------------------
// X2 — the DLRR block (BT=5)
// ---------------------------------------------------------------------------

TEST(RtcpXr, DlrrRoundTrips) {
    std::vector<XrDlrrSubReport> subs;
    XrDlrrSubReport a;
    a.ssrc    = 0x11223344;
    a.last_rr = 0x55667788;
    a.dlrr    = 0x0000ABCD;
    subs.push_back(a);

    XrDlrrSubReport b;
    b.ssrc    = 0x99AABBCC;
    b.last_rr = 0;
    b.dlrr    = 0;
    subs.push_back(b);

    const ByteBuffer block = build_xr_dlrr(subs);
    // Header plus two 3-word sub-blocks.
    ASSERT_EQ(block.size(), 4u + 2u * 12u);
    EXPECT_EQ(block[0], 5);                       // BT

    const ByteBuffer packet = make_xr_packet(0x01020304, block);
    auto parsed = parse_xr(span_of(packet));
    ASSERT_TRUE(parsed.ok());
    ASSERT_EQ(parsed.value().size(), 1u);

    const XrBlock& parsed_block = parsed.value()[0];
    EXPECT_EQ(parsed_block.type, XrBlockType::kDlrr);
    ASSERT_EQ(parsed_block.dlrr.sub_reports.size(), 2u);
    EXPECT_EQ(parsed_block.dlrr.sub_reports[0].ssrc, a.ssrc);
    EXPECT_EQ(parsed_block.dlrr.sub_reports[0].last_rr, a.last_rr);
    EXPECT_EQ(parsed_block.dlrr.sub_reports[0].dlrr, a.dlrr);
    EXPECT_EQ(parsed_block.dlrr.sub_reports[1].ssrc, b.ssrc);
    EXPECT_EQ(parsed_block.dlrr.sub_reports[1].last_rr, 0u);
}


TEST(RtcpXr, AnEmptyDlrrBlockIsLegal) {
    const ByteBuffer block = build_xr_dlrr({});
    EXPECT_EQ(block.size(), 4u);

    const ByteBuffer packet = make_xr_packet(1, block);
    auto parsed = parse_xr(span_of(packet));
    ASSERT_TRUE(parsed.ok());
    ASSERT_EQ(parsed.value().size(), 1u);
    EXPECT_EQ(parsed.value()[0].type, XrBlockType::kDlrr);
    EXPECT_TRUE(parsed.value()[0].dlrr.sub_reports.empty());
}


// ---------------------------------------------------------------------------
// X3 — the Statistics Summary block (BT=6)
// ---------------------------------------------------------------------------

TEST(RtcpXr, StatisticsSummaryRoundTrips) {
    XrStatisticsSummary s;
    s.ssrc       = 0xCAFEBABE;
    s.begin_seq  = 1000;
    s.end_seq    = 2000;
    s.has_loss   = true;
    s.lost_packets = 42;
    s.has_duplicates = true;
    s.dup_packets = 7;
    s.has_jitter = true;
    s.min_jitter  = 11;
    s.max_jitter  = 22;
    s.mean_jitter = 33;
    s.dev_jitter  = 44;
    s.ttl_or_hop_limit_kind = 1;   // IPv4 TTL
    s.min_ttl_or_hl  = 64;
    s.max_ttl_or_hl  = 65;
    s.mean_ttl_or_hl = 66;
    s.dev_ttl_or_hl  = 1;

    const ByteBuffer block = build_xr_statistics_summary(s);
    // The block is 10 words.
    ASSERT_EQ(block.size(), 40u);
    EXPECT_EQ(block[0], 6);                       // BT

    const ByteBuffer packet = make_xr_packet(0x0F0F0F0F, block);
    auto parsed = parse_xr(span_of(packet));
    ASSERT_TRUE(parsed.ok());
    ASSERT_EQ(parsed.value().size(), 1u);

    const XrBlock& b = parsed.value()[0];
    EXPECT_EQ(b.type, XrBlockType::kStatisticsSummary);

    const XrStatisticsSummary& ps = b.statistics_summary;
    EXPECT_EQ(ps.ssrc, s.ssrc);
    EXPECT_EQ(ps.begin_seq, s.begin_seq);
    EXPECT_EQ(ps.end_seq, s.end_seq);
    EXPECT_TRUE(ps.has_loss);
    EXPECT_EQ(ps.lost_packets, s.lost_packets);
    EXPECT_TRUE(ps.has_duplicates);
    EXPECT_EQ(ps.dup_packets, s.dup_packets);
    EXPECT_TRUE(ps.has_jitter);
    EXPECT_EQ(ps.min_jitter, s.min_jitter);
    EXPECT_EQ(ps.max_jitter, s.max_jitter);
    EXPECT_EQ(ps.mean_jitter, s.mean_jitter);
    EXPECT_EQ(ps.dev_jitter, s.dev_jitter);
    EXPECT_EQ(ps.ttl_or_hop_limit_kind, 1);
    EXPECT_EQ(ps.min_ttl_or_hl, 64);
    EXPECT_EQ(ps.max_ttl_or_hl, 65);
    EXPECT_EQ(ps.mean_ttl_or_hl, 66);
    EXPECT_EQ(ps.dev_ttl_or_hl, 1);
}


// ---------------------------------------------------------------------------
// X4 — an unknown block type is skipped, not an error
// ---------------------------------------------------------------------------

TEST(RtcpXr, AnUnknownBlockTypeIsSkippedNotAnError) {
    // BT=200 is not in the registry: the block must be walked past by
    // its length, and the known block behind it must still be found.
    ByteBuffer body = make_raw_block(200, 0, 2);
    const ByteBuffer known = build_xr_receiver_reference_time(0x1122334455667788ull);
    body.insert(body.end(), known.begin(), known.end());

    const ByteBuffer packet = make_xr_packet(0x0A0B0C0D, body);
    auto parsed = parse_xr(span_of(packet));
    ASSERT_TRUE(parsed.ok());
    ASSERT_EQ(parsed.value().size(), 2u);

    EXPECT_EQ(parsed.value()[0].type, XrBlockType::kUnknown);
    EXPECT_EQ(parsed.value()[1].type, XrBlockType::kReceiverReferenceTime);
    EXPECT_EQ(parsed.value()[1].receiver_reference_time.ntp_timestamp,
              0x1122334455667788ull);
}


TEST(RtcpXr, ARecognisedButUnparsedBlockTypeIsReportedAsSuch) {
    // BT=7 is VoIP Metrics: it is in the registry, so the type is
    // known, but NimRTC does not parse it.
    const ByteBuffer body = make_raw_block(7, 0, 1);
    const ByteBuffer packet = make_xr_packet(1, body);
    auto parsed = parse_xr(span_of(packet));
    ASSERT_TRUE(parsed.ok());
    ASSERT_EQ(parsed.value().size(), 1u);
    EXPECT_EQ(parsed.value()[0].type, XrBlockType::kVoipMetrics);
}


// ---------------------------------------------------------------------------
// X5 / X6 — malformed input
// ---------------------------------------------------------------------------

TEST(RtcpXr, ATruncatedBlockIsAProtocolError) {
    // A Receiver Reference Time block that declares 3 words but only
    // carries 2 is malformed.
    ByteBuffer body(8, 0);
    body[0] = 4;
    body[2] = 2;   // block length = 3 words
    const ByteBuffer packet = make_xr_packet(1, body);

    auto parsed = parse_xr(span_of(packet));
    EXPECT_FALSE(parsed.ok());
}


TEST(RtcpXr, ABlockLongerThanThePacketIsAProtocolError) {
    ByteBuffer body(4, 0);
    body[0] = 4;
    body[2] = 40;   // claims 41 words
    const ByteBuffer packet = make_xr_packet(1, body);

    auto parsed = parse_xr(span_of(packet));
    EXPECT_FALSE(parsed.ok());
}


TEST(RtcpXr, ATruncatedPacketHeaderIsAProtocolError) {
    const std::uint8_t tiny[3] = {0x80, 207, 0};
    auto parsed = parse_xr(ByteSpan(tiny, sizeof(tiny)));
    EXPECT_FALSE(parsed.ok());
}


TEST(RtcpXr, AWrongPayloadTypeIsAProtocolError) {
    ByteBuffer packet = make_xr_packet(1, {});
    packet[1] = 206;   // PSFB, not XR
    auto parsed = parse_xr(span_of(packet));
    EXPECT_FALSE(parsed.ok());
}


TEST(RtcpXr, AWrongVersionIsAProtocolError) {
    ByteBuffer packet = make_xr_packet(1, {});
    packet[0] = 0x40;   // V=1
    auto parsed = parse_xr(span_of(packet));
    EXPECT_FALSE(parsed.ok());
}


// ---------------------------------------------------------------------------
// The XR packet builder
// ---------------------------------------------------------------------------

TEST(RtcpXr, BuildingAPacketStacksTheBlocks) {
    XrBlock rr;
    rr.type = XrBlockType::kReceiverReferenceTime;
    rr.receiver_reference_time.ntp_timestamp = 0x0102030405060708ull;

    XrBlock dlrr;
    dlrr.type = XrBlockType::kDlrr;
    XrDlrrSubReport sub;
    sub.ssrc = 0x99887766;
    dlrr.dlrr.sub_reports.push_back(sub);

    const ByteBuffer packet = build_xr(0x0BADF00D, {rr, dlrr});
    ASSERT_EQ(packet.size(), 8u + 12u + 16u);
    EXPECT_EQ(packet[0], 0x80);
    EXPECT_EQ(packet[1], 207);
    // The length field counts 32-bit words minus one.
    EXPECT_EQ(packet[2], 0);
    EXPECT_EQ(packet[3], static_cast<std::uint8_t>(packet.size() / 4u - 1u));

    auto parsed = parse_xr(span_of(packet));
    ASSERT_TRUE(parsed.ok());
    ASSERT_EQ(parsed.value().size(), 2u);
    EXPECT_EQ(parsed.value()[0].type, XrBlockType::kReceiverReferenceTime);
    EXPECT_EQ(parsed.value()[1].type, XrBlockType::kDlrr);
    ASSERT_EQ(parsed.value()[1].dlrr.sub_reports.size(), 1u);
    EXPECT_EQ(parsed.value()[1].dlrr.sub_reports[0].ssrc, 0x99887766u);
}


// ---------------------------------------------------------------------------
// X8 / X9 — TMMBR (RFC 5104 §4.2.1)
// ---------------------------------------------------------------------------

TEST(RtcpTmmbr, TmmbrRoundTrips) {
    std::vector<TmmbrEntry> entries;
    TmmbrEntry e;
    e.ssrc = 0x12345678;
    e.bitrate_bps = 500000;
    e.measured_overhead_bytes = 40;
    entries.push_back(e);

    const ByteBuffer packet = build_tmmbr(0xAABBCCDD, entries);
    ASSERT_EQ(packet.size(), 12u + 8u);
    EXPECT_EQ(packet[0] & 0x1Fu, 3);   // FMT = TMMBR
    EXPECT_EQ(packet[1], 205);         // PT = RTPFB

    auto parsed = parse_tmmbr(span_of(packet));
    ASSERT_TRUE(parsed.ok());
    ASSERT_EQ(parsed.value().size(), 1u);
    EXPECT_EQ(parsed.value()[0].ssrc, e.ssrc);
    // The encoding is `mantissa * 2^exp`, so the round-trip is exact
    // only for a value the mantissa can express.  500000 = 0x7A120;
    // the encoder picks the smallest exponent that fits 17 bits, so
    // the value is representable exactly.
    EXPECT_EQ(parsed.value()[0].bitrate_bps, e.bitrate_bps);
    EXPECT_EQ(parsed.value()[0].measured_overhead_bytes,
              e.measured_overhead_bytes);
}


TEST(RtcpTmmbr, TheExponentMantissaSplitSurvivesTheRoundTrip) {
    std::vector<TmmbrEntry> entries;
    TmmbrEntry e;
    e.ssrc = 7;
    // A value that needs the exponent: 2^20 is not representable with
    // a 17-bit mantissa at exponent 0, but it is at exponent 3.
    e.bitrate_bps = 1u << 20;
    entries.push_back(e);

    const ByteBuffer packet = build_tmmbr(1, entries);
    auto parsed = parse_tmmbr(span_of(packet));
    ASSERT_TRUE(parsed.ok());
    ASSERT_EQ(parsed.value().size(), 1u);
    EXPECT_EQ(parsed.value()[0].bitrate_bps, e.bitrate_bps);
}


TEST(RtcpTmmbr, SeveralEntriesRoundTrip) {
    std::vector<TmmbrEntry> entries;
    for (std::uint32_t i = 0; i < 3; ++i) {
        TmmbrEntry e;
        e.ssrc = 1000 + i;
        e.bitrate_bps = 100000 * (i + 1);
        entries.push_back(e);
    }

    const ByteBuffer packet = build_tmmbr(1, entries);
    // The length field is 2 + 2*N words (RFC 5104 §4.2.1.1).
    EXPECT_EQ(packet.size(), 12u + 3u * 8u);

    auto parsed = parse_tmmbr(span_of(packet));
    ASSERT_TRUE(parsed.ok());
    ASSERT_EQ(parsed.value().size(), 3u);
    for (std::uint32_t i = 0; i < 3; ++i) {
        EXPECT_EQ(parsed.value()[i].ssrc, 1000 + i);
        EXPECT_EQ(parsed.value()[i].bitrate_bps, 100000 * (i + 1));
    }
}


// ---------------------------------------------------------------------------
// X10 / X11 — the empty cases
// ---------------------------------------------------------------------------

TEST(RtcpTmmbr, AnEmptyTmmbnIsLegal) {
    const ByteBuffer packet = build_tmmbn(0x11223344, {});
    ASSERT_EQ(packet.size(), 12u);
    EXPECT_EQ(packet[0] & 0x1Fu, 4);   // FMT = TMMBN

    auto parsed = parse_tmmbn(span_of(packet));
    ASSERT_TRUE(parsed.ok());
    EXPECT_TRUE(parsed.value().empty());
}


TEST(RtcpTmmbr, AnEmptyTmmbrBuildsNothing) {
    // RFC 5101 §4.2.1.1: a TMMBR SHALL carry one or more FCI
    // entries, so an empty one is not built at all.
    EXPECT_TRUE(build_tmmbr(1, {}).empty());
}


// ---------------------------------------------------------------------------
// X12 — the wrong message is rejected
// ---------------------------------------------------------------------------

TEST(RtcpTmmbr, AWrongFmtIsRejected) {
    const ByteBuffer packet = build_tmmbn(1, {});
    auto parsed = parse_tmmbr(span_of(packet));
    EXPECT_FALSE(parsed.ok());
}


TEST(RtcpTmmbr, AWrongPayloadTypeIsRejected) {
    TmmbrEntry e;
    e.ssrc = 1;
    e.bitrate_bps = 1000;
    ByteBuffer packet = build_tmmbr(1, {e});
    packet[1] = 206;   // PSFB, not RTPFB
    auto parsed = parse_tmmbr(span_of(packet));
    EXPECT_FALSE(parsed.ok());
}


TEST(RtcpTmmbr, ATruncatedMessageIsRejected) {
    const std::uint8_t tiny[4] = {0x83, 205, 0, 0};
    auto parsed = parse_tmmbr(ByteSpan(tiny, sizeof(tiny)));
    EXPECT_FALSE(parsed.ok());
}


TEST(RtcpTmmbr, ATruncatedFciIsRejected) {
    // The length field claims 5 words (20 B) but only 16 B arrived.
    TmmbrEntry e;
    e.ssrc = 1;
    e.bitrate_bps = 1000;
    const ByteBuffer packet = build_tmmbr(1, {e});
    ASSERT_EQ(packet.size(), 20u);

    const ByteSpan truncated(packet.data(), 16);
    auto parsed = parse_tmmbr(truncated);
    EXPECT_FALSE(parsed.ok());
}
