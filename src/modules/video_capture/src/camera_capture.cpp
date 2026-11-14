// modules/video_capture/src/camera_capture.cpp
// ----------------------------------------------------------------------------
// The camera capture layer (remote-desktop plan A9).
//
// The *portable* half: the device enumeration, the format negotiation,
// the bounded queue with the `kDropNewest` policy, the frame delivery
// and the statistics.  It is compiled — and unit-tested — on **every**
// CI leg, including the Windows leg where the macOS and Linux camera
// adapters cannot even be compiled.
//
// The *platform* half (Media Foundation, AVFoundation, V4L2) lives under
// `src/platform/`.
// ----------------------------------------------------------------------------

#include <nimrtc/video_capture/camera_capture.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>

#include <nimrtc/core/registry.hpp>

#include "platform/camera_factory.hpp"

namespace nimrtc::video_capture {

namespace {

/** How long the capture thread sleeps when the camera has nothing to
 *  deliver.  The wait happens on the capture thread, never on the
 *  engine's tick thread. */
constexpr auto kIdleSleep = std::chrono::milliseconds(4);

std::int64_t now_us() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/**
 * @brief A camera device for a backend this build does not have.
 *
 * It answers every query honestly ("no cameras", "no formats") and
 * fails `open()` with `kErrUnsupported`, so a host that names
 * `"camera-v4l2"` on a Windows build gets a clear failure rather than a
 * link error or a silent black picture.
 */
class UnsupportedCamera final : public plugins::IVideoCapture {
  public:
    UnsupportedCamera(plugins::CaptureConfig cfg, std::string_view id,
                      std::string_view name) noexcept
        : cfg_(std::move(cfg)), id_(id), name_(name) {}

    [[nodiscard]] const char* name() const noexcept override { return name_.data(); }

    plugins::Status open() noexcept override { return plugins::kErrUnsupported; }

    void close() noexcept override {}

    [[nodiscard]] std::vector<plugins::CaptureDevice>
    enumerate_devices() const noexcept override {
        return {};
    }

    [[nodiscard]] std::vector<plugins::CaptureFormat>
    supported_formats(const std::string&) const noexcept override {
        return {};
    }

    [[nodiscard]] plugins::CaptureCapabilities
    capabilities() const noexcept override {
        plugins::CaptureCapabilities caps;
        caps.backend = std::string(id_);
        return caps;
    }

    [[nodiscard]] bool is_open() const noexcept override { return false; }

    [[nodiscard]] plugins::CaptureConfig config() const noexcept override { return cfg_; }

    plugins::Status pop_frame(plugins::CaptureFrame&) noexcept override {
        return plugins::kErrNotReady;
    }

    void set_frame_callback(plugins::CaptureFrameCallback cb) noexcept override {
        cb_ = std::move(cb);
    }

    [[nodiscard]] plugins::CaptureStats stats() const noexcept override { return stats_; }

