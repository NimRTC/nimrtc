/**
 * @file tests/test_engine_timeline.cpp
 * @brief ADR-017 — the *engine* side of the cross-end frame timeline.
 *
 * `src/modules/rtp/tests/test_timeline.cpp` covers the reconstruction
 * arithmetic (`nimrtc::rtp::SenderReportTracker`) in isolation.  This
 * suite covers the wiring above it — the part that decides whether the
 * arithmetic ever runs at all:
 *
 *   1. `EngineConfig::timeline_enabled = false` emits **nothing**:
 *      no `a=extmap` in the offer, no abs-send-time on outbound RTP,
 *      no Sender Reports.  The flag must cost zero bytes on the wire
 *      when off (ADR-017 §"Decision 5").
 *
 *   2. With the flag on, `create_offer()` advertises the abs-send-time
 *      extension at `kAbsSendTimeDefaultLocalId` and the engine reports
 *      that id back through `abs_send_time_ext_id()`.
 *
 *   3. Before any inbound Sender Report is seen, every reconstruction
 *      accessor returns `std::nullopt` — the engine does NOT invent a
 *      capture timestamp from the abs-send-time field alone.
 *
 *   4. `Profile::timeline_enabled` → `EngineConfig::timeline_enabled` is
 *      the documented host hand-off; the field defaults to `false` so a
 *      host that forgets to copy it gets the off-state, not the on-state.
 *
 *   5. **The reconstruction itself.** Cases 1-4 are all about config
 *      plumbing: they pass whether or not the tracker is wired to
 *      anything. Cases 5-7 push real RTCP Sender Reports and real RTP
 *      packets (built with `nimrtc::rtp::SrBuilder` / `PacketBuilder`)
 *      through `feed_media_inbound()` — the same RTCP-demux → RTP-parse →
 *      dispatch tail `on_transport_recv()` uses — and assert the capture
 *      instant that comes back out. This is the only path that would have
 *      caught the 1.875× audio clock-rate bug, and the only one that
 *      would catch a regression of it.
 *
 *   6. **The inter-clock skew.** The skew accessor
 *      (`last_sender_recv_skew_us()` / `sender_recv_skew_us_for_ssrc()`)
 *      is the difference between how far *our* clock advanced and how
 *      far the peer's *media* clock advanced between the Sender Report
 *      and the packet. It is the quantity a jitter-buffer controller
 *      needs, and it is the one accessor that is easy to publish
 *      without ever having computed anything — so cases 8-11 pin both
 *      the value and the "empty when there is nothing to say" contract.
 *
 * The suite uses the "shell" transport strategy (the same sentinel
 * `test_datachannel_engine` uses) so `open()` reaches the open state
 * without binding ICE UDP sockets — these cases are about config
 * plumbing and SDP, not the transport.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/rtp/packet.hpp>
#include <nimrtc/rtp/timeline.hpp>

namespace {

using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
using nimrtc::rtp::kAbsSendTimeDefaultLocalId;
using nimrtc::rtp::kNtpEpochOffset;

class EngineTimeline : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
    }

    static EngineConfig make_test_config() {
        EngineConfig cfg;
        cfg.stun_server_host       = "";
        cfg.local_port_range_begin = 52400;
        cfg.local_port_range_end   = 52500;
        // "shell" is the Slice 8 sentinel strategy: open() succeeds
        // without touching the network.  See test_datachannel_engine.cpp
        // for why the real stack cannot be used on CI runners.
        cfg.transport_strategy_name = "shell";
        return cfg;
    }

    // ---- Packet factories -------------------------------------------
    //
    // These build genuine wire bytes rather than poking the tracker, so
    // the cases below exercise the *whole* engine seam: the RTCP demux,
    // the SR parse, the abs-send-time element lookup under the negotiated
    // extension id, and the per-SSRC clock-rate pinning.

    static std::int64_t now_unix_us() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    /** Wall-clock seconds since the Unix epoch, truncated.
     *
     *  The reconstruction asserts an exact microsecond value, so the SR's
     *  NTP instant must land on a whole second: a fractional second would
     *  round-trip through the 2^-32 second field with a ±1 µs truncation
     *  and make the expected value ambiguous. */
    static std::int64_t whole_second_unix_us() {
        return (now_unix_us() / 1'000'000LL) * 1'000'000LL;
    }

    static nimrtc::core::ByteBuffer make_sr(std::uint32_t ssrc,
                                            std::uint32_t rtp_ts,
                                            std::int64_t unix_us) {
        // EXPECT_*, not ASSERT_*: the latter expands to `return;` on
        // failure, which cannot compile in a value-returning function.
        EXPECT_EQ(unix_us % 1'000'000LL, 0)
            << "make_sr() expects a whole-second instant — see "
               "whole_second_unix_us()";
        // Split the two halves so the signed→unsigned conversion is
        // explicit. `unix_us / 1'000'000LL + kNtpEpochOffset` in one
        // expression makes GCC convert the signed operand to unsigned as
        // part of the arithmetic (-Wsign-conversion).
        const std::uint64_t ntp_secs =
            static_cast<std::uint64_t>(unix_us / 1'000'000LL) + kNtpEpochOffset;
        const std::uint64_t ntp = ntp_secs << 32;
        nimrtc::rtp::SrBuilder sr;
        sr.set_ssrc(ssrc).set_ntp_timestamp(ntp).set_rtp_timestamp(rtp_ts);
        return sr.build();
    }

    static nimrtc::core::ByteBuffer make_rtp(std::uint32_t ssrc,
                                             std::uint8_t pt,
                                             std::uint32_t rtp_ts,
                                             std::uint32_t ast24) {
        static const std::uint8_t kPayload[4] = {0xDE, 0xAD, 0xBE, 0xEF};
        nimrtc::rtp::PacketBuilder pb;
        pb.set_ssrc(ssrc).set_seq(0).set_timestamp(rtp_ts).set_payload_type(pt);
        pb.set_abs_send_time(nimrtc::rtp::AbsSendTime{ast24},
                             kAbsSendTimeDefaultLocalId);
        pb.set_payload(
            nimrtc::core::ByteSpan(kPayload, sizeof(kPayload)));
        return pb.build();
    }

    /** Run the SDP negotiation a real session performs.
     *
     *  `observe_timeline()` returns immediately when no peer's `a=extmap`
     *  was seen, so without this no reconstruction is possible at all.
     *  A second throwaway engine stands in for the peer and supplies its
     *  offer — the same handshake order test_dtls_srtp_timing.cpp uses. */
    static bool negotiate_with_peer(NimRTCEngine& rx, const EngineConfig& cfg) {
        NimRTCEngine tx(cfg);
        if (tx.open() != 0) return false;
        const std::string offer = tx.create_offer();
        if (offer.empty()) return false;
        return rx.process_remote_sdp(offer).has_value();
    }
};

