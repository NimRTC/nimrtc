/**
 * @file tests/test_engine_remote_desktop_stats.cpp
 * @brief The remote-desktop pipeline breakdown (v1.2 plan Track S / S7).
 *
 * ## What this test proves
 *
 * Plan row **S7** observed that the engine already measures the
 * pipeline's per-stage cost (`Stats::video_stage_*`) and already exposes
 * the QoE feel metrics (`Stats::qoe`), but had **no
 * remote-desktop-shaped view of where the time went**: a host had to
 * know that `video_stage_encode.mean_us` was the encode cost and then
 * divide by 1000 itself.
 *
 * `Stats::remote_desktop` closes that gap.  This file pins three
 * properties of it:
 *
 *   1. **It is wired.**  Driving the outbound pipeline moves
 *      `encode_ms`, `encode_fps` and `frames_encoded`; driving the
 *      inbound pipeline moves `decode_ms`, `decode_fps` and
 *      `frames_decoded`.
 *
 *   2. **It agrees with the stage timings.**  `encode_ms` is
 *      `video_stage_encode.mean_ns / 1e6` — the summary cannot drift
 *      from the table it summarises.
 *
 *   3. **It separates the directions.**  The drop accounting
 *      (`frames_captured - frames_encoded`) is clamped at zero and is
 *      not polluted by an empty local encode stage.
 *
 * The engine is driven exactly the way `test_engine_capture_pump.cpp`
 * and `test_engine_video_stats.cpp` drive it: the capture pump for the
 * outbound side, `feed_media_inbound()` for the receive side, and a
 * deterministic software codec so no test depends on the host's
 * hardware.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/h264/codec_plugin.hpp>
#include <nimrtc/plugins/video_capture.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/video_frame/frame.hpp>

// The cadence rule lives in its own header inside the engine's
// implementation; it is included here so the counting rule can be
// pinned deterministically, without driving a device through a
// timing-sensitive stall.  The same header is what the pump uses.
#include "engine_capture_cadence.hpp"

namespace {

namespace np = nimrtc::plugins;
namespace nc = nimrtc::core;
using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// A capture device that produces BGRA frames from its own thread.
//
// Same contract as the one in test_engine_capture_pump.cpp: a bounded
// queue, a device-owned producer thread, and capture timestamps that
// advance by the frame interval so the capture rate is measurable.
// ---------------------------------------------------------------------------

struct RdCaptureState {
    struct Frame {
        std::vector<std::uint8_t> pixels;
        std::int64_t ts_us = 0;
        std::uint32_t seq  = 0;
    };

    std::mutex mutex;
    std::deque<Frame> frames;
    Frame popped;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> produced{0};
    std::atomic<std::uint32_t> next_seq{0};
    std::atomic<std::int64_t> next_ts_us{0};
    std::uint32_t width  = 64;
    std::uint32_t height = 48;
    /**
     * The queue depth the device keeps.
     *
     * A real desktop device keeps a *bounded* queue and drops the
     * oldest frame when the consumer falls behind -- during a stall of
     * the tick thread, which is exactly when the capture rate matters.
     * With a depth of 1 that happens on every slow tick, so the test
     * exercises the lost-frame path instead of the happy path only.
     */
    std::size_t queue_depth = 1;
};

class RdCaptureDevice : public np::IVideoCapture {
public:
    RdCaptureDevice(np::CaptureConfig cfg, RdCaptureState* state)
        : cfg_(std::move(cfg)), state_(state) {}
    ~RdCaptureDevice() override { close(); }

    const char* name() const noexcept override { return "RdCaptureDevice"; }

    np::Status open() noexcept override {
        if (open_) return np::kOk;
        open_ = true;
        producer_ = std::thread([this] { produce_loop(); });
        return np::kOk;
    }

    void close() noexcept override {
        if (!open_) return;
        open_ = false;
        state_->stop.store(true);
        if (producer_.joinable()) producer_.join();
    }

    std::vector<np::CaptureDevice> enumerate_devices() const noexcept override {
        return {};
    }
    std::vector<np::CaptureFormat> supported_formats(
        const std::string&) const noexcept override {
        return {};
    }
    np::CaptureCapabilities capabilities() const noexcept override {
        np::CaptureCapabilities c;
        c.backend = "rd_stats_test";
        return c;
    }
    bool is_open() const noexcept override { return open_; }
    np::CaptureConfig config() const noexcept override { return cfg_; }