  private:
    plugins::CaptureConfig cfg_;
    std::string id_;
    std::string name_;
    plugins::CaptureFrameCallback cb_;
    plugins::CaptureStats stats_{};
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// CameraCapture
// ---------------------------------------------------------------------------

CameraCapture::CameraCapture(plugins::CaptureConfig cfg,
                             std::unique_ptr<ICameraFrameSource> source,
                             CameraCaptureOptions opts) noexcept
    : cfg_(std::move(cfg)), opts_(opts), source_(std::move(source)) {
    // The camera's drop policy is the camera's own: `kDropNewest`.
    // A slightly stale frame beats a frame that arrives too late to be
    // displayed, and the seam's `kDropOldest` default is a
    // *screen-content* default.  The effective policy is written back
    // into the configuration, so `config()` reports what is actually in
    // force.
    cfg_.drop_policy = opts_.drop_policy;
    queue_.set_drop_policy(cfg_.drop_policy);
    if (cfg_.queue_depth > 0) queue_.set_depth(cfg_.queue_depth);
}

CameraCapture::~CameraCapture() {
    stop_thread();
}

const char* CameraCapture::name() const noexcept {
    switch (opts_.backend) {
        case CameraBackend::kMediaFoundation: return "mediafoundation-camera";
        case CameraBackend::kAvFoundation:   return "avfoundation-camera";
        case CameraBackend::kV4l2:           return "v4l2-camera";
        case CameraBackend::kAndroidCamera2: return "android-camera2-camera";
        case CameraBackend::kSynthetic:      return "synthetic-camera";
    }
    return "camera-capture";
}

plugins::Status CameraCapture::open() noexcept {
    if (open_) return plugins::kOk;
    if (!source_) return plugins::kErrUnsupported;

    // ---- The device ------------------------------------------------------
    const auto devices = source_->enumerate_cameras();
    if (devices.empty()) return plugins::kErrInvalidParam;

    const plugins::CaptureDevice* chosen = nullptr;
    if (!cfg_.device_id.empty()) {
        for (const auto& d : devices) {
            if (d.id == cfg_.device_id) {
                chosen = &d;
                break;
            }
        }
        if (chosen == nullptr) {
            // The caller asked for a camera that does not exist.  That
            // is an error, not a silent fallback to the default camera.
            return plugins::kErrInvalidParam;
        }
    } else {
        chosen = &devices.front();
    }
    cfg_.device_id = chosen->id;

    // ---- The negotiation -------------------------------------------------
    //
    // The camera's native size is the ceiling: a request larger than
    // the camera is *clamped*, never upscaled.  The caller reads the
    // result back from `config()`.
    plugins::CaptureFormat requested = cfg_.format;
    if (requested.width == 0 || requested.height == 0) {
        requested.width = chosen->max_width;
        requested.height = chosen->max_height;
    } else {
        requested.width = std::min(requested.width, chosen->max_width);
        requested.height = std::min(requested.height, chosen->max_height);
    }
    if (requested.width == 0 || requested.height == 0) {
        requested.width = chosen->max_width;
        requested.height = chosen->max_height;
    }
    if (requested.fps == 0) {
        requested.fps = (chosen->max_fps != 0) ? chosen->max_fps : opts_.default_fps;
    }
    if (requested.tag.empty()) {
        requested.tag = std::string(camera_backend_name(opts_.backend)) + "-nv12";
    }
    // The honest flags: the camera does not report dirty rectangles and
    // (in this portable layer) does not deliver a GPU surface.
    requested.zero_copy = false;
    requested.has_dirty_rects = false;

    negotiated_ = requested;

    const plugins::Status status = source_->open(cfg_.device_id, negotiated_);
    if (status != plugins::kOk) return status;

    next_seq_ = 0;
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { capture_loop(); });

    open_ = true;
    return plugins::kOk;
}

void CameraCapture::close() noexcept {
    stop_thread();
    if (source_) source_->close();
    queue_.clear();
    open_ = false;
}

void CameraCapture::stop_thread() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        if (thread_.joinable()) thread_.join();
        return;
    }
    if (thread_.joinable()) thread_.join();
}

void CameraCapture::capture_loop() noexcept {
    std::vector<std::uint8_t> pixels;
    std::size_t size = 0;
    std::int32_t stride = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int64_t ts = 0;

    while (running_.load(std::memory_order_acquire)) {
        const plugins::Status status = source_->next_frame(pixels, size, stride, width, height, ts);
        if (status == plugins::kErrNotReady) {
            // Nothing to deliver right now.  The capture thread sleeps
            // rather than spinning; the engine's tick thread is never
            // involved either way.
            std::this_thread::sleep_for(kIdleSleep);
            continue;
        }
        if (status != plugins::kOk) {
            ++stats_.device_errors;
            running_.store(false, std::memory_order_release);
            break;
        }

        const std::uint32_t seq = next_seq_++;

        // A camera's picture has no "unchanged region": the whole
        // frame is the content.  The honest answer is `full_frame`.
        plugins::CaptureDirtyRects dirty;
        dirty.full_frame = true;
        dirty.rects.clear();

        const auto outcome =
            queue_.push(pixels.data(), size, stride, width, height, ts, seq, dirty);
        switch (outcome) {
            case CaptureQueue::PushResult::kQueued:
                ++stats_.frames_captured;
                break;
            case CaptureQueue::PushResult::kDroppedOldest:
                ++stats_.frames_captured;
                ++stats_.frames_dropped;
                break;
            case CaptureQueue::PushResult::kRejected:
                // The camera's default policy drops the *newest* frame:
                // the queue keeps what it had, and the arrival is
                // counted as a drop.
                ++stats_.frames_dropped;
                break;
        }
        ++stats_.frames_cpu_staged;

        if (cb_) {
            plugins::CaptureFrame view;
            if (queue_.peek(view)) {
                view.format = opts_.pixel_format;
                view.gpu_backend = plugins::GpuBackend::kNone;
                view.has_gpu_handle = false;
                view.gpu_handle = nullptr;
                cb_(view);
            }
        }
    }
}

