// tests/test_engine_video_capture.cpp
// ----------------------------------------------------------------------------
// The capture-*device* seam wiring (remote-desktop plan A1, architecture
// remediation plan Phase 2 §2.2).
//
// `EngineConfig::video_capture_name` was the one `*_name` field the
// engine never read: the registry slot existed, the plugin interface
// existed, the synthetic device existed — but nothing ever resolved the
// id, so naming a capture device did nothing at all.  That is the
// "a knob that is never read is a lie" defect the remediation plan
// targets.
//
// The test pins the three cases that matter:
//
//   1. An empty name installs no device (the normal case — the host
//      feeds frames itself).
//   2. A registered id is resolved, the device is created AND opened,
//      and the engine hands it to the host through video_capture().
//   3. An unknown id is reported loudly through on_error() rather than
//      silently downgraded to "no capture device" — a typo must not
//      turn into a session that quietly never produces a frame.
// ----------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <vector>

#include <nimrtc/core/engine_errors.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/video_capture.hpp>

#include <gtest/gtest.h>

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
namespace np = nimrtc::plugins;

namespace {

// ---------------------------------------------------------------------------
// A stub capture device.
//
// It records what the engine did to it, so the test can prove the device
// was really created and opened — not merely that a pointer came back.
// ---------------------------------------------------------------------------
struct CaptureCalls {
    int created = 0;
    int opened = 0;
    int closed = 0;
};

class StubCaptureDevice : public np::IVideoCapture {
public:
    explicit StubCaptureDevice(np::CaptureConfig cfg, CaptureCalls* calls) noexcept
        : cfg_(std::move(cfg)), calls_(calls) {
        if (calls_) ++calls_->created;
    }
    ~StubCaptureDevice() override = default;

    // ---- IPlugin ----------------------------------------------------------
    const char* name() const noexcept override { return "StubCaptureDevice"; }
    np::Status open() noexcept override {
        if (calls_) ++calls_->opened;
        opened_ = true;
        // A capture device reports the layout it negotiated.  The
        // engine's capture path hands the device to the capture
        // module's shared adapter, and the adapter refuses to drive a
        // device whose pixel layout it cannot name -- guessing a
        // layout would produce garbage pixels instead of an error.
        // Every real backend (DXGI, WGC, ScreenCaptureKit, PipeWire,
        // X11) reports "bgra"; so does this stub.
        cfg_.format.width = 320;
        cfg_.format.height = 240;
        cfg_.format.fps = 30;
        cfg_.format.tag = "bgra";
        return np::kOk;
    }
    void close() noexcept override {
        if (calls_) ++calls_->closed;
        opened_ = false;
    }

    // ---- Enumeration ------------------------------------------------------
    std::vector<np::CaptureDevice> enumerate_devices() const noexcept override {
        return {};
    }
    std::vector<np::CaptureFormat> supported_formats(
        const std::string& /*device_id*/) const noexcept override {
        return {};
    }
    np::CaptureCapabilities capabilities() const noexcept override {
        return {};
    }

    // ---- Lifecycle --------------------------------------------------------
    bool is_open() const noexcept override { return opened_; }
    np::CaptureConfig config() const noexcept override { return cfg_; }

    // ---- Frame delivery ---------------------------------------------------
    np::Status pop_frame(np::CaptureFrame& /*out*/) noexcept override {
        return np::kErrNotReady;
    }
    void set_frame_callback(np::CaptureFrameCallback /*cb*/) noexcept override {}
    np::CaptureStats stats() const noexcept override { return {}; }

private:
    np::CaptureConfig cfg_{};
    CaptureCalls* calls_ = nullptr;
    bool opened_ = false;
};

class StubCaptureFactory : public np::IVideoCaptureFactory {
public:
    explicit StubCaptureFactory(CaptureCalls* calls) noexcept : calls_(calls) {}

    std::string_view id() const noexcept override { return "stub_capture"; }
    std::string_view display_name() const noexcept override {
        return "Stub capture device (test)";
    }
    np::IVideoCapture* create(np::CaptureConfig cfg) const override {
        return new StubCaptureDevice(std::move(cfg), calls_);
    }

private:
    CaptureCalls* calls_ = nullptr;
};

// The engine's own video pipeline is not exercised here: the test is
// about the *seam*, not about encoding.  The "shell" transport strategy
// keeps open() from touching the network, so the test needs no socket.
EngineConfig make_config() {
    EngineConfig cfg;
    cfg.transport_strategy_name = "shell";
    cfg.stun_server_host = "";
    return cfg;
}

}  // namespace

// ----------------------------------------------------------------------------
// 1. The default (empty name) installs no device.
// ----------------------------------------------------------------------------
TEST(EngineVideoCapture, EmptyNameInstallsNoDevice) {
    EngineConfig cfg = make_config();
    ASSERT_TRUE(cfg.video_capture_name.empty());

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The normal case: the host feeds frames itself, so there is no
    // capture device to hand out.
    EXPECT_EQ(engine.video_capture(), nullptr);
}

// ----------------------------------------------------------------------------
// 2. A registered id is resolved, created and opened.
// ----------------------------------------------------------------------------
TEST(EngineVideoCapture, RegisteredIdIsResolvedAndOpened) {
    CaptureCalls calls;
    static const StubCaptureFactory factory(&calls);
    PluginRegistry::instance().register_video_capture(factory.id(), &factory);

    EngineConfig cfg = make_config();
    cfg.video_capture_name = "stub_capture";

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The engine really resolved the id and drove the device.
    EXPECT_EQ(calls.created, 1);
    EXPECT_EQ(calls.opened, 1);

    // And the host can reach it.
    ASSERT_NE(engine.video_capture(), nullptr);
    EXPECT_EQ(std::string{engine.video_capture()->name()}, "StubCaptureDevice");
    EXPECT_TRUE(engine.video_capture()->is_open());
}

// ----------------------------------------------------------------------------
// 3. An unknown id is reported loudly, not silently ignored.
// ----------------------------------------------------------------------------
TEST(EngineVideoCapture, UnknownIdFiresOnError) {
    std::vector<std::uint32_t> errors;

    EngineConfig cfg = make_config();
    cfg.video_capture_name = "no_such_capture_device";

    NimRTCEngine engine(cfg);
    engine.set_on_error([&errors](std::uint32_t code, std::string_view msg) {
        errors.push_back(code);
        (void)msg;
    });
    // The video family is a *soft* failure (the session still opens, the
    // capture feature is inert), but it is NOT silent: the host is told.
    ASSERT_EQ(engine.open(), 0u);

    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0], nimrtc::core::kVideoCapturePluginMissing);

    // And no device is installed — the failure is visible, not papered
    // over with some other device.
    EXPECT_EQ(engine.video_capture(), nullptr);
}

// ----------------------------------------------------------------------------
// 4. The device is closed when the engine closes.
// ----------------------------------------------------------------------------
TEST(EngineVideoCapture, DeviceIsClosedOnEngineClose) {
    CaptureCalls calls;
    static const StubCaptureFactory factory(&calls);
    PluginRegistry::instance().register_video_capture(factory.id(), &factory);

    EngineConfig cfg = make_config();
    cfg.video_capture_name = "stub_capture";

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_EQ(calls.closed, 0);

    engine.close();
    EXPECT_EQ(calls.closed, 1);
}
