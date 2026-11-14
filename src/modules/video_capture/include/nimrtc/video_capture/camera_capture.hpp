/**
 * @file nimrtc/video_capture/camera_capture.hpp
 * @brief The camera capture layer (remote-desktop plan A9).
 *
 * A camera is the *same seam*, a different device class:
 *
 * > "the same seam, a different device class: Windows Media Foundation /
 * > D3D11 camera, macOS AVFoundation, Linux V4L2.  Delivers to the same
 * > `GpuBuffer` / `VideoSourceFrame` contract so the encoder path is
 * > identical."  (plan A9)
 *
 * So this module delivers a `plugins::IVideoCapture` whose devices are
 * of kind `CaptureDeviceKind::kCamera`, whose id is the *camera's* id,
 * and whose frames are typically **NV12** rather than BGRA.  Everything
 * else — the bounded queue, the drop policy, the format negotiation,
 * the statistics — is the machinery the screen path already uses, and
 * it is reused rather than reimplemented.
 *
 * ## What is different from a screen capture
 *
 * | | screen | camera |
 * |---|---|---|
 * | device kind | `kScreen` / `kWindow` | `kCamera` |
 * | pixel format | BGRA | NV12 / I420 |
 * | drop policy | `kDropOldest` (newest wins) | `kDropNewest` (stale beats late) |
 * | dirty rects | the interesting part | not applicable (every frame is "full") |
 * | cursor | the pointer is a separate layer | there is no pointer |
 *
 * The drop policy difference is the important one.  For screen content
 * a stale frame is worth *nothing* (the screen has since changed), so
 * the oldest goes.  For a camera a stale frame is still a perfectly
 * good picture, while a frame that arrives too late to be displayed is
 * worthless — so the *newest* goes.  That asymmetry is exactly what
 * `plugins::CaptureDropPolicy` documents, and the camera path defaults
 * to `kDropNewest`.
 *
 * ## What is portable
 *
 * The camera *layer* is portable C++20 and is unit-tested on every CI
 * leg:
 *
 *   - the `CameraBackend` enum and its name helper,
 *   - the device enumeration (cameras, not displays),
 *   - the format negotiation (a request larger than the camera's
 *     native size is clamped),
 *   - the bounded queue and the `kDropNewest` policy,
 *   - the frame delivery (the timestamps and the sequence numbers),
 *   - and the **synthetic camera** — a camera that needs no hardware,
 *     so the camera path is testable on every CI leg.
 *
 * The *sources* — Media Foundation, AVFoundation, V4L2 — are thin
 * platform adapters under `src/platform/`.
 *
 * @note Remote-desktop plan A9, milestone M2.
 */

#ifndef NIMRTC_VIDEO_CAPTURE_CAMERA_CAPTURE_HPP
#define NIMRTC_VIDEO_CAPTURE_CAMERA_CAPTURE_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nimrtc/plugins/hw_seam.hpp>
#include <nimrtc/plugins/video_capture.hpp>
#include <nimrtc/video_capture/capture_queue.hpp>

namespace nimrtc::video_capture {

// ---------------------------------------------------------------------------
// CameraBackend
// ---------------------------------------------------------------------------

/**
 * @brief The camera's own hardware backend.
 *
 * This is the camera analogue of `plugins::HwBackend`: it says which
 * platform API actually drives the device.  It is reported through
 * `CameraCapture::hardware_backend()`, exactly as the codec seam reports
 * its own.
 */
enum class NIMRTC_API_STABLE CameraBackend : std::uint8_t {
    /** Windows Media Foundation (the `IMFSourceReader` path). */
    kMediaFoundation = 0,

    /** macOS AVFoundation (`AVCaptureSession`). */
    kAvFoundation = 1,

    /** Linux V4L2 (`VIDIOC_STREAMON` + `mmap`). */
    kV4l2 = 2,

    /** Android Camera2. */
    kAndroidCamera2 = 3,

