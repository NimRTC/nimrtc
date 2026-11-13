// modules/video_capture/src/platform/windows/windows_capture.cpp
// ----------------------------------------------------------------------------
// The Windows capture device 锟?the seam implementation that ties the two
// Windows backends to the portable capture layer.
//
// See include/nimrtc/video_capture/windows_capture.hpp for the contract.
//
// ## Threading (RD-6)
//
// The device owns a thread.  It does the DXGI acquire / WGC poll there,
// and it does *all* the device calls there.  The engine's tick thread
// only ever touches the bounded `CaptureQueue` 锟?never a device object 锟?// so it can never be parked by a slow GPU.
//
// ## The two delivery paths
//
//   - **GPU (zero-copy).**  When a GPU callback is attached, the frame is
//     copied *on the device* into a pooled texture (the pixels never
//     leave the GPU), wrapped in a `GpuBuffer`, and handed to the
//     callback.  The frame does **not** go through the copy queue: that
//     queue copies the pixels into host memory, which is exactly what
//     the zero-copy path exists to avoid.
//
//   - **CPU (staged).**  When there is no GPU callback 锟?or when the GPU
//     staging failed 锟?the frame is read back into host memory and
//     pushed through the `CaptureQueue`, and the copied bytes are
//     counted in `CaptureStats::bytes_copied`.  The fallback is honest:
//     the statistics show that the fast path was not taken.
//
// A failure in either backend (consent refused, device lost) stops the
// capture thread and reports through the error callback with a distinct
// error code.  It never crashes and never spins.
// ----------------------------------------------------------------------------

#include "nimrtc/video_capture/windows_capture.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/video_capture/capture_queue.hpp>

#ifdef _WIN32
#include "d3d11_support.hpp"
#include "dxgi_desktop_duplication.hpp"
#include "wgc_capture.hpp"
#endif