std::vector<plugins::CaptureDevice>
CameraCapture::enumerate_devices() const noexcept {
    if (!source_) return {};
    return source_->enumerate_cameras();
}

std::vector<plugins::CaptureFormat>
CameraCapture::supported_formats(const std::string& device_id) const noexcept {
    if (!source_) return {};
    return source_->supported_formats(device_id);
}

plugins::CaptureCapabilities CameraCapture::capabilities() const noexcept {
    plugins::CaptureCapabilities caps;
    caps.zero_copy = false;
    // A camera has no dirty rectangles: every frame is a new picture.
    caps.dirty_rects = false;
    // A camera has no pointer, so there is nothing to composite.  The
    // flag stays honest.
    caps.cursor_composited = false;
    // A camera cannot capture a sub-region of its sensor.
    caps.region_selection = false;
    caps.enumeration = true;

    std::uint32_t max_w = 0;
    std::uint32_t max_h = 0;
    std::uint32_t max_fps = 0;
    for (const auto& d : enumerate_devices()) {
        max_w = std::max(max_w, d.max_width);
        max_h = std::max(max_h, d.max_height);
        max_fps = std::max(max_fps, d.max_fps);
    }
    caps.max_pixels = static_cast<std::uint64_t>(max_w) * max_h;
    caps.max_fps = max_fps;
    caps.backend = std::string(camera_backend_name(opts_.backend));
    return caps;
}

bool CameraCapture::is_open() const noexcept {
    return open_;
}

plugins::CaptureConfig CameraCapture::config() const noexcept {
    plugins::CaptureConfig out = cfg_;
    if (negotiated_.width != 0 && negotiated_.height != 0) {
        out.format = negotiated_;
    }
    return out;
}

plugins::Status CameraCapture::pop_frame(plugins::CaptureFrame& out) noexcept {
    if (!open_) return plugins::kErrNotReady;
    if (!queue_.pop(out)) return plugins::kErrNotReady;
    out.format = opts_.pixel_format;
    out.gpu_backend = plugins::GpuBackend::kNone;
    out.has_gpu_handle = false;
    out.gpu_handle = nullptr;
    return plugins::kOk;
}

void CameraCapture::set_frame_callback(plugins::CaptureFrameCallback cb) noexcept {
    cb_ = std::move(cb);
}

plugins::CaptureStats CameraCapture::stats() const noexcept {
    return stats_;
}

// ---------------------------------------------------------------------------
// SyntheticCamera
// ---------------------------------------------------------------------------

SyntheticCamera::SyntheticCamera(plugins::CaptureConfig cfg) noexcept : cfg_(std::move(cfg)) {
    // The camera's drop policy is `kDropNewest`: a slightly stale frame
    // beats a frame that arrives too late to be displayed.  The seam's
    // `kDropOldest` default is a *screen-content* default, so it is
    // deliberately not applied here.
    cfg_.drop_policy = plugins::CaptureDropPolicy::kDropNewest;
    queue_.set_drop_policy(cfg_.drop_policy);
    if (cfg_.queue_depth > 0) queue_.set_depth(cfg_.queue_depth);
}

SyntheticCamera::~SyntheticCamera() = default;

plugins::Status SyntheticCamera::open() noexcept {
    if (open_) return plugins::kOk;

    // A zero-sized request means "the device's best", which for the
    // synthetic camera is 640x480.
    if (cfg_.format.width == 0) cfg_.format.width = 640;
    if (cfg_.format.height == 0) cfg_.format.height = 480;
    if (cfg_.format.fps == 0) cfg_.format.fps = 30;
    if (cfg_.format.tag.empty()) cfg_.format.tag = "nv12-synthetic";

    // The camera's native size is the ceiling: a request larger than
    // the camera is *clamped*, never upscaled.  The caller reads the
    // result back from `config()`.
    cfg_.format.width = std::min(cfg_.format.width, kMaxWidth);
    cfg_.format.height = std::min(cfg_.format.height, kMaxHeight);

    // The device id is opaque; an unknown one is an error, not a
    // silent fallback, because the caller asked for a camera that does
    // not exist.
    if (!cfg_.device_id.empty() && cfg_.device_id != kDeviceId) {
        return plugins::kErrInvalidParam;
    }
    cfg_.device_id = kDeviceId;

    // The frame is NV12: one luma plane followed by one interleaved
    // chroma plane.
    const std::size_t y_size = static_cast<std::size_t>(cfg_.format.width) *
                               static_cast<std::size_t>(cfg_.format.height);
    const std::size_t uv_size = y_size / 2u;
    scratch_.assign(y_size + uv_size, 0u);

    frame_index_ = 0;
    open_ = true;
    return plugins::kOk;
}

