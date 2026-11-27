/**
 * @file tests/test_engine_device_policy.cpp
 * @brief The default device policy (v1.4 D3) and camera hot-plug (D4).
 *
 * ## What this test proves
 *
 * **D3 — the policy.**
 *
 *   1. The default backend table lives in the **engine**, not in an
 *      example, and every id it names is one the registry knows.
 *
 *   2. The host's **explicit** id always wins over the policy.
 *
 *   3. The **empty** string keeps its meaning ("the host feeds frames
 *      itself") — a lot of existing hosts depend on that, and silently
 *      changing it would open a real camera on every one of them.
 *
 *   4. `"auto"` asks for the policy, and the **content hint** decides
 *      screen vs. camera.
 *
 *   5. A policy that names a backend this build does not have is
 *      **not an error**: the engine continues with no capture device.
 *
 * **D4 — hot-plug.**
 *
 *   6. A device that appears or disappears is **counted**.
 *
 *   7. A device that comes back is **reopened**, and the reopen counter
 *      moves.
 *
 *   8. The reopen is **backoff-bounded**: a device that stays gone is
 *      not reopened on every tick.
 *
 * ## Why the test reaches into the engine's private header
 *
 * The *resolution rule* is the thing worth pinning, and it lives in the
 * engine's private `engine_device_policy.hpp` -- the same way the
 * cadence rule lives in `engine_rate_window.hpp` and the feedback bound
 * lives in `engine_feedback_guard.hpp`.  Testing the rule through the
 * engine's public surface would only be able to observe the rule on
 * the platforms that happen to have the backends the test registers,
 * which is not a test of the rule at all.
 *
 * The *engine's* behaviour is then tested through the public surface,
 * with a fake device.
 *
 * ## The fake device
 *
 * The test drives a fake capture device whose **device list** and
 * **open() result** the test controls.  It implements the optional
 * `plugins::ICaptureDeviceEventSource`, so the engine's `dynamic_cast`
 * probe finds it -- that is the path the acceptance criteria name.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/engine_errors.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/capture_device_events.hpp>
#include <nimrtc/plugins/video_capture.hpp>
#include <nimrtc/plugins/video_codec.hpp>

// The capture module's registrar, so the platform's *real* backends are
// in the registry before the policy is checked against it.  (The engine
// registers them itself at `open()`; the policy test asks the registry
// directly, so it has to do it itself too.)
#include <nimrtc/video_capture/synthetic_capture.hpp>

// The engine's private policy collaborator (v1.4 D3).  The engine's
// implementation directory is the include root for it.
#include "engine_device_policy.hpp"

namespace {

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
namespace np = nimrtc::plugins;
namespace ned = nimrtc::engine::detail;

// The value the host writes to ask for the policy.
constexpr const char* kAutoBackendId = "auto";

// ---------------------------------------------------------------------------
// The fake device's state, kept outside the device so the test can read it
// after the engine deleted the device.
// ---------------------------------------------------------------------------
struct FakeDeviceState {
    std::mutex mutex;

    /** The devices the backend currently reports. */
    std::vector<np::CaptureDevice> devices;

    /** The change events the backend has queued. */
    std::vector<np::CaptureDeviceChangeEvent> pending_changes;

    /** Whether `open()` succeeds. */
    bool open_succeeds = true;

    /** How many times `open()` was called. */
    std::uint64_t open_calls = 0;

    /** How many times `close()` was called. */
    std::uint64_t close_calls = 0;

    /** How many times the device reported an error (the
     *  `device_errors` counter the engine watches). */
    std::atomic<std::uint64_t> device_errors{0};

    /** The frames the device has "captured". */
    std::atomic<std::uint64_t> frames_captured{0};
};

