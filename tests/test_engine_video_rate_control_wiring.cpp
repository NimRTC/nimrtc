/**
 * @file tests/test_engine_video_rate_control_wiring.cpp
 * @brief The engine half of the E6 rate-control wiring (v1.2 plan Track E).
 *
 * ## What this file covers
 *
 * `IVideoBitrateController` has three inputs.  Two of them were wired
 * before this work:
 *
 *   - `on_bandwidth_estimate(bps)` — the link's opinion, fed from the
 *     tick;
 *   - and the controller's level is read back on every frame and pushed
 *     into the encoder.
 *
 * The third one — the controller's own *reaction* to what the peer says
 * — was not wired at all:
 *
 *   - `on_measured_bitrate(bps)` — what the encoder is actually putting
 *     on the wire.  Without it the controller's multiplicative
 *     decrease (`BitrateController::budget()`) could never fire, so the
 *     closed loop the saturation benchmark measures was not the loop
 *     the product runs;
 *   - `on_keyframe_requested()` — the peer asking for a key frame.
 *
 * This file pins that the engine *calls* both seams, using a recording
 * controller registered in place of the built-in one.  The *rules* the
 * calls carry (the window, the arithmetic) are pinned by
 * `tests/test_engine_rate_window.cpp`, which tests the same private
 * header the engine includes.
 *
 * ## Why the recording controller
 *
 * The built-in controller is a *policy*: it decides which rung to sit
 * on, and it hides the fact that it was called at all.  The wiring
 * question — "did the engine feed this seam, with this number?" —
 * needs an observer, not a policy.  So the test registers its own
 * controller under a test id and watches what the engine hands it.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/video_bitrate_controller.hpp>

namespace {

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
namespace np = nimrtc::plugins;

// ---------------------------------------------------------------------------
// The recording controller
// ---------------------------------------------------------------------------

/**
 * The recorded traffic.
 *
 * The counters live in a *static* struct because the engine owns the
 * controller it creates (it deletes it on `close()`), so the factory
 * must hand out a freshly allocated instance every time — a static
 * controller object would be deleted by the engine and the heap
 * corrupted.
 */
struct RecordingState {
    /** How many times the engine fed the measured bitrate. */
    std::atomic<std::uint32_t> measured_calls{0};

    /** The last measured bitrate the engine reported, in bits/s. */
    std::atomic<std::uint32_t> last_measured_bps{0};

    /** How many times the engine reported a key-frame request. */
    std::atomic<std::uint32_t> keyframe_calls{0};

    /** How many times the engine fed the bandwidth estimate. */
    std::atomic<std::uint32_t> estimate_calls{0};

    void reset() {
        measured_calls.store(0);
        last_measured_bps.store(0);
        keyframe_calls.store(0);
        estimate_calls.store(0);
    }
};

/**
 * A controller that records what the engine hands it and changes
 * nothing.
 *
 * It is registered under a test id, so the engine resolves *this*
 * object through the registry's video-bitrate-controller slot — the
 * same seam the built-in controller is resolved through (Layout
 * Invariant 4: the engine never names a concrete module).
 */
class RecordingController : public np::IVideoBitrateController {
public:
    explicit RecordingController(RecordingState* state) : state_(state) {}

    const char* name() const noexcept override {
        return "recording-rate-controller";
    }

    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    np::Status set_config(np::VideoBitrateControllerConfig cfg) noexcept override {
        cfg_ = std::move(cfg);
        return np::kOk;
    }
    np::VideoBitrateControllerConfig config() const noexcept override {
        return cfg_;
    }

    void on_bandwidth_estimate(std::uint32_t bps) noexcept override {
        (void)bps;
        ++state_->estimate_calls;
    }

    void on_measured_bitrate(std::uint32_t bps) noexcept override {
        ++state_->measured_calls;
        state_->last_measured_bps.store(bps);
    }

    void on_keyframe_requested() noexcept override {
        ++state_->keyframe_calls;
    }

    np::VideoLadderLevel current_level() const noexcept override {
        // A level that keeps the configured geometry, so the engine's
        // per-frame push is a no-op and the test is about the *calls*,
        // not about the encoder's parameters.
        np::VideoLadderLevel lvl;
        lvl.width       = cfg_.source_width;
        lvl.height      = cfg_.source_height;
        lvl.fps         = cfg_.source_fps;
        lvl.bitrate_bps = cfg_.source_bitrate_bps;
        return lvl;
    }

    np::VideoBitrateControllerStats stats() const noexcept override {
        return {};
    }

    np::Status set_level(std::uint32_t level_index) noexcept override {
        (void)level_index;
        return np::kOk;
    }

private:
    RecordingState* state_ = nullptr;
    np::VideoBitrateControllerConfig cfg_{};
};

class RecordingControllerFactory : public np::IVideoBitrateControllerFactory {
public:
    explicit RecordingControllerFactory(RecordingState* state) : state_(state) {}

    std::string_view id() const noexcept override {
        return "recording-rate-controller";
    }
    std::string_view display_name() const noexcept override {
        return "Recording rate controller";
    }