void SyntheticCamera::close() noexcept {
    if (!open_) return;
    open_ = false;
    queue_.clear();
    scratch_.clear();
    scratch_.shrink_to_fit();
}

std::vector<plugins::CaptureDevice>
SyntheticCamera::enumerate_devices() const noexcept {
    plugins::CaptureDevice d;
    d.id = kDeviceId;
    d.name = "Synthetic camera";
    // A camera is a camera: the kind is what tells the engine that this
    // is *not* screen content.
    d.kind = plugins::CaptureDeviceKind::kCamera;
    d.max_width = kMaxWidth;
    d.max_height = kMaxHeight;
    d.max_fps = 30;
    return {d};
}

std::vector<plugins::CaptureFormat>
SyntheticCamera::supported_formats(const std::string& device_id) const noexcept {
    (void)device_id;
    std::vector<plugins::CaptureFormat> out;
    const std::uint32_t sizes[][2] = {{320, 240}, {640, 480}, {1280, 720}};
    for (const auto& s : sizes) {
        plugins::CaptureFormat f;
        f.width = s[0];
        f.height = s[1];
        f.fps = 30;
        f.tag = "nv12-synthetic";
        f.zero_copy = false;
        f.has_dirty_rects = false;
        out.push_back(std::move(f));
    }
    return out;
}

plugins::CaptureCapabilities SyntheticCamera::capabilities() const noexcept {
    plugins::CaptureCapabilities c;
    c.zero_copy = false;
    // A camera has no dirty rectangles.
    c.dirty_rects = false;
    // A camera has no pointer: nothing to composite, and the flag says
    // so honestly.
    c.cursor_composited = false;
    // A camera cannot capture a sub-region.
    c.region_selection = false;
    // The synthetic camera enumerates itself.
    c.enumeration = true;
    c.max_pixels = static_cast<std::uint64_t>(kMaxWidth) * kMaxHeight;
    c.max_fps = 30;
    c.backend = std::string(camera_backend_name(CameraBackend::kSynthetic));
    return c;
}

plugins::Status SyntheticCamera::pop_frame(plugins::CaptureFrame& out) noexcept {
    if (!open_) return plugins::kErrNotReady;
    if (!queue_.pop(out)) return plugins::kErrNotReady;
    out.format = plugins::VideoPixelFormat::kNV12;
    out.gpu_backend = plugins::GpuBackend::kNone;
    out.has_gpu_handle = false;
    out.gpu_handle = nullptr;
    return plugins::kOk;
}

void SyntheticCamera::render_frame(std::uint8_t* dst, std::size_t size,
                                    std::uint32_t frame_index) const noexcept {
    if (dst == nullptr || size == 0) return;

    const std::uint32_t w = cfg_.format.width;
    const std::uint32_t h = cfg_.format.height;
    const std::size_t y_size = static_cast<std::size_t>(w) * h;

    // The luma plane is a moving gradient, so successive frames differ
    // and the encoder has something to compress.  The chroma plane is
    // flat (a neutral grey), which is what a real camera in a fixed
    // lighting condition looks like.
    const std::uint8_t luma_base = static_cast<std::uint8_t>(16u + ((frame_index * 5u) % 200u));
    for (std::uint32_t y = 0; y < h; ++y) {
        std::uint8_t* row = dst + static_cast<std::size_t>(y) * w;
        const std::uint8_t luma =
            static_cast<std::uint8_t>(luma_base + ((y * 64u) / (h == 0 ? 1u : h)));
        for (std::uint32_t x = 0; x < w; ++x) {
            row[x] = luma;
        }
    }
    if (size > y_size) {
        std::memset(dst + y_size, 128, size - y_size);
    }
}

