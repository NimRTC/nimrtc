/**
 * @file tests/test_engine_xr_report.cpp
 * @brief v1.2.0 (session capability plan X1) — the engine's side of the
 *        RTCP Extended Report path.
 *
 * The XR parser has its own module tests
 * (`src/modules/rtp/tests/test_rtcp_xr.cpp`) and the seam adapter has
 * its own (`src/modules/rtp/tests/test_xr_report_plugin.cpp`).  What
 * neither can cover is the *wiring* between the engine and the seam:
 * that the engine's RTCP demux routes the XR sub-packets (payload type
 * 207) into the consumer, and that the host can read the decoded blocks
 * back.
 *
 * Covered:
 *   X-E1 - the engine creates the XR consumer at open() (the seam
 *          resolves through the registry's XR slot)
 *   X-E2 - an inbound XR packet reaches the consumer and the engine's
 *          routing counter moves
 *   X-E3 - the decoded Receiver Reference Time block (BT = 4) is
 *          readable by the host
 *   X-E4 - the routing table: a Receiver Report is *not* an XR packet
 *          and never reaches the consumer
 *   X-E5 - a malformed XR packet is counted, not fatal
 *   X-E6 - the consumer can be switched off (empty name)
 *
 * The suite uses the "shell" transport strategy (the same sentinel
 * `test_engine_net_feedback` uses) so `open()` reaches the open state
 * without binding ICE UDP sockets.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/xr_report.hpp>

namespace {

using nimrtc::core::ByteBuffer;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
using nimrtc::plugins::XrBlockType;

void put_be32(ByteBuffer& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void put_be16(ByteBuffer& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void put_u8(ByteBuffer& out, std::uint8_t v) { out.push_back(v); }

/// Build an XR packet (PT 207) carrying one Receiver Reference Time
/// block (BT = 4).  RFC 3611 §2 / §4.4.
ByteBuffer make_xr_rrtr(std::uint32_t ssrc, std::uint64_t ntp) {
    ByteBuffer body;
    put_be32(body, ssrc);                 // originator SSRC
    put_u8(body, 4u);                     // BT = Receiver Reference Time
    put_u8(body, 0u);                     // type-specific
    put_be16(body, 2u);                   // block length = 2 words
    put_be32(body, static_cast<std::uint32_t>(ntp >> 32));
    put_be32(body, static_cast<std::uint32_t>(ntp & 0xFFFFFFFFull));

    ByteBuffer out;
    put_u8(out, 0x80u);                   // V=2, no padding
    put_u8(out, 207u);                   // PT = XR
    put_be16(out, static_cast<std::uint16_t>(out.size() / 4u + body.size() / 4u));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

/// Build a Receiver Report (PT 201) — not an XR packet.
ByteBuffer make_rr(std::uint32_t ssrc) {
    ByteBuffer body;
    put_be32(body, 0xDEADBEEFu);          // sender SSRC
    put_be32(body, ssrc);                 // reported SSRC
    put_u8(body, 0);                      // fraction lost
    put_u8(body, 0);
    put_u8(body, 0);
    put_be32(body, 0);                    // highest seq
    put_be32(body, 0);                    // jitter
    put_be32(body, 0);                    // LSR
    put_be32(body, 0);                    // DLSR

    ByteBuffer out;
    put_u8(out, 0x81u);                   // V=2, RC=1
    put_u8(out, 201u);
    put_be16(out, static_cast<std::uint16_t>(out.size() / 4u + body.size() / 4u));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

class EngineXrReport : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
    }

    /// The "shell" transport strategy reaches the open state without
    /// touching the network (see test_engine_net_feedback.cpp).
    static EngineConfig make_test_config() {
        EngineConfig cfg;
        cfg.stun_server_host        = "";
        cfg.local_port_range_begin  = 52800;
        cfg.local_port_range_end    = 52900;
        cfg.transport_strategy_name = "shell";
        return cfg;
    }
};

// ---------------------------------------------------------------------------
// X-E1 — the seam resolves
// ---------------------------------------------------------------------------

TEST_F(EngineXrReport, TheConsumerIsCreatedAtOpen) {
    NimRTCEngine engine(make_test_config());
    ASSERT_EQ(engine.open(), 0u);

    // The consumer comes from the registry's XR slot; the engine
    // reaches it through the seam, never through a module header.
    ASSERT_NE(engine.xr_report(), nullptr);
    EXPECT_STREQ(engine.xr_report()->name(),
                 "nimrtc::rtp::XrReportAdapter (RFC 3611 RTCP XR)");
}

// ---------------------------------------------------------------------------
// X-E2 / X-E3 — the routing reaches the consumer
// ---------------------------------------------------------------------------

TEST_F(EngineXrReport, AnInboundXrPacketReachesTheConsumer) {
    NimRTCEngine engine(make_test_config());
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_NE(engine.xr_report(), nullptr);

    const ByteBuffer xr = make_xr_rrtr(0x11223344u, 0x0102030405060708ull);
    engine.feed_media_inbound(xr.data(), xr.size());

    // The block was routed to the consumer straight away (the routing
    // happens on the receive path, not in the tick).
    EXPECT_EQ(engine.stats().xr_report_blocks, 1u);

    const auto snap = engine.xr_report()->snapshot();
    EXPECT_TRUE(snap.has_data);
    EXPECT_EQ(snap.packets_received, 1u);
    EXPECT_EQ(snap.packets_malformed, 0u);
    EXPECT_EQ(snap.last_ssrc, 0x11223344u);

    ASSERT_EQ(snap.last_packet.blocks.size(), 1u);
    EXPECT_EQ(snap.last_packet.blocks[0].type,
              XrBlockType::kReceiverReferenceTime);
    EXPECT_EQ(snap.last_packet.blocks[0].receiver_reference_time.ntp_timestamp,
              0x0102030405060708ull);
}

// ---------------------------------------------------------------------------
// X-E4 — the routing table
// ---------------------------------------------------------------------------

TEST_F(EngineXrReport, AReceiverReportDoesNotReachTheConsumer) {
    NimRTCEngine engine(make_test_config());
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_NE(engine.xr_report(), nullptr);

    const ByteBuffer rr = make_rr(0x1u);
    engine.feed_media_inbound(rr.data(), rr.size());

    EXPECT_EQ(engine.stats().xr_report_blocks, 0u);
    EXPECT_FALSE(engine.xr_report()->snapshot().has_data);
}

// ---------------------------------------------------------------------------
// X-E5 — a malformed report is counted, not fatal
// ---------------------------------------------------------------------------

TEST_F(EngineXrReport, AMalformedXrPacketIsCountedNotFatal) {
    NimRTCEngine engine(make_test_config());
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_NE(engine.xr_report(), nullptr);

    // PT 207 with a self-consistent packet length, but the block inside
    // claims more words than the packet carries: the demultiplexer
    // accepts the packet and the *block* parse is what fails.
    ByteBuffer broken;
    put_u8(broken, 0x80u);
    put_u8(broken, 207u);
    put_be16(broken, 2u);              // 3 words total = 12 bytes
    put_be32(broken, 0x11223344u);     // originator SSRC
    put_u8(broken, 4u);   // BT = Receiver Reference Time ...
    put_u8(broken, 0u);
    put_be16(broken, 2u); // ... claiming a 12-byte block that is not there

    engine.feed_media_inbound(broken.data(), broken.size());

    EXPECT_EQ(engine.stats().xr_report_blocks, 1u);
    const auto snap = engine.xr_report()->snapshot();
    EXPECT_FALSE(snap.has_data);
    EXPECT_EQ(snap.packets_malformed, 1u);
}

// ---------------------------------------------------------------------------
// X-E6 — the consumer can be switched off
// ---------------------------------------------------------------------------

TEST_F(EngineXrReport, TheConsumerCanBeSwitchedOff) {
    EngineConfig cfg = make_test_config();
    cfg.xr_report_name = "";

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    EXPECT_EQ(engine.xr_report(), nullptr);

    // An XR packet with no consumer is simply dropped — the pre-v1.2.0
    // behaviour.
    const ByteBuffer xr = make_xr_rrtr(0x11223344u, 0x0102030405060708ull);
    engine.feed_media_inbound(xr.data(), xr.size());
    EXPECT_EQ(engine.stats().xr_report_blocks, 0u);
}

}  // namespace