    np::IVideoBitrateController* create(
        np::VideoBitrateControllerConfig cfg) const override {
        // A *new* instance every time: the engine owns (and deletes)
        // the controller it resolves.
        auto* c = new RecordingController(state_);
        (void)c->set_config(std::move(cfg));
        return c;
    }

private:
    RecordingState* state_;
};

// ---------------------------------------------------------------------------
// The test fixture
// ---------------------------------------------------------------------------

class EngineVideoRateControlWiring : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
    }

    void SetUp() override {
        state_ = &shared_state();
        PluginRegistry::instance().register_video_bitrate_controller(
            "recording-rate-controller", &shared_factory());
        state_->reset();
    }

    void TearDown() override {
        PluginRegistry::instance().unregister_video_bitrate_controller(
            "recording-rate-controller");
    }

    /** The counters every test in this file shares.
     *
     *  The plugin registry is a process-wide singleton, so the factory
     *  has to be a process-lifetime object, and a process-lifetime
     *  factory cannot point at a *stack* object.  The state it
     *  records into is therefore a static too. */
    static RecordingState& shared_state() {
        static RecordingState s;
        return s;
    }

    static RecordingControllerFactory& shared_factory() {
        static RecordingControllerFactory f(&shared_state());
        return f;
    }

    /** The engine's configuration for this suite.
     *
     *  "shell" is the network-free sentinel strategy (Slice 8): the
     *  engine opens without touching ICE, so the test needs no peer
     *  and no sockets. */
    static EngineConfig make_config() {
        EngineConfig cfg;
        cfg.transport_strategy_name = "shell";
        cfg.video_bitrate_controller_name = "recording-rate-controller";
        cfg.video_codec_name   = "h264";
        cfg.video_width        = 320;
        cfg.video_height       = 240;
        cfg.video_fps          = 30;
        cfg.video_start_bitrate_bps = 1'000'000;
        // No capture device: the host feeds the frames itself.
        cfg.video_capture_name = "";
        cfg.video_capture_pump = false;
        return cfg;
    }

    /** Push one grey frame through the encoder.
     *
     *  The frame needs *pixels*: the engine stages it into a
     *  codec-facing buffer, and a hardware encoder rejects a null
     *  buffer.  A flat grey I420 picture is enough — the test is about
     *  the byte counters, not the picture. */
    static np::Status send_one_frame(NimRTCEngine& engine) {
        constexpr std::uint32_t kWidth  = 320;
        constexpr std::uint32_t kHeight = 240;
        static std::vector<std::uint8_t> y_plane(kWidth * kHeight, 128);
        static std::vector<std::uint8_t> u_plane((kWidth / 2) * (kHeight / 2), 128);
        static std::vector<std::uint8_t> v_plane((kWidth / 2) * (kHeight / 2), 128);

        np::VideoSourceFrame frame{};
        frame.width  = kWidth;
        frame.height = kHeight;
        frame.format = np::VideoPixelFormat::kI420;
        frame.capture_ts_us = 1000;
        frame.stride_y = static_cast<std::int32_t>(kWidth);
        frame.stride_u = static_cast<std::int32_t>(kWidth / 2);
        frame.stride_v = static_cast<std::int32_t>(kWidth / 2);
        frame.plane_y = y_plane.data();
        frame.plane_u = u_plane.data();
        frame.plane_v = v_plane.data();
        frame.buffer_size = y_plane.size() + u_plane.size() + v_plane.size();
        return engine.send_video(frame);
    }

    /** Build a PSFB message (RFC 4585 §6.3) for `fmt`.
     *
     *  The PLI (FMT 1) and the FIR (FMT 4) both say "send me a key
     *  frame"; they differ in how insistently they say it. */
    static std::vector<std::uint8_t> make_psfb(std::uint8_t fmt,
                                                std::uint32_t sender_ssrc,
                                                std::uint32_t media_ssrc) {
        std::vector<std::uint8_t> buf(12, 0);
        buf[0] = static_cast<std::uint8_t>(0x80 | (fmt & 0x1Fu));  // V=2, FMT
        buf[1] = 206;                                              // PSFB
        buf[2] = 0;
        buf[3] = 2;   // length in 32-bit words minus one
        buf[4]  = static_cast<std::uint8_t>((sender_ssrc >> 24) & 0xFF);
        buf[5]  = static_cast<std::uint8_t>((sender_ssrc >> 16) & 0xFF);
        buf[6]  = static_cast<std::uint8_t>((sender_ssrc >> 8) & 0xFF);
        buf[7]  = static_cast<std::uint8_t>(sender_ssrc & 0xFF);
        buf[8]  = static_cast<std::uint8_t>((media_ssrc >> 24) & 0xFF);
        buf[9]  = static_cast<std::uint8_t>((media_ssrc >> 16) & 0xFF);
        buf[10] = static_cast<std::uint8_t>((media_ssrc >> 8) & 0xFF);
        buf[11] = static_cast<std::uint8_t>(media_ssrc & 0xFF);
        return buf;
    }

    static inline RecordingState* state_ = nullptr;
};