    /**
     * Inject a single frame from the test thread.
     *
     * The device's producer is asynchronous, so a sequence that the
     * test wants to exist (a rewound counter, a repeated timestamp)
     * cannot be made to happen by the producer; it is pushed here.
     */
    void push_scripted(std::int64_t ts_us, std::uint32_t seq) noexcept {
        const std::size_t bytes =
            static_cast<std::size_t>(state_->width) * state_->height * 4u;
        RdCaptureState::Frame f;
        f.pixels.assign(bytes, 0x40u);
        f.ts_us = ts_us;
        f.seq   = seq;
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->frames.push_back(std::move(f));
        ++state_->produced;
    }

    np::Status pop_frame(np::CaptureFrame& out) noexcept override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->frames.empty()) return np::kErrNotReady;
        state_->popped = std::move(state_->frames.front());
        state_->frames.pop_front();

        out.pixels        = state_->popped.pixels.data();
        out.size          = state_->popped.pixels.size();
        out.stride        = static_cast<std::int32_t>(state_->width * 4u);
        out.width         = state_->width;
        out.height        = state_->height;
        out.format        = np::VideoPixelFormat::kBGRA;
        out.capture_ts_us = state_->popped.ts_us;
        out.frame_seq     = state_->popped.seq;
        return np::kOk;
    }

    void set_frame_callback(np::CaptureFrameCallback) noexcept override {}

    np::CaptureStats stats() const noexcept override {
        np::CaptureStats s;
        s.frames_captured   = state_->produced.load();
        s.frames_cpu_staged = state_->produced.load();
        return s;
    }

private:
    void produce_loop() {
        const std::size_t bytes =
            static_cast<std::size_t>(state_->width) * state_->height * 4u;
        while (!state_->stop.load()) {
            RdCaptureState::Frame f;
            f.pixels.assign(bytes, 0x40u);
            // 33 333 us — 30 fps on the device's own clock.
            f.ts_us = state_->next_ts_us.fetch_add(33'333);
            f.seq   = state_->next_seq.fetch_add(1);
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                while (state_->frames.size() >= state_->queue_depth) {
                    state_->frames.pop_front();   // the device's own drop
                }
                state_->frames.push_back(std::move(f));
            }
            ++state_->produced;
            std::this_thread::sleep_for(5ms);
        }
    }

    np::CaptureConfig cfg_{};
    RdCaptureState*   state_ = nullptr;
    std::thread       producer_;
    bool              open_ = false;
};

class RdCaptureFactory : public np::IVideoCaptureFactory {
public:
    explicit RdCaptureFactory(RdCaptureState* state) : state_(state) {}
    std::string_view id() const noexcept override { return "rd_stats_capture"; }
    std::string_view display_name() const noexcept override {
        return "Remote-desktop stats test capture";
    }
    np::IVideoCapture* create(np::CaptureConfig cfg) const override {
        return new RdCaptureDevice(std::move(cfg), state_);
    }

private:
    RdCaptureState* state_;
};

// ---------------------------------------------------------------------------
// A deterministic software H.264 codec.
//
// The stock "h264" plugin selects a backend by priority, so a unit test
// on a workstation with a GPU would exercise a different code path than
// CI.  Wrapping the h264 module's own software adapter keeps the test on
// the in-tree fallback regardless of the host.
// ---------------------------------------------------------------------------