plugins::Status SyntheticCamera::produce_one() noexcept {
    if (!open_) return plugins::kErrNotReady;

    render_frame(scratch_.data(), scratch_.size(), frame_index_);

    // A camera's picture has no "unchanged region".
    plugins::CaptureDirtyRects dirty;
    dirty.full_frame = true;
    dirty.rects.clear();

    const std::int64_t ts = now_us();
    const auto outcome =
        queue_.push(scratch_.data(), scratch_.size(),
                    static_cast<std::int32_t>(cfg_.format.width), cfg_.format.width,
                    cfg_.format.height, ts, frame_index_, dirty);
    switch (outcome) {
        case CaptureQueue::PushResult::kQueued:
            ++stats_.frames_captured;
            break;
        case CaptureQueue::PushResult::kDroppedOldest:
            ++stats_.frames_captured;
            ++stats_.frames_dropped;
            break;
        case CaptureQueue::PushResult::kRejected:
            ++stats_.frames_dropped;
            break;
    }

    ++frame_index_;

    if (cb_) {
        plugins::CaptureFrame view;
        if (!queue_.peek(view)) return plugins::kOk;
        view.format = plugins::VideoPixelFormat::kNV12;
        view.gpu_backend = plugins::GpuBackend::kNone;
        view.has_gpu_handle = false;
        view.gpu_handle = nullptr;
        cb_(view);
    }
    return plugins::kOk;
}

// ---------------------------------------------------------------------------
// Factories + registration
// ---------------------------------------------------------------------------

std::string_view CameraCaptureFactory::id() const noexcept {
    switch (backend_) {
        case CameraBackend::kMediaFoundation: return "camera-mediafoundation";
        case CameraBackend::kAvFoundation:   return "camera-avfoundation";
        case CameraBackend::kV4l2:           return "camera-v4l2";
        case CameraBackend::kAndroidCamera2: return "camera-android-camera2";
        case CameraBackend::kSynthetic:      return "synthetic-camera";
    }
    return "camera-unknown";
}

std::string_view CameraCaptureFactory::display_name() const noexcept {
    switch (backend_) {
        case CameraBackend::kMediaFoundation:
            return "Windows Media Foundation camera (IMFSourceReader, NV12)";
        case CameraBackend::kAvFoundation:
            return "macOS AVFoundation camera (AVCaptureSession, NV12)";
        case CameraBackend::kV4l2:
            return "Linux V4L2 camera (VIDIOC_STREAMON + mmap, NV12)";
        case CameraBackend::kAndroidCamera2:
            return "Android Camera2 camera";
        case CameraBackend::kSynthetic:
            return "Synthetic camera (CI-testable, NV12)";
    }
    return "Camera capture";
}

plugins::IVideoCapture* CameraCaptureFactory::create(plugins::CaptureConfig cfg) const {
    if (backend_ == CameraBackend::kSynthetic) {
        return new SyntheticCamera(std::move(cfg));
    }
    if (auto* device = detail::create_camera_capture(std::move(cfg), backend_)) {
        return device;
    }
    // The backend is not available in this build.  The device still
    // exists -- it just fails `open()` with "unsupported", which is a
    // clean error rather than a link error.
    return new UnsupportedCamera(std::move(cfg), id(), display_name());
}

void register_camera_capture_plugins() noexcept {
    static bool registered = false;
    if (registered) return;
    registered = true;

    static const SyntheticCameraFactory synthetic_factory{};
    core::PluginRegistry::instance().register_video_capture(synthetic_factory.id(),
                                                            &synthetic_factory);

    // The platform backends register even when this build does not have
    // them: the device then fails `open()` with `kErrUnsupported`, which
    // is a clean error rather than a link error.
    static const CameraCaptureFactory mf_factory{CameraBackend::kMediaFoundation};
    static const CameraCaptureFactory avf_factory{CameraBackend::kAvFoundation};
    static const CameraCaptureFactory v4l2_factory{CameraBackend::kV4l2};
    core::PluginRegistry::instance().register_video_capture(mf_factory.id(), &mf_factory);
    core::PluginRegistry::instance().register_video_capture(avf_factory.id(), &avf_factory);
    core::PluginRegistry::instance().register_video_capture(v4l2_factory.id(), &v4l2_factory);
}

bool mediafoundation_camera_available() noexcept {
#if defined(NIMRTC_HAS_MEDIA_FOUNDATION_CAMERA)
    return true;
#else
    return false;
#endif
}

bool avfoundation_camera_available() noexcept {
#if defined(NIMRTC_HAS_AVFOUNDATION_CAMERA)
    return true;
#else
    return false;
#endif
}

bool v4l2_camera_available() noexcept {
#if defined(NIMRTC_HAS_V4L2_CAMERA)
    return true;
#else
    return false;
#endif
}

} // namespace nimrtc::video_capture