// ---------------------------------------------------------------------------
// The measured bitrate
// ---------------------------------------------------------------------------

TEST_F(EngineVideoRateControlWiring,
       TheMeasuredBitrateIsNotReportedBeforeAWindowCloses) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    ASSERT_EQ(send_one_frame(engine), np::kOk);

    // The first tick establishes the window's anchor: there is no
    // interval yet, so there is no rate.
    engine.tick();

    EXPECT_EQ(state_->measured_calls.load(), 0u)
        << "the first observation has no interval, so no rate may be "
           "reported — a rate over a zero-length interval would be a "
           "division by zero dressed up as a number";
}

TEST_F(EngineVideoRateControlWiring,
       TheMeasuredBitrateIsReportedOnceTheWindowCloses) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    // The anchor.
    ASSERT_EQ(send_one_frame(engine), np::kOk);
    engine.tick();
    ASSERT_EQ(state_->measured_calls.load(), 0u);

    // Let the one-second window elapse, then put more bytes on the
    // wire so the window has something to measure.
    std::this_thread::sleep_for(std::chrono::milliseconds(1'100));
    ASSERT_EQ(send_one_frame(engine), np::kOk);
    engine.tick();

    ASSERT_EQ(state_->measured_calls.load(), 1u)
        << "the engine must feed the controller what the encoder is "
           "actually putting on the wire — without this seam the "
           "controller's multiplicative decrease can never fire";
    EXPECT_GT(state_->last_measured_bps.load(), 0u)
        << "the reported rate must be the measured rate, not a zero "
           "(a zero reads as 'no measurement', which is the same as "
           "'not congested')";
}

TEST_F(EngineVideoRateControlWiring,
       TheMeasuredBitrateIsReportedForEveryClosedWindow) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    ASSERT_EQ(send_one_frame(engine), np::kOk);
    engine.tick();

    for (int i = 0; i < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1'100));
        ASSERT_EQ(send_one_frame(engine), np::kOk);
        engine.tick();
    }

    EXPECT_EQ(state_->measured_calls.load(), 3u)
        << "the window is closed, not sliding: each elapsed second is "
           "one measurement";
}

TEST_F(EngineVideoRateControlWiring,
       AnEngineWithoutAControllerStillTicks) {
    // The measurement seam must not be load-bearing: a session with no
    // controller configured has nothing to feed, and the engine must
    // not care.
    EngineConfig cfg = make_config();
    cfg.video_bitrate_controller_name = "";

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    ASSERT_EQ(send_one_frame(engine), np::kOk);
    engine.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(1'100));
    ASSERT_EQ(send_one_frame(engine), np::kOk);
    engine.tick();

    EXPECT_EQ(state_->measured_calls.load(), 0u);
    engine.close();
}

// ---------------------------------------------------------------------------
// The key-frame request
// ---------------------------------------------------------------------------

TEST_F(EngineVideoRateControlWiring, AnIncomingPliStepsTheLadderDown) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    const auto pli = make_psfb(/*fmt=*/1, 0xDEADBEEFu, 0x12345678u);
    engine.feed_media_inbound(pli.data(), pli.size());

    EXPECT_EQ(state_->keyframe_calls.load(), 1u)
        << "a PLI means the peer cannot decode what it is getting, so "
           "the controller must be told (v1.2 plan E6)";
}

TEST_F(EngineVideoRateControlWiring, AnIncomingFirStepsTheLadderDown) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    const auto fir = make_psfb(/*fmt=*/4, 0xDEADBEEFu, 0x12345678u);
    engine.feed_media_inbound(fir.data(), fir.size());

    EXPECT_EQ(state_->keyframe_calls.load(), 1u)
        << "a FIR says the same thing a PLI does, and more insistently";
}

TEST_F(EngineVideoRateControlWiring, EachKeyframeRequestIsCounted) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    const auto pli = make_psfb(/*fmt=*/1, 0xDEADBEEFu, 0x12345678u);
    for (int i = 0; i < 5; ++i) {
        engine.feed_media_inbound(pli.data(), pli.size());
    }

    EXPECT_EQ(state_->keyframe_calls.load(), 5u)
        << "the controller counts the requests itself; the engine's job "
           "is to hand every one of them over";
}

TEST_F(EngineVideoRateControlWiring,
       AnUnrelatedFeedbackMessageIsNotAKeyframeRequest) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    // A REMB (FMT 15) is the peer's bandwidth estimate, not a
    // complaint about a frame: it must not step the ladder down.
    const auto remb = make_psfb(/*fmt=*/15, 0xDEADBEEFu, 0x12345678u);
    engine.feed_media_inbound(remb.data(), remb.size());

    EXPECT_EQ(state_->keyframe_calls.load(), 0u)
        << "only a PLI (FMT 1) and a FIR (FMT 4) ask for a key frame";
}

}  // namespace
