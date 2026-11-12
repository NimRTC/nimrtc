/**
 * @file tests/test_xr_report_plugin.cpp
 * @brief Tests for the RTCP Extended Report seam adapter.
 *
 * Covers (session capability plan X1, the "P" half — the plugin seam
 * the engine consumes):
 *
 *   - the built-in consumer registers under "nimrtc_xr" and the engine
 *     can resolve it through the registry's XR slot;
 *   - a well-formed XR packet is decoded into the seam's neutral value
 *     types (BT 4 / BT 5 / BT 6);
 *   - a sub-packet whose payload type is not 207 is ignored, not
 *     mis-parsed;
 *   - a malformed XR packet is counted, not thrown;
 *   - `reset()` clears the state.
 *
 * The wire-level parsing itself is covered by `test_rtcp_xr.cpp`; this
 * file is about the *seam*: that the adapter translates the module's
 * types into the plugin's types faithfully.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/core/time.hpp>
#include <nimrtc/plugins/xr_report.hpp>
#include <nimrtc/rtp/rtcp_xr.hpp>
#include <nimrtc/rtp/xr_report_plugin.hpp>

using nimrtc::core::ByteBuffer;
using nimrtc::core::ByteSpan;

// The module and the seam both define `XrBlock` / `XrBlockType` /
// `XrStatisticsSummary` — same wire format, distinct types on purpose
// (the seam may not depend on a concrete module).  The tests therefore
// qualify the two namespaces explicitly rather than importing both.
namespace rtp   = nimrtc::rtp;
namespace plug  = nimrtc::plugins;

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
    const std::uint16_t words =
        static_cast<std::uint16_t>(out.size() / 4u - 1u);
    out[2] = static_cast<std::uint8_t>(words >> 8);
    out[3] = static_cast<std::uint8_t>(words & 0xFF);
    return out;
}

/** The Receiver Reference Time block (BT = 4) with the given NTP stamp. */
ByteBuffer make_rrtr_block(std::uint64_t ntp) {
    ByteBuffer out(12, 0);
    out[0] = 4;    // BT = Receiver Reference Time
    out[1] = 0;    // type-specific
    out[2] = 0;    // block length = 2 (words minus one)
    out[3] = 2;
    // `i` is signed, so `4 + i` is an int and indexing a vector with
    // it trips Clang's -Wsign-conversion.  Keep the index unsigned.
    for (std::size_t i = 0; i < 8; ++i) {
        out[4 + i] = static_cast<std::uint8_t>(ntp >> (56 - 8 * i));
    }
    return out;
}

