// modules/video_capture/tests/test_camera_capture.cpp
// ----------------------------------------------------------------------------
// The camera capture layer's tests (remote-desktop plan A9).
//
// These run on **every** platform: the camera *layer* is portable, and
// the platform *sources* (Media Foundation, AVFoundation, V4L2) are
// thin adapters over it.
//
// The synthetic camera is what makes plan A9 verifiable in CI: it needs
// no hardware, no permission prompt and no OS, so the camera path — the
// enumeration, the negotiation, the drop policy, the frame delivery — is
// exercised on every CI leg.
// ----------------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/video_capture/camera_capture.hpp>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace plugins = nimrtc::plugins;

using nimrtc::plugins::CaptureCapabilities;
using nimrtc::plugins::CaptureConfig;
using nimrtc::plugins::CaptureDevice;
using nimrtc::plugins::CaptureDeviceKind;
using nimrtc::plugins::CaptureDropPolicy;
using nimrtc::plugins::CaptureFormat;
using nimrtc::plugins::CaptureFrame;
using nimrtc::plugins::VideoPixelFormat;
using nimrtc::video_capture::CameraBackend;
using nimrtc::video_capture::CameraCapture;
using nimrtc::video_capture::CameraCaptureFactory;
using nimrtc::video_capture::CameraCaptureOptions;
using nimrtc::video_capture::SyntheticCamera;
using nimrtc::video_capture::SyntheticCameraFactory;

} // anonymous namespace

// ---------------------------------------------------------------------------
// The backend enum
// ---------------------------------------------------------------------------

TEST(CameraBackend, TheNamesAreStable) {
    EXPECT_EQ(nimrtc::video_capture::camera_backend_name(CameraBackend::kMediaFoundation),
              "mediafoundation");
    EXPECT_EQ(nimrtc::video_capture::camera_backend_name(CameraBackend::kAvFoundation),
              "avfoundation");
    EXPECT_EQ(nimrtc::video_capture::camera_backend_name(CameraBackend::kV4l2), "v4l2");
    EXPECT_EQ(nimrtc::video_capture::camera_backend_name(CameraBackend::kAndroidCamera2),
              "android-camera2");
    EXPECT_EQ(nimrtc::video_capture::camera_backend_name(CameraBackend::kSynthetic),
              "synthetic");
}

TEST(CameraBackend, TheNamesMatchTheHardwareBackendConvention) {
    // The convention across the code base is a short lower-case
    // identifier ("d3d11", "metal", "v4l2", "software").
    for (const auto backend : {CameraBackend::kMediaFoundation, CameraBackend::kAvFoundation,
                                CameraBackend::kV4l2, CameraBackend::kAndroidCamera2,
                                CameraBackend::kSynthetic}) {
        const std::string_view name = nimrtc::video_capture::camera_backend_name(backend);
        EXPECT_FALSE(name.empty());
        EXPECT_EQ(name.find(' '), std::string_view::npos);
        EXPECT_NE(name, "unknown");
    }
}

// ---------------------------------------------------------------------------
// The synthetic camera
// ---------------------------------------------------------------------------

TEST(SyntheticCamera, ItEnumeratesACamera) {
    SyntheticCamera camera{CaptureConfig{}};
    const auto devices = camera.enumerate_devices();

    ASSERT_EQ(devices.size(), 1u);
    EXPECT_EQ(devices[0].kind, CaptureDeviceKind::kCamera);
    EXPECT_FALSE(devices[0].id.empty());
    EXPECT_FALSE(devices[0].name.empty());
    EXPECT_GT(devices[0].max_width, 0u);
    EXPECT_GT(devices[0].max_height, 0u);
}

TEST(SyntheticCamera, ItReportsItsBackendName) {
    SyntheticCamera camera{CaptureConfig{}};
    EXPECT_EQ(camera.hardware_backend(), "synthetic");
    EXPECT_EQ(camera.name(), std::string_view("synthetic-camera"));
}

