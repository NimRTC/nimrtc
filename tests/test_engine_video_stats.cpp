/**
 * @file tests/test_engine_video_stats.cpp
 * @brief Phase 4.2 / D2 — the engine ↔ `video_stats` module wiring.
 *
 * The `video_stats` module is a complete, well-tested implementation of
 * the per-stream video quality counters (freeze detection, QP accumulation,
 * bitrate EMA, drop attribution).  Before the Phase 4.2 wiring it was a
 * *dead module*: nothing in the engine ever loaded it, so a host that
 * asked "how many freezes did we see?" got nothing — and the module's own
 * tests passed, which is the worst possible state for a feature ("the
 * code exists, so it must work").
 *
 * This file pins the *engine* half of the wiring — the part the module's
 * own tests cannot reach:
 *
 *   - the inbound path: an RTP frame arrives → `on_frame_received` →
 *     decoded → `on_frame_decoded` → rendered → `on_frame_rendered`;
 *   - the outbound path: the encoder produced a frame → the outbound
 *     sink records it (this is where the QP lands);
 *   - the drop attribution: a PLI / FIR the *peer* sends is a dependency
 *     loss on *our* transmit stream, not on the inbound one;
 *   - the freeze detector: a gap between two rendered frames shows up in
 *     `Engine::Stats::video_freeze_count`.
 *
 * The two directions are kept apart (Chrome's `inbound-rtp` /
 * `outbound-rtp` split): mixing them would produce numbers that mean
 * nothing, so the test asserts each direction on its own sink.
 *
 * The engine is driven through `feed_media_inbound()` (a documented test
 * seam) — no ICE / DTLS handshake is needed, and the packets travel in
 * cleartext, which is exactly what makes the wire format assertable.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/h264/codec_plugin.hpp>
#include <nimrtc/video_frame/frame.hpp>

namespace {

namespace np = nimrtc::plugins;
using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

using namespace std::chrono_literals;

/** The SSRC the fake peer uses for its video stream. */
constexpr std::uint32_t kPeerVideoSsrc = 0x11223344u;

/** Build a minimal RTP packet (12-byte header + payload).
 *
 *  The marker bit is set: the packet carries the *last* fragment of a
 *  frame, so the jitter buffer may release it immediately.  Without the
 *  marker the frame never completes — it would sit in the jitter buffer
 *  until the max-wait timeout and be released as an *incomplete* frame. */