namespace nimrtc::video_capture {

#ifdef _WIN32

namespace {

/** The plugin ids the two backends register under. */
constexpr const char* kDxgiBackendId = "windows-dxgi";
constexpr const char* kWgcBackendId  = "windows-wgc";

std::int64_t now_us() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/**
 * @brief The device id of the WGC backend for a *window*.
 *
 * A window is identified by its `HWND`, which is a per-process value
 * and therefore useless as a cross-process device id.  The WGC backend
 * accepts a window handle spelled `"hwnd:0x1234"` instead, and the
 * *monitor* ids are the DXGI device names (`\\.\DISPLAY1`), which are
 * stable across processes.
 */
constexpr const char* kWindowDevicePrefix = "hwnd:";

/** Parse a `"hwnd:0x..."` device id. */
bool parse_window_device_id(const std::string& id, std::uintptr_t& out) {
    if (id.rfind(kWindowDevicePrefix, 0) != 0) return false;
    const std::string digits = id.substr(std::strlen(kWindowDevicePrefix));
    if (digits.empty()) return false;
    try {
        std::size_t consumed = 0;
        const unsigned long long value = std::stoull(digits, &consumed, 0);
        if (consumed != digits.size()) return false;
        out = static_cast<std::uintptr_t>(value);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// WindowsCapture::Impl
// ---------------------------------------------------------------------------

class WindowsCapture::Impl {
public:
    Impl(plugins::CaptureConfig cfg, WindowsCaptureOptions opts)
        : cfg_(std::move(cfg)), opts_(opts) {
        queue_.set_drop_policy(cfg_.drop_policy);
        if (cfg_.queue_depth > 0) queue_.set_depth(cfg_.queue_depth);
    }

    ~Impl() { stop_thread(); }

    // ---- Device lifecycle -------------------------------------------------

    bool open_device() noexcept {
        switch (opts_.backend) {
            case WindowsCaptureBackend::kDxgiDesktopDuplication:
                return open_dxgi();
            case WindowsCaptureBackend::kWgc:
                return open_wgc();
        }
        return false;
    }

    void close_device() noexcept {
        stop_thread();
        if (dxgi_) {
            dxgi_->close();
            dxgi_.reset();
        }
        if (wgc_) {
            wgc_->close();
            wgc_.reset();
        }
        outputs_.clear();
        open_ = false;
    }

    // ---- Delivery ---------------------------------------------------------

    void start_thread() noexcept {
        if (running_.exchange(true)) return;
        thread_ = std::thread([this] { capture_loop(); });
    }

    void stop_thread() noexcept {
        if (!running_.exchange(false)) {
            if (thread_.joinable()) thread_.join();
            return;
        }
        stop_cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    void capture_loop() noexcept {
        while (running_.load(std::memory_order_acquire)) {
            bool delivered = false;
            switch (opts_.backend) {
                case WindowsCaptureBackend::kDxgiDesktopDuplication:
                    delivered = pump_dxgi();
                    break;
                case WindowsCaptureBackend::kWgc:
                    delivered = pump_wgc();
                    break;
            }
            if (!delivered && !running_.load(std::memory_order_acquire)) break;
        }
    }

    // ---- DXGI -------------------------------------------------------------

    bool open_dxgi() noexcept {
        outputs_ = wcap::enumerate_dxgi_outputs();
        if (outputs_.empty()) {
            report_error(WindowsCaptureError::kDeviceNotFound,
                         "windows capture: no display available for DXGI "
                         "duplication");
            return false;
        }

        const wcap::DxgiOutput* chosen = nullptr;
        if (!cfg_.device_id.empty()) {
            for (const auto& out : outputs_) {
                if (out.device_id == cfg_.device_id) {
                    chosen = &out;
                    break;
                }
            }
            if (chosen == nullptr) {
                report_error(WindowsCaptureError::kDeviceNotFound,
                             "windows capture: no such display '" +
                                 cfg_.device_id + "'");
                return false;
            }
        } else {
            chosen = &outputs_.front();
        }

        dxgi_ = std::make_unique<wcap::DxgiDesktopDuplication>();
        if (!dxgi_->open(*chosen)) {
            report_error(WindowsCaptureError::kD3dDeviceFailed,
                         "windows capture: DXGI duplication could not be "
                         "opened");
            dxgi_.reset();
            return false;
        }

        // The negotiated size: the device's native size, unless the
        // caller asked for something smaller (the region path crops).
        negotiated_width_ = dxgi_->width();
        negotiated_height_ = dxgi_->height();
        if (cfg_.format.width != 0 && cfg_.format.height != 0 &&
            cfg_.region.width == 0 && cfg_.region.height == 0) {
            // The duplication always delivers the full output; the
            // requested size is honoured by the region path below.
            if (cfg_.format.width > negotiated_width_ ||
                cfg_.format.height > negotiated_height_) {
                report_error(WindowsCaptureError::kUnsupportedFormat,
                             "windows capture: the requested format is larger "
                             "than the display");
                dxgi_->close();
                dxgi_.reset();
                return false;
            }
        }
        if (cfg_.region.width != 0 && cfg_.region.height != 0) {
            negotiated_width_ = cfg_.region.width;
            negotiated_height_ = cfg_.region.height;
        }

        open_ = true;
        return true;
    }

    bool pump_dxgi() noexcept {
        if (!dxgi_) return false;

        wcap::ComPtr<ID3D11Texture2D> texture;
        plugins::CaptureDirtyRects dirty;
        std::uint64_t frame_index = 0;
        WindowsCaptureError error = WindowsCaptureError::kNone;

        const bool acquired =
            dxgi_->acquire_next(texture, dirty, frame_index, kAcquireTimeoutMs,
                                error);
        if (!acquired) {
            if (error == WindowsCaptureError::kDeviceLost) {
                ++stats_.device_errors;
                report_error(error, "windows capture: the DXGI duplication was "
                                     "lost (display change or GPU reset)");
                return false;
            }
            if (error != WindowsCaptureError::kNone) {
                ++stats_.device_errors;
                report_error(error, "windows capture: the DXGI acquisition failed");
            }
            return false;
        }

        deliver_texture(texture, dirty);
        dxgi_->release_frame();
        return true;
    }

    // ---- WGC --------------------------------------------------------------

    bool open_wgc() noexcept {
        outputs_ = wcap::enumerate_dxgi_outputs();

        wcap::WgcTargetKind target = wcap::WgcTargetKind::kMonitor;
        void* handle = nullptr;

        std::uintptr_t window = 0;
        if (parse_window_device_id(cfg_.device_id, window)) {
            target = wcap::WgcTargetKind::kWindow;
            handle = reinterpret_cast<void*>(window);
        } else {
            const wcap::DxgiOutput* chosen = nullptr;
            if (!cfg_.device_id.empty()) {
                for (const auto& out : outputs_) {
                    if (out.device_id == cfg_.device_id) {
                        chosen = &out;
                        break;
                    }
                }
                if (chosen == nullptr) {
                    report_error(WindowsCaptureError::kDeviceNotFound,
                                 "windows capture: no such display '" +
                                     cfg_.device_id + "'");
                    return false;
                }
            } else if (!outputs_.empty()) {
                chosen = &outputs_.front();
            }
            if (chosen == nullptr) {
                report_error(WindowsCaptureError::kDeviceNotFound,
                             "windows capture: no display available for WGC");
                return false;
            }
            handle = reinterpret_cast<void*>(chosen->monitor);
            negotiated_width_ = chosen->width;
            negotiated_height_ = chosen->height;
        }

        wgc_ = std::make_unique<wcap::WgcCapture>();
        WindowsCaptureError error = WindowsCaptureError::kNone;
        const std::uint32_t want_width =
            (cfg_.format.width != 0) ? cfg_.format.width : negotiated_width_;
        const std::uint32_t want_height =
            (cfg_.format.height != 0) ? cfg_.format.height : negotiated_height_;

        if (!wgc_->open(target, handle, want_width, want_height,
                        cfg_.want_cursor, opts_.require_consent, error)) {
            // The consent refusal and the missing device are distinct
            // conditions and get distinct codes 锟?the host reacts
            // differently to "ask the user" and to "that display is
            // gone".
            report_error(error,
                         error == WindowsCaptureError::kConsentDenied
                             ? "windows capture: the capture consent was refused"
                             : "windows capture: the WGC session could not be opened");
            wgc_.reset();
            return false;
        }

        if (target == wcap::WgcTargetKind::kWindow) {
            negotiated_width_ = wgc_->width();
            negotiated_height_ = wgc_->height();
        }
        if (cfg_.region.width != 0 && cfg_.region.height != 0) {
            negotiated_width_ = cfg_.region.width;
            negotiated_height_ = cfg_.region.height;
        }

        open_ = true;
        return true;
    }

    bool pump_wgc() noexcept {
        if (!wgc_) return false;

        wcap::ComPtr<ID3D11Texture2D> texture;
        WindowsCaptureError error = WindowsCaptureError::kNone;
        if (!wgc_->try_get_next(texture, error)) {
            if (error != WindowsCaptureError::kNone) {
                ++stats_.device_errors;
                report_error(error, "windows capture: the WGC frame retrieval failed");
            }
            // No frame yet: the capture thread spins briefly rather than
            // burning a core.  The tick thread is never involved.
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            return false;
        }

        // WGC reports no dirty rectangles 锟?the honest answer is "the
        // whole frame changed".
        plugins::CaptureDirtyRects dirty;
        dirty.full_frame = true;
        dirty.rects.clear();

        deliver_texture(texture, dirty);
        return true;
    }

    // ---- Delivery ---------------------------------------------------------

    /**
     * @brief Deliver one captured texture.
     *
     * The GPU path copies the texture *on the device* into a pooled
     * texture and hands that to the GPU callback.  The CPU path reads
     * the texture back and pushes it through the queue.
     */
    void deliver_texture(const wcap::ComPtr<ID3D11Texture2D>& texture,
                         const plugins::CaptureDirtyRects& dirty) noexcept {
        const std::uint32_t seq = next_seq_++;
        const std::int64_t ts = now_us();

        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);

        const wcap::SourceRegion region = source_region(desc.Width, desc.Height);

        // ---- GPU path ----------------------------------------------------
        if (opts_.allow_zero_copy && gpu_cb_) {
            wcap::GpuTexturePool& pool = texture_pool();
            wcap::ComPtr<ID3D11Texture2D> owned =
                pool.acquire(region.width, region.height, desc.Format);
            if (owned &&
                wcap::copy_texture_on_device(immediate_context(), texture.Get(),
                                            region, owned.Get())) {
                const auto format = wcap::pixel_format_from_dxgi(desc.Format);
                auto buffer = wcap::make_gpu_buffer(owned, region.width,
                                                   region.height, format);
                if (buffer) {
                    GpuCaptureFrame frame;
                    frame.gpu_buffer = std::move(buffer);
                    frame.dirty = dirty;
                    frame.capture_ts_us = ts;
                    frame.frame_seq = seq;
                    ++stats_.frames_captured;
                    ++stats_.frames_zero_copy;
                    gpu_cb_(std::move(frame));
                    return;
                }
            }
            // The GPU staging failed 锟?fall through to the CPU path and
            // *count the copy*, so the statistics tell the truth.
        }

        // ---- CPU path ----------------------------------------------------
        std::vector<std::uint8_t> pixels;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::int32_t stride = 0;
        if (!wcap::read_texture_to_cpu(immediate_context(), texture.Get(),
                                      region, pixels, width, height, stride)) {
            ++stats_.device_errors;
            report_error(WindowsCaptureError::kCaptureFailed,
                         "windows capture: the frame could not be staged to "
                         "host memory");
            return;
        }

        stats_.bytes_copied += static_cast<std::uint64_t>(pixels.size());

        const auto outcome = queue_.push(pixels.data(), pixels.size(), stride,
                                         width, height, ts, seq, dirty);
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
        ++stats_.frames_cpu_staged;

        if (cb_) {
            plugins::CaptureFrame view;
            if (queue_.peek(view)) {
                view.format = plugins::VideoPixelFormat::kBGRA;
                view.gpu_backend = plugins::GpuBackend::kNone;
                view.has_gpu_handle = false;
                view.gpu_handle = nullptr;
                cb_(view);
            }
        }
    }

    wcap::SourceRegion source_region(std::uint32_t src_w,
                                    std::uint32_t src_h) const noexcept {
        wcap::SourceRegion region;
        region.width = src_w;
        region.height = src_h;
        if (cfg_.region.width != 0 && cfg_.region.height != 0) {
            region.x = cfg_.region.x;
            region.y = cfg_.region.y;
            region.width = cfg_.region.width;
            region.height = cfg_.region.height;
        }
        return region;
    }

    wcap::GpuTexturePool& texture_pool() noexcept {
        if (!pool_initialised_) {
            pool_.set_device(current_device());
            pool_initialised_ = true;
        }
        return pool_;
    }

    wcap::ComPtr<ID3D11DeviceContext> immediate_context() const noexcept {
        if (dxgi_) return dxgi_->d3d().immediate;
        if (wgc_) return wgc_->d3d().immediate;
        return nullptr;
    }

    wcap::ComPtr<ID3D11Device> current_device() const noexcept {
        if (dxgi_) return dxgi_->d3d().device;
        if (wgc_) return wgc_->d3d().device;
        return nullptr;
    }

    // ---- Errors -----------------------------------------------------------

    void report_error(WindowsCaptureError error, std::string message) noexcept {
        last_error_ = error;
        auto cb = error_cb_;
        if (cb) cb(error, message);
    }

    // ---- State ------------------------------------------------------------

    plugins::CaptureConfig cfg_;
    WindowsCaptureOptions opts_;

    CaptureQueue queue_{2};
    plugins::CaptureFrameCallback cb_;
    GpuCaptureFrameCallback gpu_cb_;
    WindowsCaptureErrorCallback error_cb_;
    plugins::CaptureStats stats_{};

    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;

    std::vector<wcap::DxgiOutput> outputs_;
    std::unique_ptr<wcap::DxgiDesktopDuplication> dxgi_;
    std::unique_ptr<wcap::WgcCapture> wgc_;
    wcap::GpuTexturePool pool_;
    bool pool_initialised_ = false;

    std::uint32_t negotiated_width_ = 0;
    std::uint32_t negotiated_height_ = 0;
    std::uint32_t next_seq_ = 0;
    bool open_ = false;
    WindowsCaptureError last_error_ = WindowsCaptureError::kNone;

    /** How long the DXGI acquire waits for a frame.  The wait happens
     *  on the capture thread, never on the engine's tick thread. */
    static constexpr std::uint32_t kAcquireTimeoutMs = 100;
};

// ---------------------------------------------------------------------------
// WindowsCapture
// ---------------------------------------------------------------------------

WindowsCapture::WindowsCapture(plugins::CaptureConfig cfg,
                               WindowsCaptureOptions opts) noexcept
    : impl_(std::make_unique<Impl>(std::move(cfg), opts)),
      cfg_(impl_->cfg_),
      opts_(impl_->opts_) {}

WindowsCapture::~WindowsCapture() = default;

const char* WindowsCapture::name() const noexcept {
    switch (opts_.backend) {
        case WindowsCaptureBackend::kDxgiDesktopDuplication:
            return "windows-dxgi-capture";
        case WindowsCaptureBackend::kWgc:
            return "windows-wgc-capture";
    }
    return "windows-capture";
}

plugins::Status WindowsCapture::open() noexcept {
    if (impl_->open_) return plugins::kOk;

    if (!impl_->open_device()) {
        // The device is not usable.  The error has already been
        // reported through the error callback with its distinct code.
        return plugins::kErrHardwareError;
    }

    impl_->next_seq_ = 0;
    impl_->last_error_ = WindowsCaptureError::kNone;
    impl_->start_thread();
    return plugins::kOk;
}

void WindowsCapture::close() noexcept {
    impl_->close_device();
    impl_->queue_.clear();
}

bool WindowsCapture::is_open() const noexcept { return impl_->open_; }

plugins::CaptureConfig WindowsCapture::config() const noexcept {
    plugins::CaptureConfig cfg = impl_->cfg_;
    if (impl_->negotiated_width_ != 0) {
        cfg.format.width = impl_->negotiated_width_;
        cfg.format.height = impl_->negotiated_height_;
    }
    if (cfg.format.fps == 0) cfg.format.fps = 30;
    if (cfg.format.tag.empty()) {
        cfg.format.tag = (opts_.backend == WindowsCaptureBackend::kWgc)
                             ? "bgra-wgc"
                             : "bgra-dxgi";
    }
    cfg.format.zero_copy = opts_.allow_zero_copy;
    cfg.format.has_dirty_rects =
        (opts_.backend == WindowsCaptureBackend::kDxgiDesktopDuplication);
    return cfg;
}

std::vector<plugins::CaptureDevice>
WindowsCapture::enumerate_devices() const noexcept {
    std::vector<plugins::CaptureDevice> devices;

    if (opts_.backend == WindowsCaptureBackend::kWgc) {
        // The WGC backend can capture a window as well as a display.
        // Windows are identified by their handle, which is only
        // meaningful inside the capturing process, so the backend
        // advertises the displays and the host passes a window id
        // ("hwnd:0x...") directly when it wants one.
        for (const auto& out : wcap::enumerate_dxgi_outputs()) {
            plugins::CaptureDevice device;
            device.id = out.device_id;
            device.name = out.name;
            device.kind = plugins::CaptureDeviceKind::kScreen;
            device.max_width = out.width;
            device.max_height = out.height;
            device.max_fps = out.refresh_hz;
            devices.push_back(std::move(device));
        }
        return devices;
    }

    for (const auto& out : wcap::enumerate_dxgi_outputs()) {
        plugins::CaptureDevice device;
        device.id = out.device_id;
        device.name = out.name;
        device.kind = plugins::CaptureDeviceKind::kScreen;
        device.max_width = out.width;
        device.max_height = out.height;
        device.max_fps = out.refresh_hz;
        devices.push_back(std::move(device));
    }
    return devices;
}

std::vector<plugins::CaptureFormat>
WindowsCapture::supported_formats(const std::string& device_id) const noexcept {
    std::vector<plugins::CaptureFormat> formats;

    std::uint32_t max_w = 0;
    std::uint32_t max_h = 0;
    for (const auto& device : enumerate_devices()) {
        if (!device_id.empty() && device.id != device_id) continue;
        if (device.max_width > max_w) max_w = device.max_width;
        if (device.max_height > max_h) max_h = device.max_height;
    }
    if (max_w == 0 || max_h == 0) return formats;

    // The device's native size plus a couple of common ladder rungs,
    // so the engine can pick a resolution before the first frame.
    const std::uint32_t sizes[][2] = {
        {1280, 720}, {1920, 1080}, {max_w, max_h},
    };
    for (const auto& s : sizes) {
        if (s[0] > max_w || s[1] > max_h) continue;
        plugins::CaptureFormat format;
        format.width = s[0];
        format.height = s[1];
        format.fps = 30;
        format.tag = (opts_.backend == WindowsCaptureBackend::kWgc) ? "bgra-wgc"
                                                                     : "bgra-dxgi";
        format.zero_copy = true;
        format.has_dirty_rects =
            (opts_.backend == WindowsCaptureBackend::kDxgiDesktopDuplication);
        // A format that is not the native size is delivered by cropping
        // the source, which is a device-side copy 锟?still zero-copy in
        // the sense that the pixels never reach host memory.
        formats.push_back(std::move(format));
    }
    return formats;
}

plugins::CaptureCapabilities WindowsCapture::capabilities() const noexcept {
    plugins::CaptureCapabilities caps;
    caps.zero_copy = true;
    caps.dirty_rects =
        (opts_.backend == WindowsCaptureBackend::kDxgiDesktopDuplication);
    // DXGI does not composite the pointer; WGC does.
    caps.cursor_composited = (opts_.backend == WindowsCaptureBackend::kWgc);
    caps.region_selection = true;
    caps.enumeration = true;
    caps.max_pixels = 0;   // unbounded 锟?bounded by the device itself
    caps.max_fps = 0;
    caps.backend = std::string(windows_capture_backend_name(opts_.backend));
    return caps;
}

plugins::Status WindowsCapture::pop_frame(plugins::CaptureFrame& out) noexcept {
    if (!impl_->open_) return plugins::kErrNotReady;
    if (!impl_->queue_.pop(out)) return plugins::kErrNotReady;
    out.format = plugins::VideoPixelFormat::kBGRA;
    out.gpu_backend = plugins::GpuBackend::kNone;
    out.has_gpu_handle = false;
    out.gpu_handle = nullptr;
    return plugins::kOk;
}

void WindowsCapture::set_frame_callback(plugins::CaptureFrameCallback cb) noexcept {
    impl_->cb_ = std::move(cb);
}

plugins::CaptureStats WindowsCapture::stats() const noexcept {
    return impl_->stats_;
}

void WindowsCapture::set_gpu_frame_callback(GpuCaptureFrameCallback cb) noexcept {
    impl_->gpu_cb_ = std::move(cb);
}

void WindowsCapture::set_error_callback(WindowsCaptureErrorCallback cb) noexcept {
    impl_->error_cb_ = std::move(cb);
}

WindowsCaptureError WindowsCapture::last_error() const noexcept {
    return impl_->last_error_;
}

plugins::Status WindowsCapture::reopen() noexcept {
    impl_->close_device();
    impl_->stats_ = plugins::CaptureStats{};
    return open();
}

#else // !_WIN32

// ---------------------------------------------------------------------------
// The non-Windows stub.
//
// The header is portable, so a host that names the Windows backend on a
// Linux build gets a clean "not supported" instead of a link error.
// ---------------------------------------------------------------------------

namespace {

constexpr WindowsCaptureError kUnsupported =
    WindowsCaptureError::kUnsupportedFormat;

} // anonymous namespace

class WindowsCapture::Impl {
public:
    Impl(plugins::CaptureConfig cfg, WindowsCaptureOptions opts)
        : cfg_(std::move(cfg)), opts_(opts) {}

    plugins::CaptureConfig cfg_;
    WindowsCaptureOptions opts_;
    CaptureQueue queue_{2};
    plugins::CaptureFrameCallback cb_;
    GpuCaptureFrameCallback gpu_cb_;
    WindowsCaptureErrorCallback error_cb_;
    plugins::CaptureStats stats_{};
    bool open_ = false;
    WindowsCaptureError last_error_ = kUnsupported;
};

WindowsCapture::WindowsCapture(plugins::CaptureConfig cfg,
                               WindowsCaptureOptions opts) noexcept
    : impl_(std::make_unique<Impl>(std::move(cfg), opts)),
      cfg_(impl_->cfg_),
      opts_(impl_->opts_) {}

WindowsCapture::~WindowsCapture() = default;

const char* WindowsCapture::name() const noexcept { return "windows-capture"; }

plugins::Status WindowsCapture::open() noexcept {
    if (impl_->error_cb_) {
        impl_->error_cb_(kUnsupported,
                         "windows capture: this build does not support the "
                         "Windows capture backends");
    }
    return plugins::kErrUnsupported;
}

void WindowsCapture::close() noexcept { impl_->open_ = false; }

bool WindowsCapture::is_open() const noexcept { return false; }

plugins::CaptureConfig WindowsCapture::config() const noexcept {
    return impl_->cfg_;
}

std::vector<plugins::CaptureDevice>
WindowsCapture::enumerate_devices() const noexcept {
    return {};
}

std::vector<plugins::CaptureFormat>
WindowsCapture::supported_formats(const std::string&) const noexcept {
    return {};
}

plugins::CaptureCapabilities WindowsCapture::capabilities() const noexcept {
    plugins::CaptureCapabilities caps;
    caps.backend = std::string(windows_capture_backend_name(opts_.backend));
    return caps;
}

plugins::Status WindowsCapture::pop_frame(plugins::CaptureFrame&) noexcept {
    return plugins::kErrNotReady;
}

void WindowsCapture::set_frame_callback(plugins::CaptureFrameCallback cb) noexcept {
    impl_->cb_ = std::move(cb);
}

plugins::CaptureStats WindowsCapture::stats() const noexcept {
    return impl_->stats_;
}

void WindowsCapture::set_gpu_frame_callback(GpuCaptureFrameCallback cb) noexcept {
    impl_->gpu_cb_ = std::move(cb);
}

void WindowsCapture::set_error_callback(WindowsCaptureErrorCallback cb) noexcept {
    impl_->error_cb_ = std::move(cb);
}

WindowsCaptureError WindowsCapture::last_error() const noexcept {
    return impl_->last_error_;
}

plugins::Status WindowsCapture::reopen() noexcept { return open(); }

#endif // _WIN32

// ---------------------------------------------------------------------------
// Factory + registration
// ---------------------------------------------------------------------------

std::string_view WindowsCaptureFactory::id() const noexcept {
    switch (backend_) {
        case WindowsCaptureBackend::kDxgiDesktopDuplication:
            return "windows-dxgi";
        case WindowsCaptureBackend::kWgc:
            return "windows-wgc";
    }
    return "windows-unknown";
}

std::string_view WindowsCaptureFactory::display_name() const noexcept {
    switch (backend_) {
        case WindowsCaptureBackend::kDxgiDesktopDuplication:
            return "Windows DXGI Desktop Duplication (display, dirty rects)";
        case WindowsCaptureBackend::kWgc:
            return "Windows Graphics Capture (display/window, cursor composited)";
    }
    return "Windows capture";
}

plugins::IVideoCapture* WindowsCaptureFactory::create(
    plugins::CaptureConfig cfg) const {
    WindowsCaptureOptions opts;
    opts.backend = backend_;
    return new WindowsCapture(std::move(cfg), opts);
}

void register_windows_capture_plugins() noexcept {
    static bool registered = false;
    if (registered) return;
    registered = true;

    static const WindowsCaptureFactory dxgi_factory{
        WindowsCaptureBackend::kDxgiDesktopDuplication};
    static const WindowsCaptureFactory wgc_factory{
        WindowsCaptureBackend::kWgc};

    core::PluginRegistry::instance().register_video_capture(
        dxgi_factory.id(), &dxgi_factory);
    core::PluginRegistry::instance().register_video_capture(
        wgc_factory.id(), &wgc_factory);
}

} // namespace nimrtc::video_capture