// =====================================================================
// 1. Flag off ⇒ no extmap, no negotiated id.
// =====================================================================
TEST_F(EngineTimeline, DisabledEngineAdvertisesNoAbsSendTimeExtmap) {
    EngineConfig cfg = make_test_config();
    cfg.timeline_enabled = false;
    ASSERT_FALSE(cfg.timeline_enabled);

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    EXPECT_FALSE(engine.timeline_enabled())
        << "the accessor must mirror EngineConfig::timeline_enabled";

    const std::string offer = engine.create_offer();
    ASSERT_FALSE(offer.empty()) << "create_offer() must succeed for a "
                                   "shell-strategy engine";

    EXPECT_EQ(offer.find(nimrtc::rtp::kAbsSendTimeUri), std::string::npos)
        << "with timeline_enabled=false the offer MUST NOT advertise the "
           "abs-send-time extension (ADR-017 Decision 5)";

    EXPECT_EQ(engine.abs_send_time_ext_id(), 0u)
        << "no extmap advertised ⇒ outbound packets carry no abs-send-time";
    EXPECT_EQ(engine.remote_abs_send_time_ext_id(), 0u);

    engine.close();
}

// =====================================================================
// 2. Flag on ⇒ extmap advertised at the well-known local id.
// =====================================================================
TEST_F(EngineTimeline, EnabledEngineAdvertisesAbsSendTimeExtmap) {
    EngineConfig cfg = make_test_config();
    cfg.timeline_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    EXPECT_TRUE(engine.timeline_enabled());

    const std::string offer = engine.create_offer();
    ASSERT_FALSE(offer.empty()) << "create_offer() must succeed for a "
                                   "shell-strategy engine";

    const auto pos = offer.find(nimrtc::rtp::kAbsSendTimeUri);
    ASSERT_NE(pos, std::string::npos)
        << "with timeline_enabled=true the offer MUST advertise "
           "abs-send-time so the peer knows to expect the element";

    // The line must name the id the engine actually writes outbound.
    const std::uint16_t id = engine.abs_send_time_ext_id();
    ASSERT_NE(id, 0u);
    EXPECT_EQ(id, nimrtc::rtp::kAbsSendTimeDefaultLocalId);
    EXPECT_NE(offer.find("a=extmap:" + std::to_string(id) + " " +
                         std::string(nimrtc::rtp::kAbsSendTimeUri)),
              std::string::npos)
        << "the advertised id must match abs_send_time_ext_id() — a "
           "mismatch is a silent 'extension present but wrong id' bug "
           "(RFC 5285 §6)";

    engine.close();
}

