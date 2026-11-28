/**
 * @file src/engine/tests/test_engine_device_policy.cpp
 * @brief The v1-device-policy (D3 / D4) deterministic tests.
 *
 * ## What this file proves
 *
 * The slice has three behaviours that must be pinned with code, not with
 * a real capture backend:
 *
 *   1. **The default device policy (D3).**  `detail::default_device_policy()`
 *      returns the per-platform table, an *explicit* capture id always wins
 *      over the policy, `"auto"` resolves through the policy, and a policy
 *      backend this build does not register degrades to "no device" (a
 *      warn) instead of being an error.
 *
 *   2. **The reconnect backoff machine (D4).**  A lost device is reopened
 *      with an exponential backoff — 100 ms, capped at 5 s, at most 20
 *      attempts — and a device that never recovers ends in `kStopped`
 *      (which the engine reports as `kVideoCaptureOpenFailed`).
 *
 *   3. **The engine wiring.**  Driving a real `NimRTCEngine` (shell
 *      transport, software codec) with fake capture devices proves that
 *      `video_capture_name = "auto"` creates the *policy's* backend, that a
 *      removed device increments `video_capture_device_changes`, that the
 *      backoff recovers it (incrementing `video_capture_restarts`), and
 *      that a re-added device is counted again.
 *
 * The two private headers (`engine_device_policy.hpp` /
 * `engine_capture_reconnect.hpp`) are included directly so the rule and the
 * test are the same rule.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/capture_device_events.hpp>
#include <nimrtc/plugins/video_capture.hpp>
#include <nimrtc/plugins/video_codec.hpp>

#include "engine_capture_reconnect.hpp"
#include "engine_device_policy.hpp"

namespace {

namespace np = nimrtc::plugins;
namespace nc = nimrtc::core;
namespace nd = nimrtc::engine::detail;

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// A deterministic software H.264 codec (seam-only, no concrete module header).
//
// The capture pump needs a working encoder so `tick()` can drain the device
// queue into the RTP path.  Wrapping the h264 module's own adapter here would
// drag a concrete module header into `src/engine/`, which Layout Invariant 4
// forbids (ADR-025 — h264 is not on the exemption list).  So the codec is a
// fake that emits a tiny Annex-B NAL unit, the same seam-only pattern the
// top-level capture-pump test uses.
// ---------------------------------------------------------------------------

class FakeCodec final : public np::IVideoCodec {
public:
    // ---- IPlugin ----------------------------------------------------------
    const char* name() const noexcept override { return "FakeCodec"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    // ---- IVideoCodec ------------------------------------------------------
    bool can_encode() const noexcept override { return true; }
    bool can_decode() const noexcept override { return false; }
    np::VideoCodecKind kind() const noexcept override {
        return np::VideoCodecKind::kH264;
    }
    std::string_view codec_name() const noexcept override { return "h264"; }

    np::Status encode(const np::VideoFrame& raw,
                      std::uint8_t* output,
                      std::size_t output_capacity,
                      np::EncodedVideoFrame& encoded_out) noexcept override {
        if (output_capacity < 8) return np::kErrBufferTooSmall;
        static const std::uint8_t kAnnexB[] = {
            0x00, 0x00, 0x00, 0x01,  // start code
            0x65, 0xAA, 0xBB, 0xCC,  // one NAL unit
        };
        for (std::size_t i = 0; i < sizeof(kAnnexB); ++i) output[i] = kAnnexB[i];
        encoded_out.payload = nc::ByteSpan{output, sizeof(kAnnexB)};
        encoded_out.is_keyframe = true;
        encoded_out.codec = np::VideoCodecKind::kH264;
        encoded_out.nalu_format = np::NaluFormat::kAnnexBStartCode;
        encoded_out.info = raw.info;
        ++frames_encoded_;
        return np::kOk;
    }

    np::Status decode(const np::EncodedVideoFrame&, np::VideoFrame&,
                      std::uint8_t* const*) noexcept override {
        return np::kErrUnsupported;
    }
    np::Status force_keyframe() noexcept override { return np::kOk; }
    np::VideoCodecConfig config() const noexcept override { return cfg_; }
    np::Status update_config(np::VideoCodecConfig cfg) noexcept override {
        cfg_ = std::move(cfg);
        return np::kOk;
    }
    np::VideoCodecStats stats() const noexcept override {
        np::VideoCodecStats s;
        s.frames_encoded = frames_encoded_;
        return s;
    }
    std::uint8_t payload_type() const noexcept override { return 102; }
    std::string_view backend_id() const noexcept override { return "fake"; }
    bool is_hardware_accelerated() const noexcept override { return false; }

private:
    np::VideoCodecConfig cfg_{};
    std::uint64_t frames_encoded_ = 0;
};

class FakeCodecFactory final : public np::IVideoCodecFactory {
public:
    std::string_view id() const noexcept override {
        return "device-policy-fake-codec";
    }
    std::string_view display_name() const noexcept override {
        return "Fake H.264 codec (device-policy test)";
    }
    np::IVideoCodec* create(np::VideoCodecConfig cfg) const override {
        auto* codec = new FakeCodec();
        codec->update_config(std::move(cfg));
        return codec;
    }
};

// ---------------------------------------------------------------------------
// A capture device whose device list and health the test controls.
//
// `present` is the whole contract:
//   - `enumerate_devices()` returns the device only while present (so the
//     engine's Route B diff sees a removal / re-add);
//   - `open()` refuses to open while absent (so the reconnect backoff's
//     reopen fails until the device comes back);
//   - `pop_frame()` synthesises a 64x48 BGRA frame only while present and
//     open.
// ---------------------------------------------------------------------------

class FakeCaptureDevice : public np::IVideoCapture {
public:
    explicit FakeCaptureDevice(np::CaptureConfig cfg) : cfg_(std::move(cfg)) {}

    ~FakeCaptureDevice() override { close(); }

    const char* name() const noexcept override { return "FakeCaptureDevice"; }

    np::Status open() noexcept override {
        if (!present()) return np::kErrNotReady;
        open_ = true;
        return np::kOk;
    }
    void close() noexcept override { open_ = false; }
    bool is_open() const noexcept override { return open_; }
    np::CaptureConfig config() const noexcept override { return cfg_; }

    std::vector<np::CaptureDevice> enumerate_devices() const noexcept override {
        if (!present()) return {};
        np::CaptureDevice d;
        d.id        = "fake-dev-1";
        d.name      = "Fake Camera 1";
        d.kind      = np::CaptureDeviceKind::kCamera;
        d.max_width = 64;
        d.max_height = 48;
        return {d};
    }
    std::vector<np::CaptureFormat> supported_formats(
        const std::string&) const noexcept override {
        return {};
    }
    np::CaptureCapabilities capabilities() const noexcept override {
        np::CaptureCapabilities c;
        c.backend = "fake-test-backend";
        return c;
    }

    np::Status pop_frame(np::CaptureFrame& out) noexcept override {
        if (!present() || !open_) return np::kErrNotReady;
        out.pixels        = pixels_.data();
        out.size          = pixels_.size();
        out.stride        = static_cast<std::int32_t>(64u * 4u);
        out.width         = 64;
        out.height        = 48;
        out.format        = np::VideoPixelFormat::kBGRA;
        out.capture_ts_us = next_ts_us_;
        next_ts_us_ += 33'333;
        out.frame_seq = ++seq_;
        return np::kOk;
    }
    void set_frame_callback(np::CaptureFrameCallback) noexcept override {}

    np::CaptureStats stats() const noexcept override { return stats_; }

    // ---- Test controls -----------------------------------------------------
    void set_present(bool p) noexcept { present_ = p; }
    bool present() const noexcept { return present_; }

private:
    std::vector<std::uint8_t> pixels_ =
        std::vector<std::uint8_t>(64u * 48u * 4u, 0x40u);
    np::CaptureConfig cfg_;
    np::CaptureStats stats_{};
    std::int64_t  next_ts_us_ = 0;
    std::uint32_t seq_        = 0;
    bool open_    = false;
    bool present_ = true;
};

class FakeCaptureFactory final : public np::IVideoCaptureFactory {
public:
    explicit FakeCaptureFactory(std::string id) : id_(std::move(id)) {}
    std::string_view id() const noexcept override { return id_; }
    std::string_view display_name() const noexcept override {
        return "Fake capture factory (device-policy test)";
    }
    np::IVideoCapture* create(np::CaptureConfig cfg) const override {
        return new FakeCaptureDevice(std::move(cfg));
    }

private:
    std::string id_;
};

// ---------------------------------------------------------------------------
// The Route A variant: the same controllable device that ALSO implements
// `ICaptureDeviceEventSource`, so the engine's poll takes structured change
// events instead of diffing the enumeration.
// ---------------------------------------------------------------------------

class EventSourceCaptureDevice final
    : public FakeCaptureDevice,
      public np::ICaptureDeviceEventSource {
public:
    explicit EventSourceCaptureDevice(np::CaptureConfig cfg)
        : FakeCaptureDevice(std::move(cfg)) {}

    void inject_event(np::CaptureDeviceChangeEvent ev) noexcept {
        events_.push_back(std::move(ev));
    }

    std::vector<np::CaptureDeviceChangeEvent>
    take_device_changes() noexcept override {
        std::vector<np::CaptureDeviceChangeEvent> out = std::move(events_);
        events_.clear();
        return out;
    }

private:
    std::vector<np::CaptureDeviceChangeEvent> events_;
};

class EventSourceFactory final : public np::IVideoCaptureFactory {
public:
    explicit EventSourceFactory(std::string id) : id_(std::move(id)) {}
    std::string_view id() const noexcept override { return id_; }
    std::string_view display_name() const noexcept override {
        return "Event-source capture factory (device-policy test)";
    }
    np::IVideoCapture* create(np::CaptureConfig cfg) const override {
        return new EventSourceCaptureDevice(std::move(cfg));
    }

private:
    std::string id_;
};

// ---------------------------------------------------------------------------
// The engine fixture: register the default plugins, then register the fake
// capture factories — INCLUDING under the two policy backend ids, so the
// "auto" tests resolve deterministically to the fakes on every platform.
// ---------------------------------------------------------------------------

class EngineDevicePolicy : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
        static const FakeCodecFactory codec_factory;
        PluginRegistry::instance().register_video_codec(
            codec_factory.id(), &codec_factory);

        const nd::DevicePolicy policy = nd::default_device_policy();
        static const FakeCaptureFactory screen_fake(
            std::string(policy.screen_capture_backend));
        static const FakeCaptureFactory camera_fake(
            std::string(policy.camera_capture_backend));
        static const FakeCaptureFactory explicit_fake("explicit-fake-capture");
        static const FakeCaptureFactory change_fake("change-detect-capture");
        static const EventSourceFactory event_fake("event-source-capture");
        // Registering under the policy ids OVERWRITES the real backends for
        // this process — the auto tests must not open a real capture device.
        PluginRegistry::instance().register_video_capture(
            screen_fake.id(), &screen_fake);
        PluginRegistry::instance().register_video_capture(
            camera_fake.id(), &camera_fake);
        PluginRegistry::instance().register_video_capture(
            explicit_fake.id(), &explicit_fake);
        PluginRegistry::instance().register_video_capture(
            change_fake.id(), &change_fake);
        PluginRegistry::instance().register_video_capture(
            event_fake.id(), &event_fake);
    }

    static EngineConfig make_config() {
        EngineConfig cfg{};
        cfg.transport_strategy_name = "shell";
        cfg.video_codec_name        = "device-policy-fake-codec";
        cfg.video_sender_name       = "reference";
        cfg.video_receiver_name     = "reference";
        cfg.video_source_name       = "memory";
        cfg.video_sink_name         = "headless";
        cfg.video_width  = 64;
        cfg.video_height = 48;
        cfg.datachannel_backend = "";
        cfg.bwe_name            = "";
        cfg.scheduler_config.enabled = false;
        return cfg;
    }
};

// ===========================================================================
// 1. The default device policy (D3)
// ===========================================================================

TEST(DevicePolicy, DefaultPolicyMatchesPlatformTable) {
    const nd::DevicePolicy p = nd::default_device_policy();
#if defined(_WIN32)
    EXPECT_EQ(p.screen_capture_backend, "windows-wgc");
    EXPECT_EQ(p.camera_capture_backend, "camera-mediafoundation");
#elif defined(__APPLE__)
    EXPECT_EQ(p.screen_capture_backend, "macos-screencapturekit");
    EXPECT_EQ(p.camera_capture_backend, "camera-avfoundation");
#elif defined(__linux__)
    EXPECT_EQ(p.screen_capture_backend, "linux-x11");
    EXPECT_EQ(p.camera_capture_backend, "camera-v4l2");
#else
    EXPECT_EQ(p.screen_capture_backend, "synthetic");
    EXPECT_EQ(p.camera_capture_backend, "synthetic-camera");
#endif
    // The audio fields are reserved for D1 and must be empty today.
    EXPECT_EQ(p.audio_capture_backend, "");
    EXPECT_EQ(p.audio_playback_backend, "");
}

TEST(DevicePolicy, PublicQueryMatchesPrivateTable) {
    const nimrtc::engine::DevicePolicyInfo info =
        NimRTCEngine::default_device_policy();
    const nd::DevicePolicy p = nd::default_device_policy();
    EXPECT_EQ(info.screen_capture_backend, p.screen_capture_backend);
    EXPECT_EQ(info.camera_capture_backend, p.camera_capture_backend);
    EXPECT_EQ(info.audio_capture_backend, p.audio_capture_backend);
    EXPECT_EQ(info.audio_playback_backend, p.audio_playback_backend);
}

TEST(DevicePolicy, ExplicitIdWinsOverPolicy) {
    nd::DevicePolicy policy;
    policy.screen_capture_backend = "windows-wgc";
    policy.camera_capture_backend = "camera-mediafoundation";

    // A non-empty explicit id wins verbatim, whatever the policy says.
    EXPECT_EQ(nd::resolve_capture_backend("synthetic", policy,
                                          /*want_camera=*/false),
              "synthetic");
    EXPECT_EQ(nd::resolve_capture_backend("synthetic", policy,
                                          /*want_camera=*/true),
              "synthetic");

    // With no explicit id the policy selects the wanted direction (D3-1:
    // kText -> screen backend, everything else -> camera backend).
    EXPECT_EQ(nd::resolve_capture_backend("", policy, /*want_camera=*/false),
              "windows-wgc");
    EXPECT_EQ(nd::resolve_capture_backend("", policy, /*want_camera=*/true),
              "camera-mediafoundation");
}