class RdSoftwareCodecFactory final : public np::IVideoCodecFactory {
public:
    std::string_view id() const noexcept override { return "h264-rdstats"; }
    std::string_view display_name() const noexcept override {
        return "H.264 software codec (S7 test)";
    }
    np::IVideoCodec* create(np::VideoCodecConfig cfg) const override {
        return new nimrtc::h264::CodecPluginAdapter(std::move(cfg));
    }
};

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class EngineRemoteDesktopStats : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
        static const RdSoftwareCodecFactory codec_factory;
        PluginRegistry::instance().register_video_codec(
            codec_factory.id(), &codec_factory);
        static RdCaptureState capture_state;
        static const RdCaptureFactory capture_factory(&capture_state);
        PluginRegistry::instance().register_video_capture(
            capture_factory.id(), &capture_factory);
    }

    static EngineConfig make_config() {
        EngineConfig cfg{};
        cfg.transport_strategy_name = "shell";
        cfg.video_codec_name        = "h264-rdstats";
        cfg.video_sender_name       = "reference";
        cfg.video_receiver_name     = "reference";
        cfg.video_source_name       = "memory";
        cfg.video_sink_name         = "headless";
        cfg.video_capture_name      = "rd_stats_capture";
        cfg.video_width  = 64;
        cfg.video_height = 48;
        cfg.datachannel_backend = "";
        cfg.bwe_name            = "";
        cfg.scheduler_config.enabled = false;
        return cfg;
    }

    /** Complete the inbound routing table via the peer's offer. */
    static void negotiate_video(NimRTCEngine& engine, const EngineConfig& cfg) {
        NimRTCEngine peer(cfg);
        if (peer.open() != 0) return;
        const std::string offer = peer.create_offer();
        peer.close();
        if (offer.empty()) return;
        const auto answer = engine.process_remote_sdp(offer);
        EXPECT_TRUE(answer.has_value());
    }

    static std::vector<std::vector<std::uint8_t>> split_annex_b(
        const std::vector<std::uint8_t>& bs) {
        std::vector<std::vector<std::uint8_t>> out;
        std::size_t i = 0;
        while (i + 3 < bs.size()) {
            std::size_t sc = i;
            while (sc + 3 < bs.size() &&
                   !(bs[sc] == 0 && bs[sc + 1] == 0 &&
                     (bs[sc + 2] == 1 ||
                      (bs[sc + 2] == 0 && bs[sc + 3] == 1)))) {
                ++sc;
            }
            if (sc + 3 >= bs.size()) break;
            std::size_t nal_start = (bs[sc + 2] == 1) ? sc + 3 : sc + 4;
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

    static std::vector<std::uint8_t> make_rtp(std::uint32_t ssrc,
                                              std::uint16_t seq,
                                              std::uint32_t timestamp,
                                              std::uint8_t pt,
                                              const std::vector<std::uint8_t>& payload,
                                              bool marker) {
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

    static void feed_video(NimRTCEngine& engine,
                           std::uint8_t pt,
                           std::uint16_t seq,
                           std::uint32_t rtp_timestamp,
                           std::uint32_t frame_seq) {
        auto* codec = engine.video_codec();
        if (!codec) return;

        constexpr std::uint32_t kW = 320;
        constexpr std::uint32_t kH = 240;
        const nimrtc::video_frame::VideoFrameLayout layout =
            nimrtc::video_frame::compute_layout(np::VideoPixelFormat::kI420, kW, kH);
        auto buf = std::make_shared<nimrtc::video_frame::VideoFrameBuffer>(layout);
        std::memset(buf->data(), 128, layout.buffer_size);

        np::VideoFrame raw{};
        raw.buffer = buf;
        raw.info.capture_ts_us = static_cast<std::int64_t>(frame_seq) * 33333;
        raw.info.frame_seq     = frame_seq;
        raw.info.rtp_timestamp = frame_seq * 3000;

        std::vector<std::uint8_t> enc(256 * 1024);
        np::EncodedVideoFrame encoded{};
        if (codec->encode(raw, enc.data(), enc.size(), encoded) != np::kOk) {
            return;
        }
        const auto nalus = split_annex_b(
            std::vector<std::uint8_t>(encoded.payload.begin(),
                                      encoded.payload.end()));
        if (nalus.empty()) return;

        std::uint16_t s = seq;
        for (std::size_t i = 0; i < nalus.size(); ++i) {
            const bool last = (i + 1 == nalus.size());
            const auto pkt = make_rtp(0x11223344u, s++, rtp_timestamp, pt,
                                      nalus[i], last);
            engine.feed_media_inbound(pkt.data(), pkt.size());
        }
    }
};

// ---------------------------------------------------------------------------
// 1. Baseline: a fresh engine reports an empty breakdown.
// ---------------------------------------------------------------------------

TEST_F(EngineRemoteDesktopStats, FreshEngineReportsNoBreakdown) {
    EngineConfig cfg = make_config();
    cfg.video_capture_name = "";   // no device: nothing to measure
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto s = engine.stats();
    EXPECT_EQ(s.remote_desktop.frames_captured, 0u);
    EXPECT_EQ(s.remote_desktop.frames_encoded, 0u);
    EXPECT_EQ(s.remote_desktop.frames_decoded, 0u);
    EXPECT_EQ(s.remote_desktop.frames_dropped_before_encode, 0u);
    EXPECT_EQ(s.remote_desktop.capture_fps, 0.0);
    EXPECT_EQ(s.remote_desktop.encode_fps, 0.0);
    EXPECT_EQ(s.remote_desktop.decode_fps, 0.0);
    EXPECT_EQ(s.remote_desktop.encode_ms, 0.0);
    EXPECT_EQ(s.remote_desktop.decode_ms, 0.0);

    engine.close();
}

// ---------------------------------------------------------------------------
// 2. The outbound path moves the capture and encode halves.
// ---------------------------------------------------------------------------

TEST_F(EngineRemoteDesktopStats, OutboundPipelineFillsCaptureAndEncode) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    for (int i = 0; i < 400; ++i) {
        engine.tick();
        std::this_thread::sleep_for(5ms);
        if (engine.stats().remote_desktop.frames_encoded >= 5u) break;
    }

    const auto s = engine.stats();
    const auto& rd = s.remote_desktop;

    EXPECT_GE(rd.frames_encoded, 5u)
        << "the pump must have driven the encoder";
    EXPECT_GE(rd.frames_captured, 5u)
        << "the capture cadence must be recorded from the device's timestamps";

    // The rate, from the device's own timestamps: the device's clock
    // advances by 33 333 us per capture, so once frames are not being
    // lost the measured rate is 30 fps.
    //
    // The device's queue is minimal in this fixture, so a tick that
    // spends time in the scheduler can lose a frame -- the *rate* the
    // device ran at is still on its timestamps (a lost frame widens
    // the span), which is the property being pinned.  The bound
    // therefore allows the tolerances of a loaded CI host (a stall
    // that drops a frame lowers the observed rate to ~24 fps) but not
    // the failure mode this test was written for: giving up and
    // reporting 0 because a *new* frame arrived with an older
    // timestamp than the last one seen.
    ASSERT_GT(rd.capture_fps, 0.0)
        << "capture rate must follow the device's timestamps";
    EXPECT_NEAR(rd.capture_fps, 30.0, 10.0)
        << "capture rate must follow the device's timestamps";
    EXPECT_GT(rd.capture_fps, 20.0)
        << "a rate below half the device's clock would mean the "
           "endpoint was taken from a lost frame";

    // The encode rate is a rate of *stage runs* over wall-clock time.
    EXPECT_GT(rd.encode_fps, 0.0);

    // The encode cost is a real measurement (non-negative; a zero would
    // mean the stage was never timed).
    EXPECT_GE(rd.encode_ms, 0.0);
    EXPECT_GE(rd.encode_p95_ms, rd.encode_ms)
        << "the p95 cannot be below the mean -- the mean and the p95 are "
           "read in the same nanosecond unit precisely so that a rounding "
           "artefact cannot invert them";

    // The receive side stayed empty.
    EXPECT_EQ(rd.frames_decoded, 0u);
    EXPECT_EQ(rd.decode_fps, 0.0);

    engine.close();
}

// ---------------------------------------------------------------------------
// 3. The inbound path moves the decode half.
// ---------------------------------------------------------------------------

TEST_F(EngineRemoteDesktopStats, InboundPipelineFillsDecode) {
    EngineConfig cfg = make_config();
    cfg.video_capture_name = "";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    negotiate_video(engine, cfg);

    const std::uint8_t pt = cfg.video_receiver_tuning.payload_type;
    feed_video(engine, pt, /*seq=*/100, /*rtp_ts=*/90'000, /*frame_seq=*/1);
    feed_video(engine, pt, /*seq=*/110, /*rtp_ts=*/93'000, /*frame_seq=*/2);

    const auto& rd = engine.stats().remote_desktop;
    EXPECT_EQ(rd.frames_decoded, 2u)
        << "both inbound frames must reach the decoder";
    EXPECT_GT(rd.decode_fps, 0.0)
        << "two decode runs on distinct instants give a measurable rate";
    EXPECT_GE(rd.decode_ms, 0.0);

    // Nothing was captured locally.
    EXPECT_EQ(rd.frames_captured, 0u);
    EXPECT_EQ(rd.frames_encoded, 0u);
    // A receive-only session must not look like two "drops".
    EXPECT_EQ(rd.frames_dropped_before_encode, 0u)
        << "a receive-only session captures nothing, so nothing was dropped";
    EXPECT_EQ(rd.frames_decoded_without_local_encode, 2u);

    engine.close();
}

// ---------------------------------------------------------------------------
// 4. Agreement: the breakdown is a view of the stage table, not a
//    second measurement.
// ---------------------------------------------------------------------------

TEST_F(EngineRemoteDesktopStats, BreakdownAgreesWithStageTimings) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    for (int i = 0; i < 400; ++i) {
        engine.tick();
        std::this_thread::sleep_for(5ms);
        if (engine.stats().remote_desktop.frames_encoded >= 3u) break;
    }

    const auto s = engine.stats();
    EXPECT_GT(s.remote_desktop.frames_encoded, 0u);

    EXPECT_EQ(s.remote_desktop.frames_encoded,
              s.video_stage_encode.samples)
        << "the same table feeds both views";
    EXPECT_NEAR(s.remote_desktop.encode_ms,
                static_cast<double>(s.video_stage_encode.mean_ns) / 1'000'000.0,
                1e-9)
        << "encode_ms must be mean_ns / 1e6 — no independent rounding";
    EXPECT_NEAR(s.remote_desktop.capture_ms,
                static_cast<double>(s.video_stage_capture.mean_ns) / 1'000'000.0,
                1e-9);

    // The p95 is read in the same nanosecond unit as the mean, so the
    // two cannot be inverted by a rounding artefact.
    EXPECT_GE(s.remote_desktop.encode_p95_ms, s.remote_desktop.encode_ms)
        << "a p95 below the mean would be a number no host can interpret";

    engine.close();
}

// ---------------------------------------------------------------------------
// 5. The drop accounting never wraps and never counts the receive side.
// ---------------------------------------------------------------------------

TEST_F(EngineRemoteDesktopStats, DropAccountingIsClamped) {
    EngineConfig cfg = make_config();
    cfg.video_capture_name = "";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    negotiate_video(engine, cfg);

    // Feed only inbound frames: decode moves, capture and encode do not.
    const std::uint8_t pt = cfg.video_receiver_tuning.payload_type;
    feed_video(engine, pt, /*seq=*/200, /*rtp_ts=*/90'000, /*frame_seq=*/1);

    const auto& rd = engine.stats().remote_desktop;
    EXPECT_EQ(rd.frames_captured, 0u);
    EXPECT_EQ(rd.frames_encoded, 0u);
    EXPECT_EQ(rd.frames_dropped_before_encode, 0u)
        << "captured - encoded must clamp at 0, never wrap to 2^64-1";

    engine.close();
}

// ---------------------------------------------------------------------------
// 6. The cadence survives a device whose timestamps are not monotonic.
//
// This is the regression the macOS CI caught on the first landing: the
// device's queue is bounded, so a pump that falls behind causes the
// device to drop the oldest frame -- and the next frame it hands over
// carries a timestamp *later* than the one lost, so the pump never sees
// a backwards jump from a drop.  What it does see, when the device's
// producer is faster than the queue, is frames arriving after a stall
// whose timestamps are fine but whose *sequence numbers* skipped.  The
// other shape is a source that restarts its clock (a monitor
// hot-plugged, a portal re-negotiated): the timestamp jumps back.
//
// Neither shape may make a delivered frame disappear from the count,
// and a broken timestamp span must report "unknown" (0), never a
// plausible-looking wrong number.
// ---------------------------------------------------------------------------

TEST_F(EngineRemoteDesktopStats, CadenceSurvivesNonMonotonicDevice) {
    // The counting rule, in isolation from the engine: a rewound
    // timestamp on a *new* sequence is still a delivery.
    nimrtc::engine::detail::CaptureCadence cadence;
    cadence.note(0, 1);
    cadence.note(33'333, 2);
    cadence.note(66'666, 3);
    EXPECT_EQ(cadence.frames, 3u);
    // 33 333 us is 30.0003 fps -- the device's integer microsecond
    // clock is what it is.
    EXPECT_NEAR(cadence.fps(), 30.0, 0.01);

    // A repeated sequence is the *same* delivery handed over twice.
    cadence.note(66'666, 3);
    EXPECT_EQ(cadence.frames, 3u)
        << "the same frame popped twice is not a new delivery";

    // A new sequence whose timestamp went backwards: counted, and the
    // rate becomes unknown rather than wrong.
    cadence.note(10'000, 4);
    EXPECT_EQ(cadence.frames, 4u)
        << "a rewound timestamp must not hide a delivered frame";
    EXPECT_EQ(cadence.fps(), 0.0)
        << "a broken span must report unknown, not a plausible number";

    // Once broken, the span stays broken: a later frame with a *higher*
    // timestamp is not enough, because the endpoint no longer describes
    // the interval the count spans.
    cadence.note(99'999, 5);
    EXPECT_EQ(cadence.frames, 5u);
    EXPECT_EQ(cadence.fps(), 0.0)
        << "a session whose clock rewound cannot report a rate it did "
           "not observe";

    // The sequence wraps at 2^32; a wrapped counter is a new delivery,
    // not a repeat of the last one.
    nimrtc::engine::detail::CaptureCadence wrap;
    wrap.note(0, 0xFFFFFFFEu);
    wrap.note(33'333, 0xFFFFFFFFu);
    wrap.note(66'666, 0u);
    EXPECT_EQ(wrap.frames, 3u);
}

}  // namespace