// =====================================================================
// 3. No SR seen yet ⇒ every reconstruction accessor is empty.
//
// This is the guard that the abs-send-time refinement is not allowed to
// fabricate an epoch on its own: the field is only meaningful relative
// to a Sender Report.
// =====================================================================
TEST_F(EngineTimeline, ReconstructionAccessorsAreEmptyWithoutSenderReport) {
    EngineConfig cfg = make_test_config();
    cfg.timeline_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    EXPECT_FALSE(engine.last_capture_ts_us().has_value())
        << "no inbound SR ⇒ no capture timestamp may be synthesised";
    EXPECT_FALSE(engine.capture_ts_us_for_ssrc(0xDEADBEEF).has_value());
    EXPECT_FALSE(engine.last_one_way_delay_us().has_value())
        << "the one-way delay is a difference of two instants, both of "
           "which require an SR";
    EXPECT_FALSE(engine.last_sender_recv_skew_us().has_value())
        << "the inter-clock skew is measured between an SR and a packet; "
           "with neither seen there is nothing to report";
    EXPECT_FALSE(engine.sender_recv_skew_us_for_ssrc(0xDEADBEEF).has_value())
        << "the per-SSRC skew accessor must be empty for an SSRC the "
           "engine has never seen";

    engine.close();
}

// =====================================================================
// 4. The feature gate is opt-in: a default-constructed config is off.
// =====================================================================
TEST_F(EngineTimeline, TimelineIsOptInByDefault) {
    const EngineConfig defaults{};
    EXPECT_FALSE(defaults.timeline_enabled)
        << "ADR-017: timeline_enabled must default to false so an engine "
           "built without the seam emits no RTCP and no abs-send-time";

    // The SR cadence has a sane default even when the gate is off — it is
    // inert until the gate opens.
    EXPECT_GT(defaults.sr_period_ms, 0u);
    EXPECT_EQ(defaults.sr_period_ms, nimrtc::rtp::kSrPeriodMs)
        << "the default cadence should track nimrtc::rtp::kSrPeriodMs "
           "(the WebRTC default) so the two do not drift";
}