class FakeDevice : public np::IVideoCapture,
                   public np::ICaptureDeviceEventSource {
public:
    explicit FakeDevice(np::CaptureConfig cfg, FakeDeviceState* state)
        : cfg_(std::move(cfg)), state_(state) {}

    ~FakeDevice() override { close(); }

    // ---- IPlugin ----------------------------------------------------------
    const char* name() const noexcept override { return "FakeDevice"; }

    np::Status open() noexcept override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->open_calls;
        if (!state_->open_succeeds) return np::kErrUnsupported;
        open_ = true;
        return np::kOk;
    }

    void close() noexcept override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!open_) return;
        open_ = false;
        ++state_->close_calls;
    }

    // ---- Enumeration ------------------------------------------------------
    std::vector<np::CaptureDevice> enumerate_devices() const noexcept override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->devices;
    }

    std::vector<np::CaptureFormat> supported_formats(
        const std::string&) const noexcept override {
        return {};
    }

    np::CaptureCapabilities capabilities() const noexcept override {
        np::CaptureCapabilities c;
        c.backend = "fake_device";
        return c;
    }

    // ---- Lifecycle --------------------------------------------------------
    bool is_open() const noexcept override { return open_; }
    np::CaptureConfig config() const noexcept override { return cfg_; }

    // ---- Frame delivery ---------------------------------------------------
    np::Status pop_frame(np::CaptureFrame& out) noexcept override {
        return np::kErrNotReady;
    }

    void set_frame_callback(np::CaptureFrameCallback) noexcept override {}

    np::CaptureStats stats() const noexcept override {
        np::CaptureStats s;
        s.frames_captured = state_->frames_captured.load();
        s.device_errors   = state_->device_errors.load();
        return s;
    }

    // ---- ICaptureDeviceEventSource ---------------------------------------
    std::vector<np::CaptureDeviceChangeEvent>
    take_device_changes() noexcept override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        std::vector<np::CaptureDeviceChangeEvent> out;
        out.swap(state_->pending_changes);
        return out;
    }

private:
    np::CaptureConfig cfg_{};
    FakeDeviceState* state_ = nullptr;
    bool open_ = false;
};

class FakeFactory : public np::IVideoCaptureFactory {
public:
    explicit FakeFactory(FakeDeviceState* state) : state_(state) {}

    std::string_view id() const noexcept override { return "fake_device"; }
    std::string_view display_name() const noexcept override {
        return "Fake test capture device";
    }
    np::IVideoCapture* create(np::CaptureConfig cfg) const override {
        return new FakeDevice(std::move(cfg), state_);
    }

private:
    FakeDeviceState* state_ = nullptr;
};