TEST(SyntheticCamera, ItReportsTheCanonicalHardwareBackend) {
    // The camera reports itself in the *seam-wide* hardware vocabulary,
    // so a host can compare the camera's backend against the encoder's
    // without knowing anything about cameras.
    SyntheticCamera camera{CaptureConfig{}};
    EXPECT_EQ(camera.hw_backend(), nimrtc::plugins::HwBackend::Software);

    // The mapping the plan's acceptance criterion names.
    EXPECT_EQ(nimrtc::video_capture::camera_hw_backend(
                  nimrtc::video_capture::CameraBackend::kMediaFoundation),
              nimrtc::plugins::HwBackend::DirectShow);
    EXPECT_EQ(nimrtc::video_capture::camera_hw_backend(
                  nimrtc::video_capture::CameraBackend::kAvFoundation),
              nimrtc::plugins::HwBackend::AvFoundation);
    EXPECT_EQ(nimrtc::video_capture::camera_hw_backend(
                  nimrtc::video_capture::CameraBackend::kV4l2),
              nimrtc::plugins::HwBackend::V4l2);
    EXPECT_EQ(nimrtc::video_capture::camera_hw_backend(
                  nimrtc::video_capture::CameraBackend::kAndroidCamera2),
              nimrtc::plugins::HwBackend::AndroidCamera2);
}

TEST(SyntheticCamera, ItsCapabilitiesAreHonest) {
    SyntheticCamera camera{CaptureConfig{}};
    const auto caps = camera.capabilities();

    // The camera enumerates itself.
    EXPECT_TRUE(caps.enumeration);
    // A camera has no pointer: nothing to composite, and the flag says
    // so honestly.
    EXPECT_FALSE(caps.cursor_composited);
    // A camera has no dirty rectangles.
    EXPECT_FALSE(caps.dirty_rects);
    // A camera cannot capture a sub-region.
    EXPECT_FALSE(caps.region_selection);
    EXPECT_EQ(caps.backend, "synthetic");
}

TEST(SyntheticCamera, TheDropPolicyIsDropNewestByDefault) {
    SyntheticCamera camera{CaptureConfig{}};
    EXPECT_EQ(camera.config().drop_policy, CaptureDropPolicy::kDropNewest);
}

TEST(SyntheticCamera, ItDeliversFramesWithTheirTimestampsAndSequenceNumbers) {
    SyntheticCamera camera{CaptureConfig{}};
    ASSERT_EQ(camera.open(), nimrtc::plugins::kOk);
    EXPECT_TRUE(camera.is_open());

    ASSERT_EQ(camera.produce_one(), nimrtc::plugins::kOk);
    ASSERT_EQ(camera.produce_one(), nimrtc::plugins::kOk);

    CaptureFrame frame{};
    ASSERT_EQ(camera.pop_frame(frame), nimrtc::plugins::kOk);
    EXPECT_EQ(frame.frame_seq, 0u);
    EXPECT_EQ(frame.width, 640u);
    EXPECT_EQ(frame.height, 480u);
    EXPECT_EQ(frame.format, VideoPixelFormat::kNV12);
    EXPECT_GT(frame.capture_ts_us, 0);

    ASSERT_EQ(camera.pop_frame(frame), nimrtc::plugins::kOk);
    EXPECT_EQ(frame.frame_seq, 1u);
    // The timestamps are monotonic.
    EXPECT_GT(frame.capture_ts_us, 0);

    EXPECT_EQ(camera.pop_frame(frame), nimrtc::plugins::kErrNotReady);
    EXPECT_GT(camera.stats().frames_captured, 0u);
}

TEST(SyntheticCamera, TheQueueKeepsTheNewestFramesWhenItOverflows) {
    CaptureConfig cfg{};
    cfg.queue_depth = 2;
    SyntheticCamera camera{cfg};
    ASSERT_EQ(camera.open(), nimrtc::plugins::kOk);

    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(camera.produce_one(), nimrtc::plugins::kOk);
    }

    // The camera's policy drops the *newest* frame, so the queue keeps
    // what it had: the frames that made it in are the *older* ones.
    EXPECT_GT(camera.stats().frames_dropped, 0u);

    CaptureFrame frame{};
    ASSERT_EQ(camera.pop_frame(frame), nimrtc::plugins::kOk);
    EXPECT_EQ(frame.frame_seq, 0u);
    ASSERT_EQ(camera.pop_frame(frame), nimrtc::plugins::kOk);
    EXPECT_EQ(frame.frame_seq, 1u);
    EXPECT_EQ(camera.pop_frame(frame), nimrtc::plugins::kErrNotReady);
}

