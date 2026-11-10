/**
 * @file tests/test_engine_net_feedback.cpp
 * @brief v1.2.0 M1 (F1 / F6) — the engine's side of the bandwidth loop.
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
 *   inbound RTCP → engine demux → collector → snapshot → IBwe::on_feedback()
 *
 * The packets are pushed through `feed_media_inbound()`, the same
 * RTCP-demux → dispatch tail `on_transport_recv()` uses, so a test
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

/// One report block of a Receiver Report (RFC 3550 §6.4.3).
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

/// Build a Sender Report (PT 200) — the timeline's anchor, *not* a
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
// E1 — the seam resolves
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
// E2 — the routing table reaches the collector
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
// E3 — the bandwidth estimate moves (F6)
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
// E4 — the routing table: a Sender Report is not a network signal
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
// E5 — REMB reaches the collector
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

// ---------------------------------------------------------------------------
// E6 - the transport-cc negotiation (ADR-018 Decisions 2 and 6)
// ---------------------------------------------------------------------------

/// Build a transport-cc feedback (RTPFB, FMT 15) with one run-length
/// chunk of "received with a small delta" packets.
ByteBuffer make_transport_cc(std::uint16_t base_seq, std::uint16_t count,
                             const std::vector<std::uint8_t>& deltas) {
    ByteBuffer body;
    put_be16(body, static_cast<std::uint16_t>(
                       (static_cast<std::uint16_t>(1u & 0x3u) << 13) | (count & 0x1FFFu)));
    for (std::uint8_t d : deltas) {
        put_u8(body, d);
    }

    const std::size_t total = 20u + body.size();
    const std::size_t pad = (4u - (total % 4u)) % 4u;
    const std::size_t padded = total + pad;

    ByteBuffer out(padded, 0);
    out[0] = static_cast<std::uint8_t>(0x80u | (pad ? 0x20u : 0x00u) | 15u);
    out[1] = 205u;
    const std::uint16_t length_words = static_cast<std::uint16_t>(padded / 4u - 1u);
    out[2] = static_cast<std::uint8_t>(length_words >> 8);
    out[3] = static_cast<std::uint8_t>(length_words & 0xFFu);
    out[12] = static_cast<std::uint8_t>(base_seq >> 8);
    out[13] = static_cast<std::uint8_t>(base_seq & 0xFFu);
    out[14] = static_cast<std::uint8_t>(count >> 8);
    out[15] = static_cast<std::uint8_t>(count & 0xFFu);
    for (std::size_t i = 0; i < body.size(); ++i) {
        out[20u + i] = body[i];
    }
    if (pad) {
        out[padded - 1u] = static_cast<std::uint8_t>(pad);
    }
    return out;
}

/// Drop every SDP line that mentions `token`.
std::string strip_sdp_lines(const std::string& sdp, std::string_view token) {
    std::string out;
    std::size_t pos = 0;
    while (pos <= sdp.size()) {
        const std::size_t eol = sdp.find('\n', pos);
        const std::size_t end = (eol == std::string::npos) ? sdp.size() : eol;
        const std::string line = sdp.substr(pos, end - pos);
        if (line.find(token) == std::string::npos) {
            out += line;
            out += "\r\n";
        }
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return out;
}

TEST_F(EngineNetFeedback, TheOfferAdvertisesTransportCc) {
    NimRTCEngine engine(make_test_config());
    ASSERT_EQ(engine.open(), 0u);

    const std::string offer = engine.create_offer();
    ASSERT_FALSE(offer.empty());

    EXPECT_NE(offer.find("transport-cc"), std::string::npos)
        << "the offer MUST advertise a=rtcp-fb:<pt> transport-cc: a peer "
           "sends transport-cc feedback only when the negotiated "
           "description carries the attribute (ADR-018 Decision 6)";
}

TEST_F(EngineNetFeedback, TheLossRttPolicyDoesNotAdvertiseTransportCc) {
    EngineConfig cfg = make_test_config();
    cfg.net_feedback_signal = "loss_rtt";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::string offer = engine.create_offer();
    ASSERT_FALSE(offer.empty());
    EXPECT_EQ(offer.find("transport-cc"), std::string::npos)
        << "a host that pinned the loss/RTT path must not negotiate a "
           "signal it will not use";
}

TEST_F(EngineNetFeedback, TheAnswerEchoesTransportCcOnlyWhenThePeerOfferedIt) {
    // The peer offers transport-cc: the answer has to echo it, or the
    // peer will not send the feedback (RFC 3264 §6.1).
    {
        NimRTCEngine peer(make_test_config());
        ASSERT_EQ(peer.open(), 0u);
        const std::string peer_offer = peer.create_offer();
        ASSERT_NE(peer_offer.find("transport-cc"), std::string::npos);

        NimRTCEngine engine(make_test_config());
        ASSERT_EQ(engine.open(), 0u);
        const auto answer = engine.process_remote_sdp(peer_offer);
        ASSERT_TRUE(answer.has_value());
        ASSERT_FALSE(answer->empty());
        EXPECT_NE(answer->find("transport-cc"), std::string::npos);
    }

    // The peer does not offer transport-cc: the answer must not invent
    // it (the peer only sends what was negotiated).
    {
        NimRTCEngine peer(make_test_config());
        ASSERT_EQ(peer.open(), 0u);
        const std::string peer_offer =
            strip_sdp_lines(peer.create_offer(), "transport-cc");
        ASSERT_EQ(peer_offer.find("transport-cc"), std::string::npos);

        NimRTCEngine engine(make_test_config());
        ASSERT_EQ(engine.open(), 0u);
        const auto answer = engine.process_remote_sdp(peer_offer);
        ASSERT_TRUE(answer.has_value());
        EXPECT_EQ(answer->find("transport-cc"), std::string::npos);
    }
}

TEST_F(EngineNetFeedback, ThePeerCapabilityReachesTheCollector) {
    NimRTCEngine peer(make_test_config());
    ASSERT_EQ(peer.open(), 0u);
    const std::string peer_offer = peer.create_offer();

    NimRTCEngine engine(make_test_config());
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_NE(engine.net_feedback(), nullptr);

    // Before the negotiation the collector assumes the peer advertised
    // nothing, so the transport-cc path stays off.
    EXPECT_FALSE(engine.net_feedback()->peer_capabilities().transport_cc);

    ASSERT_TRUE(engine.process_remote_sdp(peer_offer).has_value());
    EXPECT_TRUE(engine.net_feedback()->peer_capabilities().transport_cc)
        << "the a=rtcp-fb facts of the remote description are the only "
           "authority on whether the peer will send transport-cc";
}

TEST_F(EngineNetFeedback, ANegotiatedTransportCcPathDrivesTheEstimate) {
    NimRTCEngine peer(make_test_config());
    ASSERT_EQ(peer.open(), 0u);
    const std::string peer_offer = peer.create_offer();

    NimRTCEngine engine(make_test_config());
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_TRUE(engine.process_remote_sdp(peer_offer).has_value());

    // The peer now sends transport-cc feedback.
    ByteBuffer cc = make_transport_cc(1000, 3, {4, 8, 12});
    engine.feed_media_inbound(cc.data(), cc.size());

    const auto snap = engine.net_feedback()->snapshot();
    EXPECT_TRUE(snap.has_transport_cc);
    EXPECT_EQ(snap.source, NetFeedbackSource::kTransportCc)
        << "with transport-cc negotiated and arriving, it drives the "
           "estimate (ADR-018 Decision 2)";

    engine.tick();
    EXPECT_EQ(engine.stats().bwe_feedback_samples, 1u);
}

TEST_F(EngineNetFeedback, AnUnnegotiatedTransportCcPathDoesNotDrive) {
    NimRTCEngine engine(make_test_config());
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_NE(engine.net_feedback(), nullptr);

    // The peer never negotiated transport-cc, so a stray transport-cc
    // feedback must not take over the estimate.
    ByteBuffer cc = make_transport_cc(1000, 3, {4, 8, 12});
    engine.feed_media_inbound(cc.data(), cc.size());

    const auto snap = engine.net_feedback()->snapshot();
    EXPECT_TRUE(snap.has_transport_cc);
    EXPECT_EQ(snap.source, NetFeedbackSource::kLossRtt);
}

// ---------------------------------------------------------------------------
// E7 - the transport-cc delay trend moves the estimate
// ---------------------------------------------------------------------------
//
// ADR-018 Decision 4: the transport-cc path is preferred because it
// carries the delay trend.  This is the end-to-end proof that the
// preference is not decorative: a queue that builds up in front of the
// bottleneck moves the estimate even though not a single packet was
// lost -- which is exactly the congestion a fraction-lost counter cannot
// see yet.

namespace {

/// A transport-cc run of `count` packets whose arrival deltas are
/// `spacing_ticks` of 250 us apart.
///
/// The first delta of a feedback packet is measured from the previous
/// feedback's reference time, so it straddles the gap between two
/// feedbacks; it is given a deliberately huge value so that a series
/// whose first delta is huge is visibly *not* a queue.
std::vector<std::uint8_t> spacing_series(std::size_t count,
                                         std::uint8_t spacing_ticks) {
    std::vector<std::uint8_t> deltas;
    deltas.reserve(count);
    deltas.push_back(200);            // the inter-feedback gap: 50 ms
    deltas.insert(deltas.end(), count - 1, spacing_ticks);
    return deltas;
}

}  // namespace

TEST_F(EngineNetFeedback, TheTransportCcDelayTrendMovesTheEstimate) {
    NimRTCEngine peer(make_test_config());
    ASSERT_EQ(peer.open(), 0u);
    const std::string peer_offer = peer.create_offer();

    NimRTCEngine engine(make_test_config());
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_TRUE(engine.process_remote_sdp(peer_offer).has_value());
    ASSERT_NE(engine.bwe(), nullptr);

    // A steady 4 ms spacing: the baseline, the operating point with no
    // queue in front of the bottleneck.
    ByteBuffer cc1 = make_transport_cc(1000, 10, spacing_series(10, 16));
    engine.feed_media_inbound(cc1.data(), cc1.size());
    engine.tick();
    ASSERT_EQ(engine.bwe()->estimate().target_bitrate_bps, 1'000'000u);

    // The spacing doubles: a queue is building up.  Nothing was lost, so
    // only the delay trend can see this.
    ByteBuffer cc2 = make_transport_cc(1010, 10, spacing_series(10, 32));
    engine.feed_media_inbound(cc2.data(), cc2.size());
    engine.tick();

    EXPECT_EQ(engine.bwe()->estimate().target_bitrate_bps, 500'000u)
        << "the transport-cc arrival series must be able to move the "
           "estimate on its own (ADR-018 Decision 4)";
    EXPECT_EQ(engine.stats().bwe_feedback_samples, 2u);
}

} // namespace