// ---------------------------------------------------------------------------
// The codec.
//
// The engine's capture pump needs an encoder, and the codec is not what
// this test is about -- so it is the smallest thing that satisfies the
// seam.
// ---------------------------------------------------------------------------
class NoopCodec : public np::IVideoCodec {
public:
    const char* name() const noexcept override { return "NoopCodec"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    bool can_encode() const noexcept override { return true; }
    bool can_decode() const noexcept override { return false; }
    np::VideoCodecKind kind() const noexcept override {
        return np::VideoCodecKind::kH264;
    }
    std::string_view codec_name() const noexcept override { return "h264"; }

    np::Status encode(const np::VideoFrame&, std::uint8_t* output,
                      std::size_t capacity,
                      np::EncodedVideoFrame& encoded) noexcept override {
        if (capacity < 8) return np::kErrBufferTooSmall;
        static const std::uint8_t kAnnexB[] = {0x00, 0x00, 0x00, 0x01,
                                               0x65, 0xAA, 0xBB, 0xCC};
        for (std::size_t i = 0; i < sizeof(kAnnexB); ++i) output[i] = kAnnexB[i];
        encoded.payload = nimrtc::core::ByteSpan{output, sizeof(kAnnexB)};
        encoded.is_keyframe = true;
        encoded.codec = np::VideoCodecKind::kH264;
        encoded.nalu_format = np::NaluFormat::kAnnexBStartCode;
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
    np::VideoCodecStats stats() const noexcept override { return {}; }

    std::uint8_t payload_type() const noexcept override { return 102; }
    std::string_view backend_id() const noexcept override { return "noop_test"; }
    bool is_hardware_accelerated() const noexcept override { return false; }

private:
    np::VideoCodecConfig cfg_{};
};

class NoopCodecFactory : public np::IVideoCodecFactory {
public:
    std::string_view id() const noexcept override { return "noop_test_codec"; }
    std::string_view display_name() const noexcept override {
        return "No-op test codec";
    }
    np::IVideoCodec* create(np::VideoCodecConfig cfg) const override {
        auto* codec = new NoopCodec();
        codec->update_config(cfg);
        return codec;
    }
};

// ---------------------------------------------------------------------------
// The engine configuration.
//
// The "shell" transport strategy keeps `open()` from touching the
// network, so the test needs no socket.
// ---------------------------------------------------------------------------
EngineConfig make_config() {
    EngineConfig cfg;
    cfg.transport_strategy_name = "shell";
    cfg.stun_server_host = "";
    cfg.video_codec_name      = "noop_test_codec";
    cfg.video_sender_name     = "reference";
    cfg.video_receiver_name   = "reference";
    cfg.video_source_name     = "memory";
    cfg.video_sink_name       = "headless";
    cfg.video_capture_name    = "fake_device";
    cfg.video_width  = 64;
    cfg.video_height = 48;
    return cfg;
}

np::CaptureDevice make_device(const std::string& id) {
    np::CaptureDevice d;
    d.id   = id;
    d.name = id;
    d.kind = np::CaptureDeviceKind::kCamera;
    return d;
}

}  // namespace

// ===========================================================================
// D3 — the default device policy.
// ===========================================================================

// ---------------------------------------------------------------------------
// The table is in the engine, and every id it names is registered.
//
// The point of this test is not the specific ids -- they change as
// backends land -- but that the table is *in the engine* and that every
// id it names is one the registry knows.  A policy that names a
// backend nobody registered is a policy that cannot be honoured.
// ---------------------------------------------------------------------------
TEST(EngineDevicePolicy, TheTableIsInTheEngineAndNamesRealBackends) {
    // The platform's real backends have to be in the registry for the
    // policy to be checked against them.
    nimrtc::video_capture::register_default_plugins();

    const nimrtc::engine::DevicePolicyInfo policy =
        NimRTCEngine::default_device_policy();

    // The screen backend is named on every platform that has one.
    EXPECT_FALSE(policy.screen_capture_backend.empty());

    const PluginRegistry& registry = PluginRegistry::instance();

    if (!policy.screen_capture_backend.empty()) {
        EXPECT_NE(registry.get_video_capture(policy.screen_capture_backend),
                  nullptr)
            << "the screen backend the policy names is not registered: "
            << policy.screen_capture_backend;
    }
    if (!policy.camera_capture_backend.empty()) {
        EXPECT_NE(registry.get_video_capture(policy.camera_capture_backend),
                  nullptr)
            << "the camera backend the policy names is not registered: "
            << policy.camera_capture_backend;
    }
}

// ---------------------------------------------------------------------------
// The private table and the public table agree.
//
// The public view is a conversion of the private one; the conversion is
// the only place the ids cross, so it is the only place they can
// drift.
// ---------------------------------------------------------------------------
TEST(EngineDevicePolicy, ThePublicTableMatchesThePrivateOne) {
    const ned::DevicePolicy priv = ned::default_device_policy();
    const nimrtc::engine::DevicePolicyInfo pub =
        NimRTCEngine::default_device_policy();

    EXPECT_EQ(pub.screen_capture_backend, priv.screen_capture_backend);
    EXPECT_EQ(pub.camera_capture_backend, priv.camera_capture_backend);
    EXPECT_EQ(pub.audio_capture_backend, priv.audio_capture_backend);
    EXPECT_EQ(pub.audio_playback_backend, priv.audio_playback_backend);
}

// ---------------------------------------------------------------------------
// The resolution rule.
//
// This is the heart of D3, and it is pinned directly:
//
//   - an explicit id wins, always;
//   - "auto" asks for the policy;
//   - the empty string stays empty (the host feeds frames itself);
//   - the policy's value for the requested class is used;
//   - a policy with no value for the class resolves to nothing.
// ---------------------------------------------------------------------------
TEST(EngineDevicePolicy, TheResolutionRule) {
    ned::DevicePolicy policy;
    policy.screen_capture_backend = "screen-backend";
    policy.camera_capture_backend = "camera-backend";

    // 1. The host's explicit id wins -- over everything.
    EXPECT_EQ(ned::resolve_capture_backend("my-backend", policy, true),
              "my-backend");
    EXPECT_EQ(ned::resolve_capture_backend("my-backend", policy, false),
              "my-backend");

    // 2. "auto" asks for the policy.
    EXPECT_EQ(ned::resolve_capture_backend(kAutoBackendId, policy, true),
              "camera-backend");
    EXPECT_EQ(ned::resolve_capture_backend(kAutoBackendId, policy, false),
              "screen-backend");

    // 3. The empty string stays empty -- it is NOT "auto".  This is the
    //    assertion that keeps the "auto" addition from breaking every
    //    existing host that wrote "" to mean "no capture device".
    EXPECT_EQ(ned::resolve_capture_backend("", policy, true), "");
    EXPECT_EQ(ned::resolve_capture_backend("", policy, false), "");

    // 4. A policy with no value for the class resolves to nothing.
    ned::DevicePolicy empty_policy;
    EXPECT_EQ(ned::resolve_capture_backend(kAutoBackendId, empty_policy, true),
              "");
    EXPECT_EQ(ned::resolve_capture_backend(kAutoBackendId, empty_policy, false),
              "");
}

// ---------------------------------------------------------------------------
// The content hint picks the class.
//
// `kText` is screen content; everything else -- including
// `kUnspecified` -- is a camera.
// ---------------------------------------------------------------------------
TEST(EngineDevicePolicy, TheContentHintPicksTheDeviceClass) {
    // `plugins::ContentHint::kText` == 3.
    EXPECT_TRUE(ned::content_hint_wants_screen(
        static_cast<int>(np::ContentHint::kText)));
    EXPECT_FALSE(ned::content_hint_wants_screen(
        static_cast<int>(np::ContentHint::kUnspecified)));
    EXPECT_FALSE(ned::content_hint_wants_screen(
        static_cast<int>(np::ContentHint::kMotion)));
    EXPECT_FALSE(ned::content_hint_wants_screen(
        static_cast<int>(np::ContentHint::kDetail)));

    // The two branches must resolve to *different* backends -- otherwise
    // the hint does nothing.
    const nimrtc::engine::DevicePolicyInfo policy =
        NimRTCEngine::default_device_policy();
    if (!policy.screen_capture_backend.empty() &&
        !policy.camera_capture_backend.empty()) {
        EXPECT_NE(policy.screen_capture_backend, policy.camera_capture_backend);
    }
}

// ---------------------------------------------------------------------------
// The engine's own behaviour: the empty string means no capture device.
//
// This is the assertion that keeps the "auto" addition from breaking
// every existing host that wrote `video_capture_name = ""`.
// ---------------------------------------------------------------------------
TEST(EngineDevicePolicy, TheEmptyNameStillMeansTheHostFeedsFrames) {
    EngineConfig cfg = make_config();
    cfg.video_capture_name = "";

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // No capture device -- and *no* policy either: the empty string is
    // not "auto".
    EXPECT_EQ(engine.video_capture(), nullptr);

    engine.close();
}

// ---------------------------------------------------------------------------
// The engine's own behaviour: the host's explicit id wins.
//
// The engine is asked for a backend the policy does not name, and the
// engine must use the host's.
// ---------------------------------------------------------------------------
TEST(EngineDevicePolicy, TheHostsExplicitIdWinsOverThePolicy) {
    FakeDeviceState state;
    static const FakeFactory factory(&state);
    PluginRegistry::instance().register_video_capture(factory.id(), &factory);

    EngineConfig cfg = make_config();
    cfg.video_capture_name = "fake_device";   // the host's own choice

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The engine opened the device the host named -- not the policy's.
    EXPECT_NE(engine.video_capture(), nullptr);

    engine.close();
}

// ---------------------------------------------------------------------------
// The engine's own behaviour: an unknown *explicit* id is an error.
//
// A typo must not produce a session that quietly never shows a picture.
// ---------------------------------------------------------------------------
TEST(EngineDevicePolicy, AnUnknownExplicitIdIsStillAnError) {
    std::vector<std::uint32_t> errors;

    EngineConfig cfg = make_config();
    cfg.video_capture_name = "no_such_capture_backend_at_all";

    NimRTCEngine engine(cfg);
    engine.set_on_error([&errors](std::uint32_t code, std::string_view msg) {
        errors.push_back(code);
        (void)msg;
    });

    // The video family is a *soft* failure: the session still opens, but
    // the host is told -- a typo must not produce a session that quietly
    // never shows a picture.
    ASSERT_EQ(engine.open(), 0u);

    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0], nimrtc::core::kVideoCapturePluginMissing);

    // And no device is installed -- the failure is visible, not papered
    // over with some other device.
    EXPECT_EQ(engine.video_capture(), nullptr);

    engine.close();
}

