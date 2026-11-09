/**
 * @file tests/test_engine_net_feedback.cpp
 * @brief v1.2.0 M1 (F1 / F6) 鈥?the engine's side of the bandwidth loop.
 *
 * The collector module has its own tests
 * (`src/modules/net_feedback/tests/test_net_feedback.cpp`), and the
 * estimator has its own (`src/modules/bwe/tests/test_bwe.cpp`).  What
 * neither can cover is the *wiring* between them, and that wiring is
 * exactly what was missing before v1.2.0: the engine read
 * `IBwe::estimate()` in its tick but nothing ever called
 * `IBwe::on_feedback()`, so the bandwidth target stayed pinned at the
 * initial bitrate forever and every rate-control feature built on top
 * of it was decorative.
 *
 * This suite therefore exercises the production path end to end:
 *
 *   inbound RTCP 鈫?engine demux 鈫?collector 鈫?snapshot 鈫?IBwe::on_feedback()
 *
 * The packets are pushed through `feed_media_inbound()`, the same
 * RTCP-demux 鈫?dispatch tail `on_transport_recv()` uses, so a test
 * cannot pass against a parallel implementation that has drifted from
 * the production one.
 *
 * Covered:
 *   E1  - the engine creates the collector at open() (the seam resolves)
 *   E2  - an inbound Receiver Report reaches the collector and the
 *         engine's feedback counters move
 *   E3  - **the BWE estimate moves**: a lossy Receiver Report lowers
 *         the bandwidth target (the F6 acceptance criterion)
 *   E4  - the routing table: a Sender Report is *not* a receiver-side
 *         network signal and never reaches the collector
 *   E5  - a REMB reaches the collector and is visible in the snapshot
 *
 * The suite uses the "shell" transport strategy (the same sentinel
 * `test_engine_timeline` uses) so `open()` reaches the open state
 * without binding ICE UDP sockets.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/net_feedback.hpp>

namespace {

using nimrtc::core::ByteBuffer;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
using nimrtc::plugins::NetFeedbackSource;

void put_be32(ByteBuffer& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void put_be24(ByteBuffer& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
}

void put_be16(ByteBuffer& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void put_u8(ByteBuffer& out, std::uint8_t v) { out.push_back(v); }

/// One report block of a Receiver Report (RFC 3550 搂6.4.3).
struct ReportBlockFields {
    std::uint32_t ssrc = 0;
    std::uint8_t  fraction_lost = 0;
    std::int32_t  cumulative_lost = 0;
    std::uint32_t highest_seq = 0;
    std::uint32_t jitter = 0;
    std::uint32_t lsr = 0;
    std::uint32_t dlsr = 0;
};

/// Build a Receiver Report (PT 201) with one report block.
ByteBuffer make_rr(const ReportBlockFields& rb) {
    ByteBuffer body;
    put_be32(body, 0xDEADBEEFu);        // SSRC of the report sender
    put_be32(body, rb.ssrc);
    put_u8(body, rb.fraction_lost);
    put_be24(body, static_cast<std::uint32_t>(rb.cumulative_lost) & 0xFFFFFFu);
    put_be32(body, rb.highest_seq);
    put_be32(body, rb.jitter);
    put_be32(body, rb.lsr);
    put_be32(body, rb.dlsr);

    ByteBuffer out;
    put_u8(out, 0x81u);                  // V=2, RC=1
    put_u8(out, 201u);
    put_be16(out, static_cast<std::uint16_t>(out.size() / 4u + body.size() / 4u));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

/// Build a REMB message (PSFB, FMT 15).
ByteBuffer make_remb(std::uint64_t bitrate_bps) {
    constexpr std::uint32_t kMaxMantissa = 0x3FFFF;
    std::uint64_t mantissa = bitrate_bps;
    std::uint8_t exponent = 0;
    while (mantissa > kMaxMantissa) {
        mantissa >>= 1;
        ++exponent;
    }

    ByteBuffer body;
    put_be32(body, 0x11223344u);   // sender SSRC
    put_be32(body, 0);             // media SSRC
    put_be32(body, 0x52454D42u);   // 'R' 'E' 'M' 'B'
    put_u8(body, 0);              // no SSRC list
    put_u8(body, static_cast<std::uint8_t>((exponent << 2) | (mantissa >> 16)));
    put_be16(body, static_cast<std::uint16_t>(mantissa & 0xFFFF));

    ByteBuffer out;
    put_u8(out, static_cast<std::uint8_t>(0x80u | 15u));
    put_u8(out, 206u);
    put_be16(out, static_cast<std::uint16_t>(out.size() / 4u + body.size() / 4u));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

/// Build a Sender Report (PT 200) 鈥?the timeline's anchor, *not* a
/// receiver-side network signal.
ByteBuffer make_sr(std::uint32_t ssrc, std::uint32_t ntp_mid32,
                   std::uint32_t rtp_ts) {
    ByteBuffer body;
    put_be32(body, ssrc);
    put_be32(body, ntp_mid32);       // NTP timestamp (high + mid)
    put_be32(body, 0);               // NTP timestamp (low + reference)
    put_be32(body, rtp_ts);
    put_be32(body, 1);               // packet count
    put_be32(body, 0);               // octet count

    ByteBuffer out;
    put_u8(out, 0x80u);              // V=2, RC=0
    put_u8(out, 200u);
    put_be16(out, static_cast<std::uint16_t>(out.size() / 4u + body.size() / 4u));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

class EngineNetFeedback : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
    }

    /// The "shell" transport strategy reaches the open state without
    /// touching the network (see test_engine_timeline.cpp).
    static EngineConfig make_test_config() {
        EngineConfig cfg;
        cfg.stun_server_host       = "";
        cfg.local_port_range_begin = 52600;
        cfg.local_port_range_end   = 52700;
        cfg.transport_strategy_name = "shell";

        // A deterministic estimator: no smoothing lag, so one loss event
        // is visible in the estimate immediately, and the halving is
        // exact.
        cfg.bwe_config.initial_bitrate_bps = 1'000'000;
        cfg.bwe_config.smoothing_alpha    = 1.0;
        cfg.bwe_config.decrease_factor    = 0.5;
        return cfg;
    }
};

// ---------------------------------------------------------------------------
// E1 鈥?the seam resolves
// ---------------------------------------------------------------------------

TEST_F(EngineNetFeedback, TheCollectorIsCreatedAtOpen) {
    NimRTCEngine engine(make_test_config());
    ASSERT_EQ(engine.open(), 0u);

    // The collector comes from the registry's net-feedback slot; the
    // engine reaches it through the seam, never through a module header.
    ASSERT_NE(engine.net_feedback(), nullptr);
    EXPECT_STREQ(engine.net_feedback()->name(), "nimrtc_net_feedback");
}

// ---------------------------------------------------------------------------
// E2 鈥?the routing table reaches the collector
// ---------------------------------------------------------------------------

TEST_F(EngineNetFeedback, AnInboundReceiverReportReachesTheCollector) {
    NimRTCEngine engine(make_test_config());
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_NE(engine.net_feedback(), nullptr);

    // 64 / 256 = 25 % loss.
    ByteBuffer rr = make_rr({.fraction_lost = 64});
    engine.feed_media_inbound(rr.data(), rr.size());

    // The block was routed to the collector straight away (the routing
    // happens on the receive path, not in the tick).
    EXPECT_EQ(engine.stats().net_feedback_blocks, 1u);

    const auto snap = engine.net_feedback()->snapshot();
    EXPECT_TRUE(snap.has_loss_rtt);
    EXPECT_DOUBLE_EQ(snap.loss_rate, 0.25);
    EXPECT_EQ(snap.source, NetFeedbackSource::kLossRtt);
}

// ---------------------------------------------------------------------------
// E3 鈥?the bandwidth estimate moves (F6)
// ---------------------------------------------------------------------------

TEST_F(EngineNetFeedback, TheBandwidthEstimateMovesUnderLoss) {
    NimRTCEngine engine(make_test_config());
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_NE(engine.bwe(), nullptr);

    const std::uint32_t before = engine.bwe()->estimate().target_bitrate_bps;
    ASSERT_EQ(before, 1'000'000u);

    ByteBuffer rr = make_rr({.fraction_lost = 64});   // 25 % loss
    engine.feed_media_inbound(rr.data(), rr.size());

    // The tick is what hands the aggregated snapshot to the estimator.
    engine.tick();

    EXPECT_EQ(engine.stats().bwe_feedback_samples, 1u);

    const std::uint32_t after = engine.bwe()->estimate().target_bitrate_bps;
    EXPECT_LT(after, before);
    // With smoothing_alpha = 1.0 the halved value is exact.
    EXPECT_EQ(after, 500'000u);
    EXPECT_EQ(engine.stats().bandwidth_estimate_bps, after);
}

// ---------------------------------------------------------------------------
// E4 鈥?the routing table: a Sender Report is not a network signal
// ---------------------------------------------------------------------------

TEST_F(EngineNetFeedback, ASenderReportDoesNotReachTheCollector) {
    NimRTCEngine engine(make_test_config());
    ASSERT_EQ(engine.open(), 0u);

    ByteBuffer sr = make_sr(0x12345678u, 0xAABBCCDDu, 12345u);
    engine.feed_media_inbound(sr.data(), sr.size());
    engine.tick();

    // The Sender Report belongs to the cross-end timeline, not to the
    // bandwidth collector.
    EXPECT_EQ(engine.stats().net_feedback_blocks, 0u);
    EXPECT_EQ(engine.stats().bwe_feedback_samples, 0u);
    EXPECT_FALSE(engine.net_feedback()->snapshot().has_loss_rtt);
}

// ---------------------------------------------------------------------------
// E5 鈥?REMB reaches the collector
// ---------------------------------------------------------------------------

TEST_F(EngineNetFeedback, ARembReachesTheCollector) {
    NimRTCEngine engine(make_test_config());
    ASSERT_EQ(engine.open(), 0u);

    ByteBuffer remb = make_remb(500'000);
    engine.feed_media_inbound(remb.data(), remb.size());
    engine.tick();

    const auto snap = engine.net_feedback()->snapshot();
    ASSERT_TRUE(snap.remb_bps.has_value());
    EXPECT_EQ(*snap.remb_bps, 500'000u);
    EXPECT_EQ(snap.source, NetFeedbackSource::kRemb);
    EXPECT_EQ(engine.stats().bwe_feedback_samples, 1u);
}

} // namespace
