/**
 * @file tests/test_engine_capture_timestamp.cpp
 * @brief v1.2.0 (audio pipeline, WP-9) — the cross-end capture timestamp of
 *        an inbound audio frame (spec B-17).
 *
 * The capture timestamp answers the question "when did the *peer*
 * capture this frame?" — not "when did we play it out?".  It is the
 * audio half of the A/V-sync contract (C-4): a host that has to line a
 * voice frame up with a video frame needs both on the *peer's*
 * timeline, and the playout clock is not that timeline.
 *
 * Two rules are absolute here (spec O-9), and they are what this file
 * pins down:
 *
 *   * **Never fake it.**  Without a usable Sender Report the value is
 *     0, and 0 means "not reconstructible".  It is never derived from
 *     the local playout clock — that would silently mix two different
 *     clocks (the peer's wall clock and ours) and produce a number that
 *     looks plausible and is wrong.
 *
 *   * **Never guess the clock rate.**  The media clock rate comes from
 *     the peer's own `a=rtpmap:` line; the engine pins it on the
 *     tracker when the first packet of an SSRC arrives.
 *
 * The engine is driven through the "shell" transport strategy (the same
 * sentinel `test_engine_audio_state.cpp` uses), so `open()` reaches the
 * open state without binding ICE UDP sockets: these cases are about the
 * timeline seam, not about the transport.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/codec.hpp>
#include <nimrtc/rtp/packet.hpp>

namespace {

using nimrtc::engine::AudioFrameInfo;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

using namespace std::chrono_literals;

/** The payload type RFC 7587 assigns to Opus, and the one the engine
 *  advertises by default. */
constexpr std::uint8_t kOpusPt = 111;

/** The SSRC the fake peer uses for its audio stream. */
constexpr std::uint32_t kPeerAudioSsrc = 0x0B0B0B0Bu;

/** An NTP timestamp in the past (2023-08-17T05:20:00Z), used as the
 *  peer's wall clock in the Sender Reports the test feeds. */
constexpr std::uint64_t kPeerNtp = 3900000000ull << 32;

class EngineCaptureTimestamp : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
    }

    static EngineConfig make_test_config() {
        EngineConfig cfg;
        cfg.stun_server_host       = "";
        cfg.local_port_range_begin = 52600;
        cfg.local_port_range_end   = 52700;
        // "shell" is the Slice 8 sentinel strategy: open() succeeds
        // without touching the network (see test_engine_audio_state.cpp).
        cfg.transport_strategy_name = "shell";
        // The cross-end timeline seam (ADR-017) has to be on: with it
        // off the engine keeps no anchor at all and the capture instant
        // is 0 by construction (the case that switches it off sets the
        // flag to false explicitly).
        cfg.timeline_enabled = true;
        return cfg;
    }

    /** Encode one 20 ms frame with the registered Opus codec plugin and
     *  wrap it into a real RTP packet.
     *
     *  Since WP-7 the receive path *decodes*: the RTP payload is an
     *  Opus frame, not raw PCM.  Returns an empty buffer when the Opus
     *  plugin is not registered (a build without the Opus module). */
    static nimrtc::core::ByteBuffer make_opus_rtp(std::uint32_t ssrc,
                                                   std::uint8_t pt,
                                                   std::uint32_t seq,
                                                   std::uint32_t rtp_ts,
                                                   std::size_t samples_per_channel) {
        const auto* factory =
            nimrtc::core::PluginRegistry::instance().get_codec("opus");
        if (!factory) return {};

        nimrtc::plugins::CodecConfig cc{};
        cc.sample_rate_hz = 48000;
        cc.channels       = 1;
        cc.bitrate_bps    = 64000;
        cc.complexity     = 10;
        cc.fec_enabled    = true;
        cc.dtx_enabled    = false;
        cc.vad_enabled    = false;
        cc.payload_type   = pt;
        cc.name           = "opus";

        std::unique_ptr<nimrtc::plugins::ICodec> enc(factory->create(cc));
        if (!enc) return {};
        if (enc->open() != nimrtc::plugins::kOk) return {};

        std::vector<float> pcm(samples_per_channel, 0.25f);
        std::vector<std::uint8_t> bytes(4096);
        const std::size_t n = enc->encode(pcm.data(), samples_per_channel,
                                          bytes.data(), bytes.size());
        if (n == 0) return {};

        nimrtc::rtp::PacketBuilder pb;
        pb.set_ssrc(ssrc)
          .set_seq(static_cast<std::uint16_t>(seq))
          .set_timestamp(rtp_ts)
          .set_payload_type(pt);
        pb.set_payload(nimrtc::core::ByteSpan(bytes.data(), n));
        return pb.build();
    }

    /** Build the peer's Sender Report: the anchor of the cross-end
     *  timeline (ADR-017) and the source of the receiver report's LSR
     *  field (RFC 3550 §6.4.1). */
    static nimrtc::core::ByteBuffer make_sender_report(std::uint32_t ssrc,
                                                       std::uint64_t ntp_timestamp,
                                                       std::uint32_t rtp_timestamp) {
        nimrtc::rtp::SrBuilder sb;
        sb.set_ssrc(ssrc)
          .set_ntp_timestamp(ntp_timestamp)
          .set_rtp_timestamp(rtp_timestamp)
          .set_sender_packet_count(2)
          .set_sender_octet_count(400);
        return sb.build();
    }

    /** Feed two Opus packets and let the pipeline play the first one
     *  out.  Returns false when the Opus plugin is missing. */
    static bool feed_two_frames(NimRTCEngine& engine) {
        const auto first = make_opus_rtp(kPeerAudioSsrc, kOpusPt, 1, 0, 960);
        if (first.empty()) return false;
        engine.feed_media_inbound(first.data(), first.size());

        std::this_thread::sleep_for(120ms);

        const auto second = make_opus_rtp(kPeerAudioSsrc, kOpusPt, 2, 960, 960);
        if (second.empty()) return false;
        engine.feed_media_inbound(second.data(), second.size());

        // The pipeline is clock-driven: the playout happens on tick().
        for (int i = 0; i < 5; ++i) {
            engine.tick();
            std::this_thread::sleep_for(20ms);
        }
        return true;
    }
};