TEST(DevicePolicy, AutoResolvesThroughPolicy) {
    nd::DevicePolicy policy;
    policy.screen_capture_backend = "linux-x11";
    policy.camera_capture_backend = "camera-v4l2";

    const auto registered = [](std::string_view) noexcept { return true; };
    EXPECT_EQ(nd::resolve_auto_capture_backend(policy, false, registered),
              "linux-x11");
    EXPECT_EQ(nd::resolve_auto_capture_backend(policy, true, registered),
              "camera-v4l2");
}

TEST(DevicePolicy, PolicyBackendNotRegisteredDegradesToNoDevice) {
    // The D3 degrade rule: "auto" pointing at a backend this build does not
    // register is NOT an error — it degrades to "" (the engine then warns
    // and installs no capture device), unlike an unknown *explicit* id.
    nd::DevicePolicy policy;
    policy.screen_capture_backend = "backend-not-in-this-build";

    const auto unregistered = [](std::string_view) noexcept { return false; };
    EXPECT_EQ(nd::resolve_auto_capture_backend(policy, false, unregistered),
              "");
}

TEST(DevicePolicy, EmptyPolicyDegradesToNoDevice) {
    nd::DevicePolicy policy;  // all four fields empty
    const auto registered = [](std::string_view) noexcept { return true; };
    EXPECT_EQ(nd::resolve_auto_capture_backend(policy, false, registered), "");
}