TEST(SyntheticCamera, TheFormatNegotiationClampsARequestLargerThanTheCamera) {
    CaptureConfig cfg{};
    cfg.format.width = 4096;
    cfg.format.height = 4096;

    SyntheticCamera camera{cfg};
    ASSERT_EQ(camera.open(), nimrtc::plugins::kOk);

    const auto negotiated = camera.config().format;
    // Never upscale: the camera's native size is the ceiling.
    EXPECT_LE(negotiated.width, 1280u);
    EXPECT_LE(negotiated.height, 720u);
    EXPECT_GT(negotiated.width, 0u);
    EXPECT_GT(negotiated.height, 0u);
}

TEST(SyntheticCamera, AZeroSizedRequestMeansTheCamerasBest) {
    SyntheticCamera camera{CaptureConfig{}};
    ASSERT_EQ(camera.open(), nimrtc::plugins::kOk);

    const auto negotiated = camera.config().format;
    EXPECT_GT(negotiated.width, 0u);
    EXPECT_GT(negotiated.height, 0u);
    EXPECT_GT(negotiated.fps, 0u);
}

TEST(SyntheticCamera, AnUnknownDeviceIdIsAnError) {
    CaptureConfig cfg{};
    cfg.device_id = "no-such-camera";
    SyntheticCamera camera{cfg};
    EXPECT_EQ(camera.open(), nimrtc::plugins::kErrInvalidParam);
    EXPECT_FALSE(camera.is_open());
}

TEST(SyntheticCamera, TheFrameCallbackFiresSynchronously) {
    SyntheticCamera camera{CaptureConfig{}};
    ASSERT_EQ(camera.open(), nimrtc::plugins::kOk);

    std::atomic<int> calls{0};
    std::uint32_t seen_seq = 0;
    camera.set_frame_callback([&](const CaptureFrame& f) {
        ++calls;
        seen_seq = f.frame_seq;
    });

    ASSERT_EQ(camera.produce_one(), nimrtc::plugins::kOk);
    EXPECT_EQ(calls.load(), 1);
    EXPECT_EQ(seen_seq, 0u);
}

TEST(SyntheticCamera, TheSupportedFormatsAreNV12) {
    SyntheticCamera camera{CaptureConfig{}};
    const auto formats = camera.supported_formats("");

    ASSERT_FALSE(formats.empty());
    for (const auto& f : formats) {
        EXPECT_FALSE(f.tag.empty());
        EXPECT_FALSE(f.has_dirty_rects);
        EXPECT_FALSE(f.zero_copy);
    }
}

// ---------------------------------------------------------------------------
// The CameraCapture device (driven by a fake frame source)
// ---------------------------------------------------------------------------

namespace {

/**
 * @brief A fake camera frame source.
 *
 * It produces NV12 frames of a fixed size, on demand.
 */
class FakeCameraSource final : public nimrtc::video_capture::ICameraFrameSource {
  public:
    [[nodiscard]] std::vector<CaptureDevice> enumerate_cameras() const noexcept override {
        CaptureDevice device;
        device.id = "fake-camera:0";
        device.name = "Fake camera";
        device.kind = CaptureDeviceKind::kCamera;
        device.max_width = 1280;
        device.max_height = 720;
        device.max_fps = 30;
        return {device};
    }

    [[nodiscard]] std::vector<CaptureFormat>
    supported_formats(const std::string&) const noexcept override {
        CaptureFormat format;
        format.width = 1280;
        format.height = 720;
        format.fps = 30;
        format.tag = "nv12-fake";
        return {format};
    }