std::vector<std::uint8_t> make_rtp(std::uint32_t ssrc,
                                    std::uint16_t seq,
                                    std::uint32_t timestamp,
                                    std::uint8_t pt,
                                    const std::vector<std::uint8_t>& payload,
                                    bool marker = true) {
    std::vector<std::uint8_t> out;
    out.reserve(12 + payload.size());
    out.push_back(0x80);
    out.push_back(static_cast<std::uint8_t>((pt & 0x7F) | (marker ? 0x80 : 0x00)));
    out.push_back(static_cast<std::uint8_t>((seq >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(seq & 0xFF));
    out.push_back(static_cast<std::uint8_t>((timestamp >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((timestamp >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((timestamp >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(timestamp & 0xFF));
    out.push_back(static_cast<std::uint8_t>((ssrc >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((ssrc >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((ssrc >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(ssrc & 0xFF));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

/** PLI (RFC 4585 §6.3.1) — 12 bytes, no FCI. */
std::vector<std::uint8_t> make_pli(std::uint32_t sender_ssrc,
                                   std::uint32_t media_ssrc) {
    std::vector<std::uint8_t> out;
    out.reserve(12);
    out.push_back(0x81);          // V=2, P=0, FMT=1 (PLI)
    out.push_back(206);           // PSFB
    out.push_back(0x00);          // length = 12/4 - 1 = 2
    out.push_back(0x02);
    const std::uint32_t ssrcs[2] = {sender_ssrc, media_ssrc};
    for (std::uint32_t s : ssrcs) {
        out.push_back(static_cast<std::uint8_t>((s >> 24) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((s >> 16) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((s >> 8) & 0xFF));
        out.push_back(static_cast<std::uint8_t>(s & 0xFF));
    }
    return out;
}

// ---------------------------------------------------------------------------
// A deterministic software H.264 codec.
//
// The shipped "h264" plugin selects its backend by priority: a hardware
// encoder / decoder when the host has one, the software stub otherwise.
// A unit test must not depend on which backend the *host* happens to
// have — the same binary would then pass on a CI runner and fail on a
// workstation with a GPU.
//
// This factory wraps the h264 module's own software adapter
// (`nimrtc::h264::CodecPluginAdapter`), so the codec behaves exactly like
// the in-tree fallback, but the backend table is never consulted.
//
// The id is "h264-software": the engine derives the codec *family* from
// the id when the plugin does not name one, and "h264" followed by a
// separator is the documented form for that (see
// `detail::codec_family_from_plugin_id`).
// ---------------------------------------------------------------------------

class SoftwareH264CodecFactory final : public np::IVideoCodecFactory {
public:
    std::string_view id() const noexcept override { return "h264-software"; }
    std::string_view display_name() const noexcept override {
        return "H.264 software codec (test)";
    }
    np::IVideoCodec* create(np::VideoCodecConfig cfg) const override {
        return new nimrtc::h264::CodecPluginAdapter(std::move(cfg));
    }
};

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class EngineVideoStats : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();

        static const SoftwareH264CodecFactory factory;
        PluginRegistry::instance().register_video_codec(
            "h264-software", &factory);
    }

    EngineConfig make_config() {
        EngineConfig cfg{};
        // The default plugins are enough: the "reference" receiver / sender
        // and the software H.264 codec drive the whole pipeline.
        cfg.video_receiver_name = "reference";
        cfg.video_sender_name   = "reference";
        cfg.video_codec_name    = "h264-software";
        cfg.video_sink_name     = "headless";
        // No DataChannel / BWE / scheduler noise in this test.
        cfg.datachannel_backend = "";
        cfg.bwe_name            = "";
        cfg.scheduler_config.enabled = false;
        return cfg;
    }

    /** Complete the inbound routing table.
     *
     *  The engine only routes an inbound RTP packet to the video pipeline
     *  when its payload type is in the *negotiated* routing table
     *  (`impl_->video_pt_routes`), and that table is filled exclusively by
     *  `process_remote_sdp()`.  Without a description exchange the packet
     *  is counted as an unknown payload type and dropped — which is
     *  correct behaviour (C11: a peer must not send video on a payload
     *  type it never negotiated), but it means this fixture has to
     *  negotiate before it can drive the receive path.
     *
     *  A second engine plays the peer: its offer names the video payload
     *  type, and processing that offer installs the route. */
    static void negotiate_video(NimRTCEngine& engine,
                                const EngineConfig& cfg) {
        NimRTCEngine peer(cfg);
        if (peer.open() != 0) return;
        const std::string offer = peer.create_offer();
        peer.close();
        if (offer.empty()) return;
        const auto answer = engine.process_remote_sdp(offer);
        EXPECT_TRUE(answer.has_value())
            << "the peer offer must be accepted — the routing table is "
               "built from it";
    }

    /** Split an Annex B bitstream into its NAL units.
     *
     *  The codec emits `00 00 00 01 <NAL>` (or `00 00 01 <NAL>`); the RTP
     *  payload carries the NAL *without* the start code (RFC 6184 §5.1). */
    static std::vector<std::vector<std::uint8_t>> split_annex_b(
        const std::vector<std::uint8_t>& bs) {
        std::vector<std::vector<std::uint8_t>> out;
        std::size_t i = 0;
        while (i + 3 < bs.size()) {
            // Find the next start code (3- or 4-byte).
            std::size_t sc = i;
            while (sc + 3 < bs.size() &&
                   !(bs[sc] == 0 && bs[sc + 1] == 0 &&
                     (bs[sc + 2] == 1 ||
                      (bs[sc + 2] == 0 && bs[sc + 3] == 1)))) {
                ++sc;
            }
            if (sc + 3 >= bs.size()) break;
            std::size_t nal_start = (bs[sc + 2] == 1) ? sc + 3 : sc + 4;
            // Find the *next* start code — that terminates this NAL.
            std::size_t next = nal_start;
            while (next + 3 < bs.size() &&
                   !(bs[next] == 0 && bs[next + 1] == 0 &&
                     (bs[next + 2] == 1 ||
                      (bs[next + 2] == 0 && bs[next + 3] == 1)))) {
                ++next;
            }
            if (next + 3 >= bs.size()) next = bs.size();
            if (next > nal_start) {
                out.emplace_back(bs.begin() + nal_start, bs.begin() + next);
            }
            i = next;
        }
        return out;
    }

    /** Encode one synthetic frame with the engine's *own* codec.
     *
     *  Why not a hand-written fake NAL unit: the engine picks the codec
     *  backend by priority (a hardware decoder when the host has one, the
     *  software stub otherwise).  A hand-written fake bitstream decodes
     *  under the stub and is *rejected* by a hardware decoder — so the
     *  test would pass on one machine and fail on another.
     *
     *  Encoding with the very codec the engine will decode with removes
     *  that dependency: whatever backend was selected, it can decode its
     *  own output.  The test asserts the *counters*, not the pixels, so
     *  the content is irrelevant. */
    static std::vector<std::vector<std::uint8_t>> encode_nalus(
        NimRTCEngine& engine,
        std::uint32_t frame_seq) {
        auto* codec = engine.video_codec();
        if (!codec) return {};

        constexpr std::uint32_t kW = 320;
        constexpr std::uint32_t kH = 240;
        const nimrtc::video_frame::VideoFrameLayout layout =
            nimrtc::video_frame::compute_layout(np::VideoPixelFormat::kI420,
                                                kW, kH);
        auto buf = std::make_shared<nimrtc::video_frame::VideoFrameBuffer>(layout);
        std::memset(buf->data(), 128, layout.buffer_size);

        np::VideoFrame raw{};
        raw.buffer = buf;
        raw.info.capture_ts_us  = static_cast<std::int64_t>(frame_seq) * 33333;
        raw.info.frame_seq      = frame_seq;
        raw.info.rtp_timestamp  = frame_seq * 3000;

        std::vector<std::uint8_t> enc(256 * 1024);
        np::EncodedVideoFrame encoded{};
        if (codec->encode(raw, enc.data(), enc.size(), encoded) != np::kOk) {
            return {};
        }
        return split_annex_b(
            std::vector<std::uint8_t>(encoded.payload.begin(),
                                      encoded.payload.end()));
    }

    /** Feed one inbound video frame as RTP packets (one per NAL unit).
     *
     *  The marker bit goes on the *last* packet of the frame: it tells
     *  the jitter buffer the frame is complete, so it can release it
     *  immediately instead of waiting for the max-wait timeout. */
    static void feed_video(NimRTCEngine& engine,
                            std::uint8_t pt,
                            std::uint16_t seq,
                            std::uint32_t rtp_timestamp,
                            std::uint32_t frame_seq) {
        const auto nalus = encode_nalus(engine, frame_seq);
        if (nalus.empty()) return;

        std::uint16_t s = seq;
        for (std::size_t i = 0; i < nalus.size(); ++i) {
            const bool last = (i + 1 == nalus.size());
            const auto pkt = make_rtp(kPeerVideoSsrc, s++, rtp_timestamp, pt,
                                      nalus[i], /*marker=*/last);
            engine.feed_media_inbound(pkt.data(), pkt.size());
        }
    }
};

// ---------------------------------------------------------------------------
// 1. Baseline: a fresh engine reports no quality activity
// ---------------------------------------------------------------------------

TEST_F(EngineVideoStats, FreshEngineReportsZeroVideoQualityCounters) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto s = engine.stats();
    EXPECT_EQ(s.video_freeze_count, 0u);
    EXPECT_EQ(s.video_freeze_total_ms, 0u);
    EXPECT_EQ(s.video_last_freeze_ms, 0u);
    EXPECT_EQ(s.video_keyframe_interval_avg_ms, 0.0);
    EXPECT_EQ(s.video_qp_sum, 0u);
    EXPECT_EQ(s.video_qp_count, 0u);
    EXPECT_EQ(s.video_frames_dropped_jb, 0u);
    EXPECT_EQ(s.video_frames_dropped_decoder, 0u);
    EXPECT_EQ(s.video_frames_dropped_dependency, 0u);
    EXPECT_EQ(s.video_current_bitrate_bps, 0u);
    EXPECT_EQ(s.video_avg_bitrate_bps, 0u);
    EXPECT_EQ(s.video_bytes_received, 0u);
    EXPECT_EQ(s.video_frames_decoded, 0u);
    EXPECT_EQ(s.video_frames_rendered, 0u);
    EXPECT_EQ(s.video_tx_frames_encoded, 0u);
    EXPECT_EQ(s.video_tx_bytes_encoded, 0u);
    EXPECT_EQ(s.video_tx_frames_dropped_dependency, 0u);

    engine.close();
}

// ---------------------------------------------------------------------------
// 2. Outbound: the encoder's output lands on the *transmit* sink
// ---------------------------------------------------------------------------

TEST_F(EngineVideoStats, OutboundVideoMovesTheTxCounters) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    np::VideoSourceFrame frame{};
    frame.width  = 320;
    frame.height = 240;
    frame.format = np::VideoPixelFormat::kI420;
    frame.capture_ts_us = 1'000'000;
    frame.frame_seq     = 1;
    frame.rtp_timestamp = 90'000;

    // The encoder needs actual pixels: a frame with null planes has
    // nothing to encode and `send_video()` fails outright (before any
    // statistics could be recorded).  Build a real I420 buffer — a flat
    // mid-grey is enough; the content is irrelevant to the counters.
    constexpr std::int32_t kW = 320;
    constexpr std::int32_t kH = 240;
    constexpr std::int32_t kStrideY = kW;
    constexpr std::int32_t kStrideU = kW / 2;
    constexpr std::int32_t kStrideV = kW / 2;
    std::vector<std::uint8_t> i420(
        static_cast<std::size_t>(kStrideY) * kH +
        static_cast<std::size_t>(kStrideU) * (kH / 2) +
        static_cast<std::size_t>(kStrideV) * (kH / 2),
        128);
    frame.stride_y = kStrideY;
    frame.stride_u = kStrideU;
    frame.stride_v = kStrideV;
    frame.plane_y  = i420.data();
    frame.plane_u  = i420.data() + static_cast<std::size_t>(kStrideY) * kH;
    frame.plane_v  = frame.plane_u + static_cast<std::size_t>(kStrideU) * (kH / 2);

    ASSERT_EQ(engine.send_video(frame), np::kOk);

    const auto s = engine.stats();
    EXPECT_EQ(s.video_tx_frames_encoded, 1u)
        << "the encoded frame must be recorded on the outbound sink";
    EXPECT_GT(s.video_tx_bytes_encoded, 0u)
        << "the outbound sink must account for the encoded bytes";

    // The inbound sink must NOT have moved: the two directions are kept
    // apart on purpose.
    EXPECT_EQ(s.video_frames_decoded, 0u);
    EXPECT_EQ(s.video_frames_rendered, 0u);
    EXPECT_EQ(s.video_bytes_received, 0u);

    engine.close();
}

// ---------------------------------------------------------------------------
// 3. Inbound: an RTP frame walks the whole receive pipeline
// ---------------------------------------------------------------------------

TEST_F(EngineVideoStats, InboundVideoMovesTheRxCounters) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    negotiate_video(engine, cfg);

    const std::uint8_t pt = cfg.video_receiver_tuning.payload_type;
    feed_video(engine, pt, /*seq=*/100, /*rtp_ts=*/90'000, /*frame_seq=*/1);

    const auto s = engine.stats();
    EXPECT_EQ(s.video_frames_decoded, 1u)
        << "the received frame must reach the decoder and be counted";
    EXPECT_EQ(s.video_frames_rendered, 1u)
        << "the decoded frame must reach the sink and be counted";
    EXPECT_GT(s.video_bytes_received, 0u)
        << "the inbound sink must account for the received bytes";

    // The outbound sink stays untouched.
    EXPECT_EQ(s.video_tx_frames_encoded, 0u);

    engine.close();
}

// ---------------------------------------------------------------------------
// 4. Drop attribution: a PLI from the peer is a *transmit* loss
// ---------------------------------------------------------------------------

TEST_F(EngineVideoStats, PeerPliIsAttributedAsTransmitDependencyLoss) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto pli = make_pli(/*sender_ssrc=*/0x55667788u, kPeerVideoSsrc);
    engine.feed_media_inbound(pli.data(), pli.size());

    const auto s = engine.stats();
    EXPECT_EQ(s.video_tx_frames_dropped_dependency, 1u)
        << "a PLI the peer sends is a dependency loss on the stream WE sent";
    EXPECT_EQ(s.video_frames_dropped_dependency, 0u)
        << "the inbound dependency counter must not be moved by a PLI — "
           "the peer is not the one who lost a reference frame, we are";

    engine.close();
}

// ---------------------------------------------------------------------------
// 5. Freeze detection: a gap between two rendered frames
// ---------------------------------------------------------------------------

TEST_F(EngineVideoStats, FreezeDetectionAcrossFrameGap) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    negotiate_video(engine, cfg);

    const std::uint8_t pt = cfg.video_receiver_tuning.payload_type;

    feed_video(engine, pt, /*seq=*/100, /*rtp_ts=*/90'000, /*frame_seq=*/1);
    EXPECT_EQ(engine.stats().video_frames_rendered, 1u);

    // The freeze threshold is 150 ms; wait longer than that before the
    // second frame so the gap between the two rendered frames is a freeze.
    std::this_thread::sleep_for(250ms);

    feed_video(engine, pt, /*seq=*/101, /*rtp_ts=*/93'000, /*frame_seq=*/2);
    EXPECT_EQ(engine.stats().video_frames_rendered, 2u);

    const auto s = engine.stats();
    EXPECT_GE(s.video_freeze_count, 1u)
        << "a 250 ms gap between two rendered frames is a freeze";
    EXPECT_GT(s.video_freeze_total_ms, 0u);

    engine.close();
}

} // namespace