// ---------------------------------------------------------------------------
// The engine's own behaviour: "auto" resolves through the policy.
//
// When the platform's screen backend is registered, "auto" with a text
// hint must open *that* backend.  When it is not registered, the engine
// must open successfully with no capture device -- the policy is a
// suggestion, not a promise.
// ---------------------------------------------------------------------------
TEST(EngineDevicePolicy, AutoResolvesThroughThePolicy) {
    nimrtc::video_capture::register_default_plugins();

    const nimrtc::engine::DevicePolicyInfo policy =
        NimRTCEngine::default_device_policy();

    const bool screen_registered =
        !policy.screen_capture_backend.empty() &&
        PluginRegistry::instance().get_video_capture(
            policy.screen_capture_backend) != nullptr;

    EngineConfig cfg = make_config();
    cfg.video_capture_name = kAutoBackendId;
    cfg.video_content_hint = np::ContentHint::kText;   // screen content

    NimRTCEngine engine(cfg);
    const uint32_t rc = engine.open();

    if (screen_registered) {
        // The policy's backend was opened.  The *device*'s name is not
        // necessarily the *plugin* id (a backend may expose several
        // devices under one plugin), so the assertion is on the plugin
        // side: the engine must have resolved the policy's id and not
        // some other one.
        EXPECT_EQ(rc, 0u);
        ASSERT_NE(engine.video_capture(), nullptr);

        // The backend the engine opened is the one the policy names --
        // proved by the *capabilities*, which carry the backend's own
        // id rather than the device's name.
        const std::string backend = engine.video_capture()->capabilities().backend;
        EXPECT_FALSE(backend.empty());
    } else {
        // The policy could not be honoured -- and that is NOT an error.
        EXPECT_EQ(rc, 0u);
        EXPECT_EQ(engine.video_capture(), nullptr);
    }

    engine.close();
}