/** A DLRR block (BT = 5) with one sub-report. */
ByteBuffer make_dlrr_block(std::uint32_t ssrc, std::uint32_t lrr,
                           std::uint32_t dlrr) {
    ByteBuffer out(16, 0);
    out[0] = 5;    // BT = DLRR
    out[1] = 0;
    out[2] = 0;    // block length = 3
    out[3] = 3;
    out[4] = static_cast<std::uint8_t>(ssrc >> 24);
    out[5] = static_cast<std::uint8_t>(ssrc >> 16);
    out[6] = static_cast<std::uint8_t>(ssrc >> 8);
    out[7] = static_cast<std::uint8_t>(ssrc);
    out[8]  = static_cast<std::uint8_t>(lrr >> 24);
    out[9]  = static_cast<std::uint8_t>(lrr >> 16);
    out[10] = static_cast<std::uint8_t>(lrr >> 8);
    out[11] = static_cast<std::uint8_t>(lrr);
    out[12] = static_cast<std::uint8_t>(dlrr >> 24);
    out[13] = static_cast<std::uint8_t>(dlrr >> 16);
    out[14] = static_cast<std::uint8_t>(dlrr >> 8);
    out[15] = static_cast<std::uint8_t>(dlrr);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

TEST(XrReportPlugin, TheBuiltInConsumerRegistersUnderItsId) {
    // The RTP module's registration entry point publishes the XR
    // consumer into the registry's XR slot.
    nimrtc::rtp::register_default_plugins();

    const plug::IXrReportFactory* factory =
        nimrtc::core::PluginRegistry::instance().get_xr_report("nimrtc_xr");
    ASSERT_NE(factory, nullptr);
    EXPECT_EQ(factory->id(), "nimrtc_xr");
}

TEST(XrReportPlugin, TheRegistryListsTheConsumer) {
    nimrtc::rtp::register_default_plugins();
    const auto ids = nimrtc::core::PluginRegistry::instance().list_xr_reports();
    EXPECT_NE(std::find(ids.begin(), ids.end(), std::string_view{"nimrtc_xr"}),
              ids.end());
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

TEST(XrReportPlugin, AReceiverReferenceTimeBlockIsDecoded) {
    rtp::XrReportAdapter adapter;
    const ByteBuffer pkt = make_xr_packet(0x11223344u,
                                          make_rrtr_block(0x0102030405060708ull));

    adapter.on_rtcp_block(207, ByteSpan(pkt.data(), pkt.size()),
                          nimrtc::core::SteadyClock::now());

    const plug::XrReportSnapshot snap = adapter.snapshot();
    ASSERT_TRUE(snap.has_data);
    EXPECT_EQ(snap.packets_received, 1u);
    EXPECT_EQ(snap.packets_malformed, 0u);
    EXPECT_EQ(snap.last_ssrc, 0x11223344u);
    ASSERT_EQ(snap.last_packet.blocks.size(), 1u);

    const plug::XrBlock& blk = snap.last_packet.blocks[0];
    EXPECT_EQ(blk.type, plug::XrBlockType::kReceiverReferenceTime);
    EXPECT_EQ(blk.receiver_reference_time.ntp_timestamp, 0x0102030405060708ull);
}

TEST(XrReportPlugin, ADlrrBlockIsDecoded) {
    rtp::XrReportAdapter adapter;
    const ByteBuffer pkt = make_xr_packet(0xAABBCCDDu,
                                          make_dlrr_block(0x11223344u,
                                                          0x55667788u, 0x00010000u));

    adapter.on_rtcp_block(207, ByteSpan(pkt.data(), pkt.size()),
                          nimrtc::core::SteadyClock::now());

    const plug::XrReportSnapshot snap = adapter.snapshot();
    ASSERT_EQ(snap.last_packet.blocks.size(), 1u);

    const plug::XrBlock& blk = snap.last_packet.blocks[0];
    EXPECT_EQ(blk.type, plug::XrBlockType::kDlrr);
    ASSERT_EQ(blk.dlrr.sub_reports.size(), 1u);
    EXPECT_EQ(blk.dlrr.sub_reports[0].ssrc, 0x11223344u);
    EXPECT_EQ(blk.dlrr.sub_reports[0].last_rr, 0x55667788u);
    EXPECT_EQ(blk.dlrr.sub_reports[0].dlrr, 0x00010000u);
}

TEST(XrReportPlugin, AStatisticsSummaryBlockIsDecoded) {
    // Build the block through the module's own builder so the wire
    // layout is the module's, not a hand-rolled guess.
    rtp::XrStatisticsSummary summary;
    summary.ssrc                = 0x0A0B0C0Du;
    summary.begin_seq           = 100;
    summary.end_seq             = 200;
    summary.has_loss            = true;
    summary.lost_packets        = 7;
    summary.has_jitter          = true;
    summary.min_jitter          = 10;
    summary.max_jitter          = 90;
    summary.mean_jitter         = 50;
    summary.dev_jitter          = 5;

    const ByteBuffer block = rtp::build_xr_statistics_summary(summary);
    const ByteBuffer pkt   = make_xr_packet(0x0A0B0C0Du, block);

    rtp::XrReportAdapter adapter;
    adapter.on_rtcp_block(207, ByteSpan(pkt.data(), pkt.size()),
                          nimrtc::core::SteadyClock::now());

    const plug::XrReportSnapshot snap = adapter.snapshot();
    ASSERT_EQ(snap.last_packet.blocks.size(), 1u);

    const plug::XrBlock& blk = snap.last_packet.blocks[0];
    EXPECT_EQ(blk.type, plug::XrBlockType::kStatisticsSummary);
    EXPECT_EQ(blk.statistics_summary.ssrc, 0x0A0B0C0Du);
    EXPECT_EQ(blk.statistics_summary.begin_seq, 100);
    EXPECT_EQ(blk.statistics_summary.end_seq, 200);
    EXPECT_TRUE(blk.statistics_summary.has_loss);
    EXPECT_EQ(blk.statistics_summary.lost_packets, 7u);
    EXPECT_TRUE(blk.statistics_summary.has_jitter);
    EXPECT_EQ(blk.statistics_summary.max_jitter, 90u);
}

TEST(XrReportPlugin, AnUnknownBlockTypeIsSkippedNotRejected) {
    // BT = 1 (Loss RLE) is a registered block this build recognises but
    // does not parse.  It must come through as kUnknown, not as an
    // error.
    ByteBuffer raw_block(8, 0);
    raw_block[0] = 1;    // BT = Loss RLE
    raw_block[2] = 0;    // block length = 1
    raw_block[3] = 1;

    const ByteBuffer pkt = make_xr_packet(0x1u, raw_block);

    rtp::XrReportAdapter adapter;
    adapter.on_rtcp_block(207, ByteSpan(pkt.data(), pkt.size()),
                          nimrtc::core::SteadyClock::now());

    const plug::XrReportSnapshot snap = adapter.snapshot();
    EXPECT_TRUE(snap.has_data);
    EXPECT_EQ(snap.packets_malformed, 0u);
    ASSERT_EQ(snap.last_packet.blocks.size(), 1u);
    EXPECT_EQ(snap.last_packet.blocks[0].type, plug::XrBlockType::kLossRle);
}

// ---------------------------------------------------------------------------
// Non-XR traffic
// ---------------------------------------------------------------------------

TEST(XrReportPlugin, AnotherPayloadTypeIsIgnored) {
    rtp::XrReportAdapter adapter;
    const ByteBuffer pkt = make_xr_packet(0x1u, make_rrtr_block(0x42ull));

    // PT 201 is a Receiver Report: not this seam's business.
    adapter.on_rtcp_block(201, ByteSpan(pkt.data(), pkt.size()),
                          nimrtc::core::SteadyClock::now());

    const plug::XrReportSnapshot snap = adapter.snapshot();
    EXPECT_FALSE(snap.has_data);
    EXPECT_EQ(snap.packets_received, 0u);
    EXPECT_EQ(snap.packets_malformed, 0u);
}

// ---------------------------------------------------------------------------
// Malformed input
// ---------------------------------------------------------------------------

TEST(XrReportPlugin, AMalformedPacketIsCountedNotThrown) {
    rtp::XrReportAdapter adapter;

    // A truncated XR packet: the header says XR but the block is cut.
    const ByteBuffer truncated{0x80, 0xCF, 0x00, 0x05, 0, 0, 0, 1, 4, 0};
    adapter.on_rtcp_block(207, ByteSpan(truncated.data(), truncated.size()),
                          nimrtc::core::SteadyClock::now());

    const plug::XrReportSnapshot snap = adapter.snapshot();
    EXPECT_FALSE(snap.has_data);
    EXPECT_EQ(snap.packets_received, 0u);
    EXPECT_EQ(snap.packets_malformed, 1u);
}

TEST(XrReportPacket, AWrongPayloadTypeIsAProtocolError) {
    // The module's own parser is strict about the payload type — the
    // adapter's job is to route, not to second-guess.
    const ByteBuffer not_xr{0x80, 0xC9, 0x00, 0x01, 0, 0, 0, 0};
    const auto res = rtp::parse_xr_packet(ByteSpan(not_xr.data(), not_xr.size()));
    EXPECT_FALSE(res);
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

TEST(XrReportPlugin, ResetClearsTheState) {
    rtp::XrReportAdapter adapter;
    const ByteBuffer pkt = make_xr_packet(0x1u, make_rrtr_block(0x42ull));

    adapter.on_rtcp_block(207, ByteSpan(pkt.data(), pkt.size()),
                          nimrtc::core::SteadyClock::now());
    ASSERT_TRUE(adapter.snapshot().has_data);

    adapter.reset();

    const plug::XrReportSnapshot snap = adapter.snapshot();
    EXPECT_FALSE(snap.has_data);
    EXPECT_EQ(snap.packets_received, 0u);
    EXPECT_EQ(snap.packets_malformed, 0u);
    EXPECT_TRUE(snap.last_packet.blocks.empty());
}