// ===========================================================================
// 2. The capture-reconnect backoff machine (D4)
// ===========================================================================

TEST(CaptureReconnect, LostProbesImmediatelyThenBacksOff) {
    nd::CaptureReconnect r;
    EXPECT_EQ(r.state(), nd::CaptureReconnect::State::kIdle);

    r.note_lost(/*now_us=*/0);
    EXPECT_EQ(r.state(), nd::CaptureReconnect::State::kBackoff);
    EXPECT_EQ(r.current_backoff_us(), 0)
        << "the first reopen must be probed immediately, not deferred: a "
           "device that recovered between the loss notification and the "
           "next tick must be back on the pump at once";
    EXPECT_TRUE(r.due(0));

    // Only a *failed* probe starts the clock: 100 ms, doubling, capped.
    r.note_failed(0);
    EXPECT_EQ(r.current_backoff_us(), 100'000);
    EXPECT_FALSE(r.due(99'999));
    EXPECT_TRUE(r.due(100'000));
}

TEST(CaptureReconnect, SecondLossDoesNotResetTheTimer) {
    // A device that stays lost must not reset its own window: re-notifying
    // a loss mid-window keeps the original retry instant.
    nd::CaptureReconnect r;
    r.note_lost(0);
    r.note_failed(100'000);  // first probe failed -> due again at 200'000
    EXPECT_EQ(r.current_backoff_us(), 100'000);
    EXPECT_FALSE(r.due(150'000));

    r.note_lost(150'000);  // still gone; must NOT move next_retry_us
    EXPECT_EQ(r.state(), nd::CaptureReconnect::State::kBackoff);
    EXPECT_FALSE(r.due(199'999));
    EXPECT_TRUE(r.due(200'000));
}

TEST(CaptureReconnect, SuccessResetsToIdle) {
    nd::CaptureReconnect r;
    r.note_lost(0);
    r.note_failed(100'000);
    r.note_failed(300'000);
    EXPECT_EQ(r.attempts(), 2u);

    r.note_succeeded(900'000);
    EXPECT_EQ(r.state(), nd::CaptureReconnect::State::kIdle);
    EXPECT_EQ(r.attempts(), 0u);
    EXPECT_EQ(r.current_backoff_us(), 0);
}

TEST(CaptureReconnect, BackoffIsBoundedAndStopsAtTwentyAttempts) {
    // Spec D4 (criterion 6): the retry count is ≤ 20, the interval between
    // retries ≤ 5 s, and a device that never recovers ends in kStopped
    // (which the engine reports as kVideoCaptureOpenFailed).
    nd::CaptureReconnectParams params;
    params.initial_backoff_us = 100'000;
    params.max_backoff_us     = 5'000'000;
    params.max_attempts       = 20;
    nd::CaptureReconnect r(params);

    r.note_lost(0);

    std::int64_t  failures = 0;
    std::int64_t  now_us   = 100'000;
    std::int64_t  max_gap  = 0;
    while (r.state() != nd::CaptureReconnect::State::kStopped) {
        ASSERT_TRUE(r.due(now_us))
            << "the retry must be due exactly at its scheduled instant";
        r.note_failed(now_us);
        ++failures;
        max_gap = std::max(max_gap, r.current_backoff_us());
        if (r.state() == nd::CaptureReconnect::State::kStopped) break;
        now_us += r.current_backoff_us();
    }

    EXPECT_EQ(failures, 20u) << "exactly max_attempts failed reopens, no more";
    EXPECT_LE(failures, 20u);
    EXPECT_LE(max_gap, 5'000'000) << "the backoff must never exceed 5 s";
    EXPECT_EQ(r.state(), nd::CaptureReconnect::State::kStopped);
    EXPECT_EQ(r.attempts(), 20u);
}

// ===========================================================================
// 3. The engine wiring (D3 "auto" + D4 hot-plug)
// ===========================================================================

TEST_F(EngineDevicePolicy, EmptyCaptureNameInstallsNoDevice) {
    // Unchanged D3 baseline: "" means the host feeds frames itself.
    EngineConfig cfg = make_config();
    cfg.video_capture_name = "";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    EXPECT_EQ(engine.video_capture(), nullptr);
    engine.close();
}

TEST_F(EngineDevicePolicy, AutoResolvesToThePolicyScreenBackend) {
    EngineConfig cfg = make_config();
    cfg.video_capture_name = "auto";
    cfg.video_content_hint = np::ContentHint::kText;  // D3-1 -> screen backend
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    auto* dev = engine.video_capture();
    ASSERT_NE(dev, nullptr) << "\"auto\" with kText must create a device";
    ASSERT_TRUE(dev->is_open());
    // The created device is the fake registered under the policy's screen
    // backend id — proof that "auto" resolved through the policy table
    // rather than picking some fixed backend.
    auto* fake = dynamic_cast<FakeCaptureDevice*>(dev);
    ASSERT_NE(fake, nullptr);
    EXPECT_EQ(dev->capabilities().backend, "fake-test-backend");
    engine.close();
}

TEST_F(EngineDevicePolicy, AutoContentHintRoutesCameraDirection) {
    // D3-1: every hint other than kText (incl. the default kUnspecified)
    // routes to the camera backend.
    EngineConfig cfg = make_config();
    cfg.video_capture_name = "auto";
    cfg.video_content_hint = np::ContentHint::kUnspecified;
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    auto* dev = engine.video_capture();
    ASSERT_NE(dev, nullptr);
    ASSERT_TRUE(dev->is_open());
    auto* fake = dynamic_cast<FakeCaptureDevice*>(dev);
    ASSERT_NE(fake, nullptr);
    EXPECT_EQ(dev->capabilities().backend, "fake-test-backend");
    engine.close();
}

TEST_F(EngineDevicePolicy, ExplicitCaptureIdWinsOverPolicy) {
    EngineConfig cfg = make_config();
    cfg.video_capture_name = "explicit-fake-capture";
    cfg.video_content_hint = np::ContentHint::kText;  // policy would be screen
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    auto* dev = engine.video_capture();
    ASSERT_NE(dev, nullptr)
        << "an explicit id must be honoured verbatim, never folded into "
           "the policy table";
    ASSERT_TRUE(dev->is_open());
    auto* fake = dynamic_cast<FakeCaptureDevice*>(dev);
    ASSERT_NE(fake, nullptr);
    engine.close();
}

TEST_F(EngineDevicePolicy, DeviceRemovalRecoveryAndReaddAreCounted) {
    // Criterion 5 (changes 0 -> 1 -> 2), criterion 7 (recovery -> frames
    // flow again) and criterion 8 (changes AND restarts both move) in one
    // remove -> recover -> re-add sequence, driven through Route B (the
    // enumeration-diff fallback).
    EngineConfig cfg = make_config();
    cfg.video_capture_name = "change-detect-capture";
    cfg.video_content_hint = np::ContentHint::kText;
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The first poll seeds the snapshot; it is not a change.
    engine.tick();
    EXPECT_EQ(engine.stats().video_capture_device_changes, 0u);
    EXPECT_EQ(engine.stats().video_capture_restarts, 0u);

    auto* dev = dynamic_cast<FakeCaptureDevice*>(engine.video_capture());
    ASSERT_NE(dev, nullptr);

    // ---- 1. Remove the device ---------------------------------------------
    dev->set_present(false);
    std::this_thread::sleep_for(510ms);  // past the 500 ms poll throttle
    engine.tick();                       // the diff sees the removal
    EXPECT_EQ(engine.stats().video_capture_device_changes, 1u);

    // ---- 2. The first reopen attempt fails (device still gone) -----------
    std::this_thread::sleep_for(110ms);  // past the 100 ms initial backoff
    engine.tick();
    EXPECT_EQ(engine.stats().video_capture_device_changes, 1u)
        << "a device that stays removed must not be counted again while "
           "gone";
    EXPECT_EQ(engine.stats().video_capture_restarts, 0u);

    // ---- 3. The device returns; the backoff reopen succeeds --------------
    dev->set_present(true);
    std::this_thread::sleep_for(210ms);  // past the 200 ms second backoff
    engine.tick();
    EXPECT_EQ(engine.stats().video_capture_restarts, 1u);
    EXPECT_TRUE(dev->is_open());
    np::CaptureFrame frame{};
    EXPECT_EQ(dev->pop_frame(frame), np::kOk)
        << "after recovery the device must deliver frames again";

    // ---- 4. The re-add is counted by a later poll -------------------------
    std::this_thread::sleep_for(510ms);
    engine.tick();
    EXPECT_EQ(engine.stats().video_capture_device_changes, 2u);
    EXPECT_EQ(engine.stats().video_capture_restarts, 1u);

    engine.close();
}

TEST_F(EngineDevicePolicy, DeviceEventSourceReportsChangesAndBackoff) {
    // The Route A variant: the device itself reports structured change
    // events through `ICaptureDeviceEventSource` (probed with a
    // `dynamic_cast`), instead of the enumeration diff.
    EngineConfig cfg = make_config();
    cfg.video_capture_name = "event-source-capture";
    cfg.video_content_hint = np::ContentHint::kText;
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    engine.tick();
    EXPECT_EQ(engine.stats().video_capture_device_changes, 0u);

    auto* dev = dynamic_cast<EventSourceCaptureDevice*>(engine.video_capture());
    ASSERT_NE(dev, nullptr);

    // ---- 1. A removal event is counted and enters the backoff ------------
    np::CaptureDeviceChangeEvent removed;
    removed.change     = np::CaptureDeviceChange::kRemoved;
    removed.device_id   = "fake-dev-1";
    removed.device_name = "Fake Camera 1";
    removed.kind        = np::CaptureDeviceKind::kCamera;
    dev->inject_event(removed);
    std::this_thread::sleep_for(510ms);
    engine.tick();
    EXPECT_EQ(engine.stats().video_capture_device_changes, 1u);

    // ---- 2. Reopen fails once, then succeeds when the device returns ----
    dev->set_present(false);
    std::this_thread::sleep_for(110ms);
    engine.tick();
    EXPECT_EQ(engine.stats().video_capture_restarts, 0u);

    dev->set_present(true);
    std::this_thread::sleep_for(210ms);
    engine.tick();
    EXPECT_EQ(engine.stats().video_capture_restarts, 1u);

    // ---- 3. An added event is counted too --------------------------------
    np::CaptureDeviceChangeEvent added;
    added.change     = np::CaptureDeviceChange::kAdded;
    added.device_id   = "fake-dev-2";
    added.device_name = "Fake Camera 2";
    added.kind        = np::CaptureDeviceKind::kCamera;
    dev->inject_event(added);
    std::this_thread::sleep_for(510ms);
    engine.tick();
    EXPECT_EQ(engine.stats().video_capture_device_changes, 2u);

    engine.close();
}

}  // namespace