// =====================================================================
// 5. A Sender Report off the wire anchors the reconstruction — at the
//    right media clock rate.
//
// This is the case that would have caught the 1.875× audio bug: the SR
// and the probe packet both arrive as raw bytes, and the only thing
// telling the engine that this SSRC runs at 48 kHz is the payload-type
// demux in handle_rtp(). If that pinning regresses to the 90 kHz
// default, the assertion below fails by ~467 ms.
// =====================================================================
TEST_F(EngineTimeline, SenderReportFromTheWireAnchorsReconstruction) {
    constexpr std::uint32_t kSsrc = 0x0A0A0A0Au;
    constexpr std::uint8_t  kPt   = 111;    // Opus, RFC 7587
    constexpr std::uint32_t kRate = 48000;

    EngineConfig cfg = make_test_config();
    cfg.timeline_enabled            = true;
    cfg.audio_codec.payload_type    = kPt;
    cfg.audio_codec.clock_rate      = kRate;

    NimRTCEngine rx(cfg);
    ASSERT_EQ(rx.open(), 0u);
    ASSERT_TRUE(negotiate_with_peer(rx, cfg))
        << "the SDP handshake a real session performs must succeed";
    ASSERT_EQ(rx.remote_abs_send_time_ext_id(), kAbsSendTimeDefaultLocalId)
        << "no negotiated extmap ⇒ observe_timeline() bails out and no "
           "reconstruction is possible at all";

    // Anchor the SR a few seconds in the past so the reconstructed
    // capture instant — and therefore the one-way delay — is positive.
    const std::int64_t sr_us = whole_second_unix_us() - 5'000'000LL;

    const auto sr = make_sr(kSsrc, /*rtp_ts=*/48'000, sr_us);
    ASSERT_FALSE(sr.empty());
    rx.feed_media_inbound(sr.data(), sr.size());

    // The SR is an *anchor*, not a capture instant: the accessors stay
    // empty until a media packet asks for one.
    EXPECT_FALSE(rx.capture_ts_us_for_ssrc(kSsrc).has_value())
        << "an SR alone must not synthesise a capture timestamp";
    EXPECT_FALSE(rx.last_capture_ts_us().has_value());

    // One second of audio later: 48000 ticks at 48 kHz.
    const auto pkt = make_rtp(kSsrc, kPt, /*rtp_ts=*/96'000, /*ast24=*/0);
    ASSERT_FALSE(pkt.empty());
    rx.feed_media_inbound(pkt.data(), pkt.size());

    const auto cap = rx.capture_ts_us_for_ssrc(kSsrc);
    ASSERT_TRUE(cap.has_value())
        << "the SR was ingested and the packet carried a negotiated "
           "abs-send-time element, so a capture instant must exist";

    EXPECT_EQ(*cap, sr_us + 1'000'000LL)
        << "48000 ticks at 48 kHz is 1 s; got " << (*cap - sr_us)
        << " µs instead — the per-SSRC clock-rate pinning (plan gap #1) "
           "has regressed to the 90 kHz default";

    EXPECT_EQ(rx.last_capture_ts_us(), cap)
        << "the convenience accessor must mirror the per-SSRC one";

    const auto owd = rx.last_one_way_delay_us();
    ASSERT_TRUE(owd.has_value());
    // arrival − capture, and the packet arrived ~4.4 s after the capture
    // instant it reconstructed. This also pins the sign: the accessor used
    // to return the negation, so a 4 s transit read back as −4 s — a
    // number no latency consumer can act on.
    EXPECT_GT(*owd, 3'000'000)
        << "one-way delay must be arrival minus capture, i.e. positive; "
           "got " << *owd << " µs";
    EXPECT_LT(*owd, 6'000'000)
        << "capture was reconstructed ~4 s before arrival, so the "
           "one-way delay must be ~4 s, not " << *owd << " µs";
}

// =====================================================================
// 6. abs-send-time refinement beats the media-clock extrapolation.
//
// The probe below sits 50400 ticks past the SR — 1.05 s at 48 kHz —
// while its abs-send-time counter says 1.000 s. That 5% gap is a media
// clock running fast, exactly the error the refinement exists to cancel
// (ADR-017 §"Decision 2"). The assertion distinguishes the two paths:
// an unrefined estimate would report +1_050_000 µs.
// =====================================================================
TEST_F(EngineTimeline, AbsSendTimeRefinementOverridesMediaClockEstimate) {
    constexpr std::uint32_t kSsrc      = 0x0B0B0B0Bu;
    constexpr std::uint8_t  kPt        = 111;
    constexpr std::uint32_t kAstAnchor = 0x100000u;

    EngineConfig cfg = make_test_config();
    cfg.timeline_enabled            = true;
    cfg.audio_codec.payload_type    = kPt;
    cfg.audio_codec.clock_rate      = 48000;

    NimRTCEngine rx(cfg);
    ASSERT_EQ(rx.open(), 0u);
    ASSERT_TRUE(negotiate_with_peer(rx, cfg));
    ASSERT_EQ(rx.remote_abs_send_time_ext_id(), kAbsSendTimeDefaultLocalId);

    const std::int64_t sr_us = whole_second_unix_us() - 5'000'000LL;
    const auto sr = make_sr(kSsrc, /*rtp_ts=*/0, sr_us);
    rx.feed_media_inbound(sr.data(), sr.size());

    // Anchor packet: sits exactly on the SR's RTP timestamp, so its
    // abs-send-time value is what the tracker pairs the SR with.
    const auto anchor = make_rtp(kSsrc, kPt, /*rtp_ts=*/0, kAstAnchor);
    rx.feed_media_inbound(anchor.data(), anchor.size());

    // 65536 abs-send-time ticks at 65536 Hz is exactly 1 s.
    const auto probe =
        make_rtp(kSsrc, kPt, /*rtp_ts=*/50'400, kAstAnchor + 65'536u);
    rx.feed_media_inbound(probe.data(), probe.size());

    const auto cap = rx.capture_ts_us_for_ssrc(kSsrc);
    ASSERT_TRUE(cap.has_value());
    EXPECT_EQ(*cap, sr_us + 1'000'000LL)
        << "expected the abs-send-time refined value (1.000 s); the "
           "media-clock extrapolation alone would have given "
        << (*cap - sr_us)
        << " µs — so either the refinement did not run, or its "
           "agreement tolerance rejected a consistent anchor";
}

// =====================================================================
// 7. The gate is symmetric: with timeline_enabled=false the engine must
//    ignore an inbound SR entirely, not merely decline to emit one.
// =====================================================================
TEST_F(EngineTimeline, DisabledEngineIgnoresInboundSenderReport) {
    constexpr std::uint32_t kSsrc = 0x0C0C0C0Cu;
    constexpr std::uint8_t  kPt   = 111;

    EngineConfig cfg = make_test_config();
    cfg.timeline_enabled         = false;
    cfg.audio_codec.payload_type = kPt;
    cfg.audio_codec.clock_rate   = 48000;

    NimRTCEngine rx(cfg);
    ASSERT_EQ(rx.open(), 0u);

    const std::int64_t sr_us = whole_second_unix_us() - 5'000'000LL;
    const auto sr = make_sr(kSsrc, /*rtp_ts=*/0, sr_us);
    rx.feed_media_inbound(sr.data(), sr.size());

    const auto pkt = make_rtp(kSsrc, kPt, /*rtp_ts=*/50'400, 0x100000u);
    rx.feed_media_inbound(pkt.data(), pkt.size());

    EXPECT_FALSE(rx.capture_ts_us_for_ssrc(kSsrc).has_value())
        << "handle_rtcp() must drop the SR when the seam is off — an "
           "inbound SR is of no use to a disabled tracker";
    EXPECT_FALSE(rx.last_capture_ts_us().has_value());
    EXPECT_FALSE(rx.last_one_way_delay_us().has_value());
    EXPECT_FALSE(rx.last_sender_recv_skew_us().has_value())
        << "the skew is computed on the same path as the capture "
           "instant, so a disabled seam must publish neither";
    EXPECT_FALSE(rx.sender_recv_skew_us_for_ssrc(kSsrc).has_value());
}

// =====================================================================
// 8. The inter-clock skew is published once there is something to say.
//
// The skew answers: "how far did the peer's *media* clock advance
// between the Sender Report and this packet, compared with how far
// *our* clock advanced in the same interval?"  Concretely,
//
//   skew = (our clock at packet) − (our clock at SR) − (media delta)
//
// The two `feed_media_inbound()` calls below are microseconds apart, so
// the first term is ~0 and the skew is essentially minus the media
// delta: the peer's media clock claims 1 s passed while our clock says
// essentially none did.
//
// The sign matters.  A skew that came back positive would tell a
// jitter-buffer controller to *shorten* its buffer by the reported
// amount, which is the opposite of what a peer whose media clock runs
// fast needs.
// =====================================================================
TEST_F(EngineTimeline, SenderReportFromTheWirePublishesTheInterClockSkew) {
    constexpr std::uint32_t kSsrc = 0x0D0D0D0Du;
    constexpr std::uint8_t  kPt   = 111;    // Opus, RFC 7587
    constexpr std::uint32_t kRate = 48000;

    EngineConfig cfg = make_test_config();
    cfg.timeline_enabled            = true;
    cfg.audio_codec.payload_type    = kPt;
    cfg.audio_codec.clock_rate      = kRate;

    NimRTCEngine rx(cfg);
    ASSERT_EQ(rx.open(), 0u);
    ASSERT_TRUE(negotiate_with_peer(rx, cfg));

    const std::int64_t sr_us = whole_second_unix_us() - 5'000'000LL;
    const auto sr = make_sr(kSsrc, /*rtp_ts=*/48'000, sr_us);
    ASSERT_FALSE(sr.empty());
    rx.feed_media_inbound(sr.data(), sr.size());

    // One second of audio later: 48000 ticks at 48 kHz.  ast24 = 0 is
    // an unrepresentable abs-send-time value, so the tracker ignores the
    // element and the estimate comes from the media clock alone — which
    // is what makes the expected skew exact.
    const auto pkt = make_rtp(kSsrc, kPt, /*rtp_ts=*/96'000, /*ast24=*/0);
    ASSERT_FALSE(pkt.empty());
    rx.feed_media_inbound(pkt.data(), pkt.size());

    const auto skew = rx.last_sender_recv_skew_us();
    ASSERT_TRUE(skew.has_value())
        << "the SR was ingested and the packet reconstructed, so the "
           "inter-clock skew must be published";

    // 1 s of media, and the two feeds are microseconds apart ⇒ the skew
    // is −1 s.  The tolerance absorbs the feed-to-feed interval and the
    // scheduling jitter around it, while still being an order of
    // magnitude tighter than the 2 s case below.
    EXPECT_NEAR(static_cast<double>(*skew), -1'000'000.0, 20'000.0)
        << "the skew must be the media-advance discrepancy (≈ −1 s for "
           "1 s of media), got " << *skew << " µs";

    EXPECT_EQ(rx.sender_recv_skew_us_for_ssrc(kSsrc), skew)
        << "the per-SSRC accessor must agree with the convenience one";
}

// =====================================================================
// 9. The skew is not a constant: it tracks the media advance.
//
// A publisher that returned a fixed value (or the negation of the
// media delta) would pass case 8 and be useless to a consumer.  The
// magnitude here is twice case 8's, and the sign is the same.
// =====================================================================
TEST_F(EngineTimeline, SkewMagnitudeTracksTheMediaAdvance) {
    constexpr std::uint32_t kSsrc = 0x0E0E0E0Eu;
    constexpr std::uint8_t  kPt   = 111;

    EngineConfig cfg = make_test_config();
    cfg.timeline_enabled            = true;
    cfg.audio_codec.payload_type    = kPt;
    cfg.audio_codec.clock_rate      = 48000;

    NimRTCEngine rx(cfg);
    ASSERT_EQ(rx.open(), 0u);
    ASSERT_TRUE(negotiate_with_peer(rx, cfg));

    const std::int64_t sr_us = whole_second_unix_us() - 5'000'000LL;
    const auto sr = make_sr(kSsrc, /*rtp_ts=*/0, sr_us);
    rx.feed_media_inbound(sr.data(), sr.size());

    // Two seconds of audio: 96000 ticks at 48 kHz.
    const auto pkt = make_rtp(kSsrc, kPt, /*rtp_ts=*/96'000, /*ast24=*/0);
    rx.feed_media_inbound(pkt.data(), pkt.size());

    const auto skew = rx.last_sender_recv_skew_us();
    ASSERT_TRUE(skew.has_value());
    EXPECT_NEAR(static_cast<double>(*skew), -2'000'000.0, 20'000.0)
        << "2 s of media advance must read back as ≈ −2 s of inter-clock "
           "discrepancy, got " << *skew << " µs";
}

// =====================================================================
// 10. The skew is published per SSRC.
//
// Two streams on one connection have two independent clock pairs
// (RFC 5956 simulcast, or audio + video), so a single "last skew"
// number cannot describe them.  The per-SSRC accessor must answer for
// the stream that was actually observed and stay empty for the others.
// =====================================================================
TEST_F(EngineTimeline, SkewIsPublishedPerSsrc) {
    constexpr std::uint32_t kSsrcA = 0x0F0F0F0Fu;
    constexpr std::uint32_t kSsrcB = 0x10101010u;
    constexpr std::uint8_t  kPt    = 111;

    EngineConfig cfg = make_test_config();
    cfg.timeline_enabled            = true;
    cfg.audio_codec.payload_type    = kPt;
    cfg.audio_codec.clock_rate      = 48000;

    NimRTCEngine rx(cfg);
    ASSERT_EQ(rx.open(), 0u);
    ASSERT_TRUE(negotiate_with_peer(rx, cfg));

    const std::int64_t sr_us = whole_second_unix_us() - 5'000'000LL;
    const auto sr = make_sr(kSsrcA, /*rtp_ts=*/0, sr_us);
    rx.feed_media_inbound(sr.data(), sr.size());

    const auto pkt = make_rtp(kSsrcA, kPt, /*rtp_ts=*/48'000, /*ast24=*/0);
    rx.feed_media_inbound(pkt.data(), pkt.size());

    const auto skew_a = rx.sender_recv_skew_us_for_ssrc(kSsrcA);
    ASSERT_TRUE(skew_a.has_value())
        << "the observed stream must have a skew";
    EXPECT_NEAR(static_cast<double>(*skew_a), -1'000'000.0, 20'000.0);

    EXPECT_FALSE(rx.sender_recv_skew_us_for_ssrc(kSsrcB).has_value())
        << "a stream that was never observed must not inherit the other "
           "stream's skew";
    EXPECT_EQ(rx.last_sender_recv_skew_us(), skew_a)
        << "the convenience accessor mirrors the most recent stream";
}

} // anonymous namespace