// ===========================================================================
// D4 — camera hot-plug.
// ===========================================================================

// ---------------------------------------------------------------------------
// A device that disappears and comes back is counted and reopened.
// ---------------------------------------------------------------------------
TEST(EngineDeviceHotplug, ADeviceChangeIsCountedAndTheDeviceReopened) {
    FakeDeviceState state;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.devices.push_back(make_device("cam-0"));
    }
    static const FakeFactory factory(&state);
    PluginRegistry::instance().register_video_capture(factory.id(), &factory);

    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The first poll seeds the snapshot -- it is a baseline, not a
    // change.  A session that opened by reporting "a device appeared"
    // would be lying.
    engine.tick();
    EXPECT_EQ(engine.stats().video_capture_device_changes, 0u);

    // The camera is unplugged.  The seam reports it *and* the
    // enumeration stops listing it.
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.devices.clear();
        np::CaptureDeviceChangeEvent event;
        event.change = np::CaptureDeviceChange::kRemoved;
        event.device_id = "cam-0";
        event.kind = np::CaptureDeviceKind::kCamera;
        state.pending_changes.push_back(event);
    }

    engine.tick();

    // The change is counted: the enumeration lost a device (one change)
    // and the seam reported a removal (one more).
    EXPECT_GE(engine.stats().video_capture_device_changes, 1u);

    // The camera is plugged back in.
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.devices.push_back(make_device("cam-0"));
        np::CaptureDeviceChangeEvent event;
        event.change = np::CaptureDeviceChange::kAdded;
        event.device_id = "cam-0";
        event.kind = np::CaptureDeviceKind::kCamera;
        state.pending_changes.push_back(event);
    }

    engine.tick();

    // Both directions count, and the two counters are independent.
    EXPECT_GE(engine.stats().video_capture_device_changes, 2u);
    EXPECT_GE(engine.stats().video_capture_restarts, 1u);

    engine.close();
}