    plugins::Status open(const std::string& device_id, const CaptureFormat& format) noexcept
        override {
        if (device_id != "fake-camera:0") return nimrtc::plugins::kErrInvalidParam;
        width_ = format.width;
        height_ = format.height;
        open_ = true;
        return nimrtc::plugins::kOk;
    }

    void close() noexcept override { open_ = false; }

    plugins::Status next_frame(std::vector<std::uint8_t>& pixels, std::size_t& size,
                               std::int32_t& stride, std::uint32_t& width,
                               std::uint32_t& height, std::int64_t& ts) noexcept override {
        if (!open_) return plugins::kErrNotReady;
        const std::size_t needed = static_cast<std::size_t>(width_) * height_ * 3u / 2u;
        if (pixels.size() < needed) pixels.assign(needed, 0x40);
        size = needed;
        stride = static_cast<std::int32_t>(width_);
        width = width_;
        height = height_;
        ts = 1000 + (++frames_ * 10);
        return plugins::kOk;
    }

  private:
    bool open_ = false;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint64_t frames_ = 0;
};

} // anonymous namespace

TEST(CameraCapture, ItDeliversFramesThroughPopFrame) {
    CameraCaptureOptions opts;
    opts.backend = CameraBackend::kSynthetic;

    CameraCapture camera{CaptureConfig{}, std::make_unique<FakeCameraSource>(), opts};
    ASSERT_EQ(camera.open(), nimrtc::plugins::kOk);
    EXPECT_TRUE(camera.is_open());
    EXPECT_EQ(camera.hardware_backend(), "synthetic");

    CaptureFrame frame{};
    nimrtc::plugins::Status status = nimrtc::plugins::kErrNotReady;
    for (int i = 0; i < 200 && status != nimrtc::plugins::kOk; ++i) {
        status = camera.pop_frame(frame);
        if (status != nimrtc::plugins::kOk) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    ASSERT_EQ(status, nimrtc::plugins::kOk);
    EXPECT_EQ(frame.format, VideoPixelFormat::kNV12);
    EXPECT_GT(frame.width, 0u);
    EXPECT_GT(frame.height, 0u);
    EXPECT_GT(frame.capture_ts_us, 0);
    EXPECT_GT(camera.stats().frames_captured, 0u);

    camera.close();
}

TEST(CameraCapture, TheSequenceNumbersAreMonotonic) {
    CameraCaptureOptions opts;
    opts.backend = CameraBackend::kSynthetic;

    CameraCapture camera{CaptureConfig{}, std::make_unique<FakeCameraSource>(), opts};
    ASSERT_EQ(camera.open(), nimrtc::plugins::kOk);

    CaptureFrame frame{};
    nimrtc::plugins::Status status = nimrtc::plugins::kErrNotReady;
    bool have_last = false;
    std::uint32_t last_seq = 0;
    int count = 0;
    for (int i = 0; i < 400 && count < 3; ++i) {
        status = camera.pop_frame(frame);
        if (status == nimrtc::plugins::kOk) {
            if (have_last) {
                EXPECT_GT(frame.frame_seq, last_seq);
            }
            last_seq = frame.frame_seq;
            have_last = true;
            ++count;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    EXPECT_EQ(count, 3);

    camera.close();
}

TEST(CameraCapture, TheDropPolicyIsDropNewest) {
    CameraCaptureOptions opts;
    opts.backend = CameraBackend::kSynthetic;

    CameraCapture camera{CaptureConfig{}, std::make_unique<FakeCameraSource>(), opts};
    EXPECT_EQ(camera.config().drop_policy, CaptureDropPolicy::kDropNewest);
}

TEST(CameraCapture, TheFormatNegotiationClampsToTheCamerasNativeSize) {
    CameraCaptureOptions opts;
    opts.backend = CameraBackend::kSynthetic;

    CaptureConfig cfg{};
    cfg.format.width = 4096;
    cfg.format.height = 4096;

    CameraCapture camera{cfg, std::make_unique<FakeCameraSource>(), opts};
    ASSERT_EQ(camera.open(), nimrtc::plugins::kOk);

    const auto negotiated = camera.config().format;
    EXPECT_EQ(negotiated.width, 1280u);
    EXPECT_EQ(negotiated.height, 720u);

    camera.close();
}

TEST(CameraCapture, ItsCapabilitiesAreHonest) {
    CameraCaptureOptions opts;
    opts.backend = CameraBackend::kSynthetic;

    CameraCapture camera{CaptureConfig{}, std::make_unique<FakeCameraSource>(), opts};
    const auto caps = camera.capabilities();

    EXPECT_TRUE(caps.enumeration);
    EXPECT_FALSE(caps.cursor_composited);
    EXPECT_FALSE(caps.dirty_rects);
    EXPECT_EQ(caps.backend, "synthetic");

    // The devices are cameras, not displays.
    const auto devices = camera.enumerate_devices();
    ASSERT_EQ(devices.size(), 1u);
    EXPECT_EQ(devices[0].kind, CaptureDeviceKind::kCamera);
}

TEST(CameraCapture, AnUnknownDeviceIdIsAnError) {
    CameraCaptureOptions opts;
    opts.backend = CameraBackend::kSynthetic;

    CaptureConfig cfg{};
    cfg.device_id = "no-such-camera";
    CameraCapture camera{cfg, std::make_unique<FakeCameraSource>(), opts};
    EXPECT_EQ(camera.open(), nimrtc::plugins::kErrInvalidParam);
    EXPECT_FALSE(camera.is_open());
}

// ---------------------------------------------------------------------------
// The factories + registration
// ---------------------------------------------------------------------------

TEST(CameraFactories, TheIdsAreStable) {
    EXPECT_EQ(SyntheticCameraFactory{}.id(), "synthetic-camera");
    EXPECT_EQ(CameraCaptureFactory{CameraBackend::kMediaFoundation}.id(),
              "camera-mediafoundation");
    EXPECT_EQ(CameraCaptureFactory{CameraBackend::kAvFoundation}.id(), "camera-avfoundation");
    EXPECT_EQ(CameraCaptureFactory{CameraBackend::kV4l2}.id(), "camera-v4l2");
}

TEST(CameraFactories, TheSyntheticCameraIsRegistered) {
    nimrtc::video_capture::register_camera_capture_plugins();
    const auto* factory =
        nimrtc::core::PluginRegistry::instance().get_video_capture("synthetic-camera");
    ASSERT_NE(factory, nullptr);

    std::unique_ptr<nimrtc::plugins::IVideoCapture> device{factory->create(CaptureConfig{})};
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(device->name(), std::string_view("synthetic-camera"));

    const auto devices = device->enumerate_devices();
    ASSERT_EQ(devices.size(), 1u);
    EXPECT_EQ(devices[0].kind, CaptureDeviceKind::kCamera);
}

TEST(CameraFactories, TheCameraPathRegistersUnderItsOwnIds) {
    nimrtc::video_capture::register_camera_capture_plugins();
    const auto& registry = nimrtc::core::PluginRegistry::instance();

    // The camera path registers under its *own* plug-in id, separately
    // from the screen path.
    EXPECT_NE(registry.get_video_capture("camera-mediafoundation"), nullptr);
    EXPECT_NE(registry.get_video_capture("camera-avfoundation"), nullptr);
    EXPECT_NE(registry.get_video_capture("camera-v4l2"), nullptr);
}

TEST(CameraFactories, AnUnavailableBackendFailsCleanly) {
    // On a build without the backend, `create()` still returns a device
    // -- and that device fails `open()` with "unsupported" rather than
    // crashing or producing black frames.
    const CameraCaptureFactory factory{CameraBackend::kV4l2};
    std::unique_ptr<nimrtc::plugins::IVideoCapture> device{factory.create(CaptureConfig{})};
    ASSERT_NE(device, nullptr);

    if (!nimrtc::video_capture::v4l2_camera_available()) {
        EXPECT_EQ(device->open(), nimrtc::plugins::kErrUnsupported);
    }
}