    /** The synthetic camera — no hardware, no OS. */
    kSynthetic = 4,
};

/**
 * @brief The camera backend's short name.
 *
 * The names match the convention the rest of the code base uses for
 * `hardware_backend()`: `"mediafoundation"`, `"avfoundation"`, `"v4l2"`,
 * `"android-camera2"`, `"synthetic"`.
 */
NIMRTC_API_STABLE inline std::string_view camera_backend_name(CameraBackend b) noexcept {
    switch (b) {
        case CameraBackend::kMediaFoundation: return "mediafoundation";
        case CameraBackend::kAvFoundation:   return "avfoundation";
        case CameraBackend::kV4l2:           return "v4l2";
        case CameraBackend::kAndroidCamera2: return "android-camera2";
        case CameraBackend::kSynthetic:      return "synthetic";
    }
    return "unknown";
}

/**
 * @brief The camera backend's canonical `plugins::HwBackend` value.
 *
 * The plan's acceptance criterion names the camera's own hardware
 * backend as `DirectShow` / `AvFoundation` / `V4l2` / `AndroidCamera2`
 * — those are the values of the seam-wide `plugins::HwBackend` enum,
 * so the camera reports itself in the *same* vocabulary every other
 * hardware-accelerated plugin uses.  A host can therefore compare the
 * camera's backend against the encoder's backend without knowing
 * anything about cameras.
 *
 * The Windows camera is Media Foundation rather than DirectShow, so it
 * maps onto `HwBackend::DirectShow` — the "Windows camera capture"
 * slot of the enum, which is the only one that fits.
 */
NIMRTC_API_STABLE inline plugins::HwBackend camera_hw_backend(CameraBackend b) noexcept {
    switch (b) {
        case CameraBackend::kMediaFoundation: return plugins::HwBackend::DirectShow;
        case CameraBackend::kAvFoundation:   return plugins::HwBackend::AvFoundation;
        case CameraBackend::kV4l2:           return plugins::HwBackend::V4l2;
        case CameraBackend::kAndroidCamera2: return plugins::HwBackend::AndroidCamera2;
        case CameraBackend::kSynthetic:      return plugins::HwBackend::Software;
    }
    return plugins::HwBackend::Unknown;
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

/** Options for the camera capture layer. */
struct NIMRTC_API_STABLE CameraCaptureOptions {
    /** Which backend drives the device. */
    CameraBackend backend = CameraBackend::kSynthetic;

    /** Whether the backend may deliver frames as GPU surfaces.
     *
     *  When true and the backend has a native surface (a D3D11 texture
     *  from Media Foundation's D3D11 manager), the frame is delivered as
     *  a GPU handle and the pixels never enter host memory.  When the
     *  GPU path is not usable the capture falls back to CPU staging and
     *  counts the copy in `CaptureStats::bytes_copied` — it never
     *  pretends to be zero-copy. */
    bool allow_zero_copy = true;

    /** The pixel format the camera delivers.
     *
     *  NV12 is the native format of every camera backend worth the
     *  name (Media Foundation's NV12 / AVFoundation's
     *  `kCVPixelFormatType_420YpCbCr8BiPlanarFullRange` / V4L2's
     *  `V4L2_PIX_FMT_NV12`), and it is what the encoder path wants. */
    plugins::VideoPixelFormat pixel_format = plugins::VideoPixelFormat::kNV12;

    /** The queue's overflow policy.
     *
     *  The camera's policy is the camera's own: `kDropNewest`, because
     *  a slightly stale frame beats a frame that arrives too late to be
     *  displayed.  The seam's `CaptureConfig::drop_policy` default
     *  (`kDropOldest`) is a *screen-content* default — for screen
     *  content the newest frame is the truth and a stale one is worth
     *  nothing — so it is deliberately **not** applied here.  A host
     *  that wants the screen-content policy on a camera sets it here
     *  explicitly. */
    plugins::CaptureDropPolicy drop_policy = plugins::CaptureDropPolicy::kDropNewest;

    /** The frame rate used when the caller asks for "the device's
     *  best" and the device does not state one. */
    std::uint32_t default_fps = 30;
};

// ---------------------------------------------------------------------------
// ICameraFrameSource
// ---------------------------------------------------------------------------

/**
 * @brief The platform-specific half of a camera capture device.
 *
 * The implementation produces the frames; everything else — the queue,
 * the drop policy, the sequence numbers, the statistics, the
 * negotiation — is the portable layer's.
 */
class NIMRTC_API_STABLE ICameraFrameSource {
public:
    virtual ~ICameraFrameSource() = default;

    /** The cameras this source can open. */
    [[nodiscard]] virtual std::vector<plugins::CaptureDevice>
    enumerate_cameras() const noexcept = 0;

    /** The formats @p device_id can deliver. */
    [[nodiscard]] virtual std::vector<plugins::CaptureFormat>
    supported_formats(const std::string& device_id) const noexcept = 0;

    /** Open the camera. */
    virtual plugins::Status open(const std::string& device_id,
                                 const plugins::CaptureFormat& format) noexcept = 0;

    /** Close the camera. */
    virtual void close() noexcept = 0;

    /**
     * @brief Get the next frame.
     *
     * @param pixels  Receives the pixel bytes (the caller owns the
     *                buffer).
     * @param size    Receives the size of the pixels.
     * @param stride  Receives the row stride in bytes.
     * @param width   Receives the width in pixels.
     * @param height  Receives the height in pixels.
     * @param ts      Receives the capture timestamp (microseconds).
     * @return `kOk` when a frame was produced, `kErrNotReady` when
     *         there is nothing to deliver right now.
     */
    virtual plugins::Status next_frame(std::vector<std::uint8_t>& pixels, std::size_t& size,
                                       std::int32_t& stride, std::uint32_t& width,
                                       std::uint32_t& height, std::int64_t& ts) noexcept = 0;
};

// ---------------------------------------------------------------------------
// CameraCapture
// ---------------------------------------------------------------------------

/**
 * @brief A camera capture device.
 *
 * It implements the whole of `plugins::IVideoCapture` over an
 * `ICameraFrameSource`, and it is the *same* contract the screen path
 * honours — so the engine's pump does not need to know whether it is
 * pumping a screen or a camera.
 *
 * The device owns its own thread: the engine's tick thread only ever
 * touches the bounded queue (RD-6).
 */
class NIMRTC_API_STABLE CameraCapture final : public plugins::IVideoCapture {
public:
    /**
     * @brief Construct a camera capture device.
     *
     * @param cfg    The requested configuration.
     * @param source The platform frame source.  Ownership is taken.
     * @param opts   The camera options.
     */
    CameraCapture(plugins::CaptureConfig cfg, std::unique_ptr<ICameraFrameSource> source,
                  CameraCaptureOptions opts = {}) noexcept;

    ~CameraCapture() override;

    CameraCapture(const CameraCapture&) = delete;
    CameraCapture& operator=(const CameraCapture&) = delete;

    // ---- IPlugin ----------------------------------------------------------
    [[nodiscard]] const char* name() const noexcept override;

    plugins::Status open() noexcept override;
    void close() noexcept override;

    // ---- IVideoCapture ----------------------------------------------------
    [[nodiscard]] std::vector<plugins::CaptureDevice>
    enumerate_devices() const noexcept override;

    [[nodiscard]] std::vector<plugins::CaptureFormat>
    supported_formats(const std::string& device_id) const noexcept override;

    [[nodiscard]] plugins::CaptureCapabilities
    capabilities() const noexcept override;

    [[nodiscard]] bool is_open() const noexcept override;

    [[nodiscard]] plugins::CaptureConfig config() const noexcept override;

    plugins::Status pop_frame(plugins::CaptureFrame& out) noexcept override;

    void set_frame_callback(plugins::CaptureFrameCallback cb) noexcept override;

    [[nodiscard]] plugins::CaptureStats stats() const noexcept override;

    // ---- Camera-specific --------------------------------------------------

    /**
     * @brief The camera's own hardware backend.
     *
     * This is the camera analogue of `IVideoCodec::hardware_backend()`:
     * it says which platform API drives the device
     * (`"mediafoundation"`, `"avfoundation"`, `"v4l2"`,
     * `"android-camera2"`, `"synthetic"`).
     */
    [[nodiscard]] std::string_view hardware_backend() const noexcept {
        return camera_backend_name(opts_.backend);
    }

    /** The backend this device uses. */
    [[nodiscard]] CameraBackend backend() const noexcept { return opts_.backend; }

    /**
     * @brief The camera's canonical `plugins::HwBackend`.
     *
     * The same value the seam-wide hardware vocabulary uses, so a host
     * can compare the camera's backend against the encoder's without
     * knowing anything about cameras.
     */
    [[nodiscard]] plugins::HwBackend hw_backend() const noexcept {
        return camera_hw_backend(opts_.backend);
    }

private:
    void capture_loop() noexcept;
    void stop_thread() noexcept;

    plugins::CaptureConfig cfg_;
    CameraCaptureOptions opts_;
    std::unique_ptr<ICameraFrameSource> source_;

    CaptureQueue queue_{2};
    plugins::CaptureFrameCallback cb_;
    plugins::CaptureStats stats_{};

    plugins::CaptureFormat negotiated_{};

    std::atomic<bool> running_{false};
    std::thread thread_;

    std::uint32_t next_seq_ = 0;
    bool open_ = false;
};

// ---------------------------------------------------------------------------
// SyntheticCamera
// ---------------------------------------------------------------------------

/**
 * @brief A camera that needs no hardware.
 *
 * This is what makes plan A9 verifiable in CI: the camera path — the
 * enumeration, the negotiation, the drop policy, the frame delivery —
 * is exercised on every CI leg, on every platform, with no camera
 * attached and no permission prompt.
 *
 * It produces NV12 frames (the format every real camera backend
 * delivers), so the *format declaration* on the path is exercised too.
 */
class NIMRTC_API_STABLE SyntheticCamera final : public plugins::IVideoCapture {
public:
    /** Construct a synthetic camera. */
    explicit SyntheticCamera(plugins::CaptureConfig cfg = {}) noexcept;

    ~SyntheticCamera() override;

    SyntheticCamera(const SyntheticCamera&) = delete;
    SyntheticCamera& operator=(const SyntheticCamera&) = delete;

    // ---- IPlugin ----------------------------------------------------------
    [[nodiscard]] const char* name() const noexcept override { return "synthetic-camera"; }

    plugins::Status open() noexcept override;
    void close() noexcept override;

    // ---- IVideoCapture ----------------------------------------------------
    [[nodiscard]] std::vector<plugins::CaptureDevice>
    enumerate_devices() const noexcept override;

    [[nodiscard]] std::vector<plugins::CaptureFormat>
    supported_formats(const std::string& device_id) const noexcept override;

    [[nodiscard]] plugins::CaptureCapabilities
    capabilities() const noexcept override;

    [[nodiscard]] bool is_open() const noexcept override { return open_; }

    [[nodiscard]] plugins::CaptureConfig config() const noexcept override { return cfg_; }

    plugins::Status pop_frame(plugins::CaptureFrame& out) noexcept override;

    void set_frame_callback(plugins::CaptureFrameCallback cb) noexcept override {
        cb_ = std::move(cb);
    }

    [[nodiscard]] plugins::CaptureStats stats() const noexcept override { return stats_; }

    // ---- Camera-specific --------------------------------------------------

    /** The camera's own hardware backend. */
    [[nodiscard]] std::string_view hardware_backend() const noexcept {
        return camera_backend_name(CameraBackend::kSynthetic);
    }

    /** The camera's canonical `plugins::HwBackend`. */
    [[nodiscard]] plugins::HwBackend hw_backend() const noexcept {
        return camera_hw_backend(CameraBackend::kSynthetic);
    }

    /**
     * Produce one frame into the queue.
     *
     * The frame callback (if set) fires synchronously, before this
     * method returns — so a test that uses the callback sees the frame
     * at a deterministic point.
     */
    plugins::Status produce_one() noexcept;

    /** The device id the synthetic camera answers to. */
    static constexpr const char* kDeviceId = "synthetic-camera:0";

    /** The synthetic camera's native width, in pixels. */
    static constexpr std::uint32_t kMaxWidth = 1280;

    /** The synthetic camera's native height, in pixels. */
    static constexpr std::uint32_t kMaxHeight = 720;

private:
    void render_frame(std::uint8_t* dst, std::size_t size,
                      std::uint32_t frame_index) const noexcept;

    plugins::CaptureConfig cfg_;
    CaptureQueue queue_{2};
    plugins::CaptureFrameCallback cb_;
    plugins::CaptureStats stats_{};
    std::vector<std::uint8_t> scratch_;
    std::uint32_t frame_index_ = 0;
    bool open_ = false;
};

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------

/**
 * @brief Factory for the synthetic camera.
 *
 * Registered under the id `"synthetic-camera"`.
 */
class NIMRTC_API_STABLE SyntheticCameraFactory final
    : public plugins::IVideoCaptureFactory {
public:
    [[nodiscard]] std::string_view id() const noexcept override { return "synthetic-camera"; }

    [[nodiscard]] std::string_view display_name() const noexcept override {
        return "Synthetic camera (CI-testable, NV12)";
    }

    [[nodiscard]] plugins::IVideoCapture* create(
        plugins::CaptureConfig cfg) const override {
        return new SyntheticCamera(std::move(cfg));
    }
};

/**
 * @brief Factory for the platform camera capture devices.
 *
 * One factory per backend, each registering under its own plug-in id:
 *
 *   - `"camera-mediafoundation"` — Windows,
 *   - `"camera-avfoundation"`   — macOS,
 *   - `"camera-v4l2"`           — Linux.
 *
 * A backend this build does not have still registers — its device
 * fails `open()` with `kErrUnsupported`, which is a clean error rather
 * than a link error or a silent black picture.
 */
class NIMRTC_API_STABLE CameraCaptureFactory final : public plugins::IVideoCaptureFactory {
public:
    explicit CameraCaptureFactory(CameraBackend backend) noexcept : backend_(backend) {}

    [[nodiscard]] std::string_view id() const noexcept override;

    [[nodiscard]] std::string_view display_name() const noexcept override;

    [[nodiscard]] plugins::IVideoCapture* create(
        plugins::CaptureConfig cfg) const override;

    /** The backend this factory produces. */
    [[nodiscard]] CameraBackend backend() const noexcept { return backend_; }

private:
    CameraBackend backend_;
};

/**
 * @brief Register the camera capture factories with `core::PluginRegistry`.
 *
 * Registers the synthetic camera (always) plus the platform backends
 * this build supports.  Idempotent.
 */
NIMRTC_API_STABLE void register_camera_capture_plugins() noexcept;

/** Whether this build supports the Windows Media Foundation camera. */
NIMRTC_API_STABLE bool mediafoundation_camera_available() noexcept;

/** Whether this build supports the macOS AVFoundation camera. */
NIMRTC_API_STABLE bool avfoundation_camera_available() noexcept;

/** Whether this build supports the Linux V4L2 camera. */
NIMRTC_API_STABLE bool v4l2_camera_available() noexcept;

} // namespace nimrtc::video_capture
#endif // NIMRTC_VIDEO_CAPTURE_CAMERA_CAPTURE_HPP