// ============================================================================
// 1. Without a Sender Report there is no capture timestamp (spec O-9)
// ============================================================================

TEST_F(EngineCaptureTimestamp, CaptureTimeIsZeroWithoutASenderReport) {
    EngineConfig cfg = make_test_config();
    // The cross-end timeline is *enabled* here: the seam is live, the
    // peer simply has not sent the Sender Report that anchors it yet.
    cfg.timeline_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    int frames = 0;
    std::int64_t last_capture_us = -1;
    engine.set_on_audio_frame_info([&](const AudioFrameInfo& info) {
        ++frames;
        last_capture_us = info.capture_time_us;
    });

    ASSERT_TRUE(feed_two_frames(engine))
        << "the Opus codec plugin is not registered in this build";

    ASSERT_GT(frames, 0)
        << "a decoded Opus packet must reach the host through the detailed "
           "callback";
    EXPECT_EQ(last_capture_us, 0)
        << "without a Sender Report the capture instant is not "
           "reconstructible, and 0 means exactly that — it must never be "
           "faked from the local playout clock (spec O-9)";

    engine.close();
}

TEST_F(EngineCaptureTimestamp, CaptureTimeStaysZeroWithTheTimelineDisabled) {
    EngineConfig cfg = make_test_config();
    // The cross-end timeline is switched off: the engine must not keep
    // an anchor at all (ADR-017 "Decision 5"), so the capture instant
    // stays 0 even though the peer sent a Sender Report.
    cfg.timeline_enabled = false;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    int frames = 0;
    std::int64_t last_capture_us = -1;
    engine.set_on_audio_frame_info([&](const AudioFrameInfo& info) {
        ++frames;
        last_capture_us = info.capture_time_us;
    });

    // The first packet teaches the engine the SSRC and the clock rate.
    const auto first = make_opus_rtp(kPeerAudioSsrc, kOpusPt, 1, 0, 960);
    if (first.empty()) {
        GTEST_SKIP() << "the Opus codec plugin is not registered in this build";
    }
    engine.feed_media_inbound(first.data(), first.size());

    const auto sr = make_sender_report(kPeerAudioSsrc, kPeerNtp, 0);
    ASSERT_FALSE(sr.empty());
    engine.feed_media_inbound(sr.data(), sr.size());

    std::this_thread::sleep_for(120ms);
    const auto second = make_opus_rtp(kPeerAudioSsrc, kOpusPt, 2, 960, 960);
    ASSERT_FALSE(second.empty());
    engine.feed_media_inbound(second.data(), second.size());

    for (int i = 0; i < 5; ++i) {
        engine.tick();
        std::this_thread::sleep_for(20ms);
    }

    ASSERT_GT(frames, 0);
    EXPECT_EQ(last_capture_us, 0)
        << "with the cross-end timeline disabled the engine must not "
           "reconstruct a capture instant";

    engine.close();
}