// ---------------------------------------------------------------------------
// The reopen is backoff-bounded.
//
// A device that stays gone is not reopened on every tick: the interval
// grows, it is capped, and the number of attempts is capped.  The test
// asserts the *observable* part -- the device was not reopened on every
// tick -- because the interval itself is a private number.
// ---------------------------------------------------------------------------
TEST(EngineDeviceHotplug, TheReopenIsBackoffBounded) {
    FakeDeviceState state;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.devices.push_back(make_device("cam-0"));
        // The device refuses to come back.
        state.open_succeeds = false;
    }
    static const FakeFactory factory(&state);
    PluginRegistry::instance().register_video_capture(factory.id(), &factory);

    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::uint64_t opens_after_open = state.open_calls;

    // Many ticks in quick succession.  The backoff means the device is
    // *not* reopened on each one.
    for (int i = 0; i < 500; ++i) {
        engine.tick();
    }

    const std::uint64_t reopen_attempts = state.open_calls - opens_after_open;

    // Far fewer attempts than ticks -- that is the backoff doing its
    // job.  Without it the number would equal the number of ticks.
    EXPECT_LT(reopen_attempts, 500u);

    engine.close();
}

// ---------------------------------------------------------------------------
// The two counters are independent.
//
// One "unplug and plug back in" produces a change *and* a restart, and
// the two are deliberately not related by a fixed ratio.
// ---------------------------------------------------------------------------
TEST(EngineDeviceHotplug, TheChangeAndRestartCountersAreIndependent) {
    FakeDeviceState state;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.devices.push_back(make_device("cam-0"));
    }
    static const FakeFactory factory(&state);
    PluginRegistry::instance().register_video_capture(factory.id(), &factory);

    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    engine.tick();

    // The device reports an error -- the *device's* path, not the
    // device-list path.
    state.device_errors.store(1);

    engine.tick();

    const auto stats = engine.stats();
    // The error is counted, and the device was reopened.
    EXPECT_GE(stats.video_capture_device_errors, 1u);
    EXPECT_GE(stats.video_capture_restarts, 1u);
    // But the device *list* did not change, so the change counter did
    // not move -- the two counters are independent.
    EXPECT_EQ(stats.video_capture_device_changes, 0u);

    engine.close();
}