// ============================================================================
// 2. With a Sender Report the capture instant is reconstructed (spec B-17)
// ============================================================================

TEST_F(EngineCaptureTimestamp, CaptureTimeIsReconstructedFromASenderReport) {
    EngineConfig cfg = make_test_config();
    cfg.timeline_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    int frames = 0;
    std::int64_t last_capture_us = 0;
    engine.set_on_audio_frame_info([&](const AudioFrameInfo& info) {
        ++frames;
        last_capture_us = info.capture_time_us;
    });

    // The first packet teaches the engine the SSRC and the media clock
    // rate (the peer's `a=rtpmap:` rate, 48 kHz for Opus).
    const auto first = make_opus_rtp(kPeerAudioSsrc, kOpusPt, 1, 0, 960);
    if (first.empty()) {
        GTEST_SKIP() << "the Opus codec plugin is not registered in this build";
    }
    engine.feed_media_inbound(first.data(), first.size());

    // The peer's Sender Report is the anchor: it pairs the peer's wall
    // clock with the peer's media clock.
    const auto sr = make_sender_report(kPeerAudioSsrc, kPeerNtp, 0);
    ASSERT_FALSE(sr.empty());
    engine.feed_media_inbound(sr.data(), sr.size());

    std::this_thread::sleep_for(120ms);
    const auto second = make_opus_rtp(kPeerAudioSsrc, kOpusPt, 2, 960, 960);
    ASSERT_FALSE(second.empty());
    engine.feed_media_inbound(second.data(), second.size());

    for (int i = 0; i < 5; ++i) {
        engine.tick();
        std::this_thread::sleep_for(20ms);
    }

    ASSERT_GT(frames, 0)
        << "a decoded Opus packet must reach the host through the detailed "
           "callback";
    EXPECT_NE(last_capture_us, 0)
        << "with a Sender Report for the audio source the capture instant "
           "must be reconstructed (spec B-17)";

    // The reconstructed instant must be the *peer's* wall clock, not
    // ours: the Sender Report says 2023-08-17T05:20:00Z, which is
    // 1'691'011'200 seconds after the Unix epoch.
    constexpr std::int64_t kExpectedUnixSeconds = 1691011200;
    const std::int64_t capture_seconds = last_capture_us / 1'000'000;
    EXPECT_NEAR(static_cast<double>(capture_seconds),
                static_cast<double>(kExpectedUnixSeconds), 2.0)
        << "the reconstructed capture instant must be on the peer's "
           "timeline, not ours";

    engine.close();
}

TEST_F(EngineCaptureTimestamp, TheCaptureTimestampIsThePeersClockNotOurs) {
    EngineConfig cfg = make_test_config();
    cfg.timeline_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    std::int64_t last_capture_us = 0;
    engine.set_on_audio_frame_info([&](const AudioFrameInfo& info) {
        last_capture_us = info.capture_time_us;
    });

    // The first packet teaches the engine the SSRC and the clock rate.
    const auto first = make_opus_rtp(kPeerAudioSsrc, kOpusPt, 1, 0, 960);
    if (first.empty()) {
        GTEST_SKIP() << "the Opus codec plugin is not registered in this build";
    }
    engine.feed_media_inbound(first.data(), first.size());

    // An anchor far in the past: the peer's clock is *not* the local
    // wall clock, and the reconstruction must follow the peer's.
    const auto sr = make_sender_report(kPeerAudioSsrc, kPeerNtp, 0);
    ASSERT_FALSE(sr.empty());
    engine.feed_media_inbound(sr.data(), sr.size());

    std::this_thread::sleep_for(120ms);
    const auto second = make_opus_rtp(kPeerAudioSsrc, kOpusPt, 2, 960, 960);
    ASSERT_FALSE(second.empty());
    engine.feed_media_inbound(second.data(), second.size());

    for (int i = 0; i < 5; ++i) {
        engine.tick();
        std::this_thread::sleep_for(20ms);
    }

    ASSERT_NE(last_capture_us, 0);

    const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    EXPECT_LT(last_capture_us, now_us - 1'000'000LL)
        << "the reconstructed capture instant must not be the local wall "
           "clock: the peer's Sender Report is three years old, so a value "
           "close to 'now' would mean the engine faked it from the local "
           "playout clock (spec O-9)";

    engine.close();
}

}  // namespace
