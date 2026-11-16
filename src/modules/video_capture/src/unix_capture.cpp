// modules/video_capture/src/unix_capture.cpp
// ----------------------------------------------------------------------------
// The portable base class of the macOS (A4) and Linux (A5) capture
// backends.
//
// The class itself is portable: it owns the capture thread, drives the
// `UnixCaptureCore`, and implements the whole of
// `plugins::IVideoCapture`.  The six `platform_*` hooks are what the
// platform adapters implement.
//
// The three factories are declared in the public header; their `create()`
// bodies live in the platform adapters, because only those know the
// concrete device type.  A host that names a backend this build does not
// have gets a device that reports `kErrUnsupported` -- a clean error,
// not a link error.
// ----------------------------------------------------------------------------

#include "platform/unix_capture_factory.hpp"

#include <chrono>
#include <cstdlib>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/video_capture/unix_capture.hpp>
#include <thread>

namespace nimrtc::video_capture {

namespace {

/** How long the capture thread sleeps when the platform has nothing to
 *  deliver.  The wait happens on the capture thread, never on the
 *  engine's tick thread. */
constexpr auto kIdleSleep = std::chrono::milliseconds(4);

/**
 * @brief A capture device for a backend this build does not have.
 *
 * It answers every query honestly ("no devices", "no formats") and
 * fails `open()` with `kErrUnsupported`, so a host that asks for
 * `"linux-pipewire"` on a Windows build gets a clear failure rather
 * than a link error or a silent black screen.
 */
class UnsupportedCapture final : public plugins::IVideoCapture {
  public:
    UnsupportedCapture(plugins::CaptureConfig cfg, std::string_view id,
                       std::string_view name) noexcept
        : cfg_(std::move(cfg)), id_(id), name_(name) {}

    const char* name() const noexcept override { return name_.data(); }

    plugins::Status open() noexcept override { return plugins::kErrUnsupported; }

    void close() noexcept override {}

    [[nodiscard]] std::vector<plugins::CaptureDevice> enumerate_devices() const noexcept override {
        return {};
    }

    [[nodiscard]] std::vector<plugins::CaptureFormat>
    supported_formats(const std::string&) const noexcept override {
        return {};
    }

    [[nodiscard]] plugins::CaptureCapabilities capabilities() const noexcept override {
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
// UnixCaptureDevice
// ---------------------------------------------------------------------------

UnixCaptureDevice::UnixCaptureDevice(plugins::CaptureConfig cfg, UnixCaptureOptions opts) noexcept
    : core_(std::move(cfg), opts) {}

UnixCaptureDevice::~UnixCaptureDevice() {
    stop_thread();
}

const char* UnixCaptureDevice::name() const noexcept {
    return plugin_name().data();
}

plugins::Status UnixCaptureDevice::open() noexcept {
    if (open_)
        return plugins::kOk;

    // The capabilities come first: the negotiation reads them, and a
    // backend that cannot deliver a format must say so *before* the
    // first frame.
    core_.set_capabilities(platform_capabilities());

    UnixCaptureError error = UnixCaptureError::kNone;
    const plugins::Status status = platform_open_device(error);
    if (status != plugins::kOk) {
        if (error != UnixCaptureError::kNone) {
            report_error(error, "unix capture: the device could not be opened");
        }
        platform_close_device();
        return status;
    }

    last_error_ = UnixCaptureError::kNone;

    // The platform's own producer starts before the capture thread, so
    // the first poll finds a frame rather than an empty queue.
    platform_start();

    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { capture_loop(); });

    open_ = true;
    return plugins::kOk;
}

void UnixCaptureDevice::close() noexcept {
    stop_thread();
    platform_stop();
    platform_close_device();
    open_ = false;
}

void UnixCaptureDevice::stop_thread() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        if (thread_.joinable())
            thread_.join();
        return;
    }
    if (thread_.joinable())
        thread_.join();
}

void UnixCaptureDevice::capture_loop() noexcept {
    while (running_.load(std::memory_order_acquire)) {
        const plugins::Status status = platform_poll_frame();
        if (status == plugins::kOk)
            continue;
        if (status == plugins::kErrNotReady) {
            // Nothing to deliver right now.  The capture thread sleeps
            // rather than spinning; the engine's tick thread is never
            // involved either way.
            std::this_thread::sleep_for(kIdleSleep);
            continue;
        }
        // Any other status is a failure: the device stops and reports
        // it.  It never crashes and never spins.
        core_.note_device_error();
        report_error(UnixCaptureError::kCaptureFailed, "unix capture: the capture device failed");
        running_.store(false, std::memory_order_release);
        break;
    }
}

std::vector<plugins::CaptureDevice> UnixCaptureDevice::enumerate_devices() const noexcept {
    return platform_enumerate_devices();
}

std::vector<plugins::CaptureFormat>
UnixCaptureDevice::supported_formats(const std::string& device_id) const noexcept {
    return platform_supported_formats(device_id);
}

plugins::CaptureCapabilities UnixCaptureDevice::capabilities() const noexcept {
    return platform_capabilities();
}

bool UnixCaptureDevice::is_open() const noexcept {
    return open_;
}

plugins::CaptureConfig UnixCaptureDevice::config() const noexcept {
    return core_.config();
}

plugins::Status UnixCaptureDevice::pop_frame(plugins::CaptureFrame& out) noexcept {
    if (!open_)
        return plugins::kErrNotReady;
    return core_.pop_frame(out);
}

void UnixCaptureDevice::set_frame_callback(plugins::CaptureFrameCallback cb) noexcept {
    core_.set_frame_callback(std::move(cb));
}

plugins::CaptureStats UnixCaptureDevice::stats() const noexcept {
    return core_.stats();
}

void UnixCaptureDevice::set_error_callback(UnixCaptureErrorCallback cb) noexcept {
    error_cb_ = std::move(cb);
}

UnixCaptureError UnixCaptureDevice::last_error() const noexcept {
    return last_error_;
}

void UnixCaptureDevice::report_error(UnixCaptureError error, std::string_view message) noexcept {
    last_error_ = error;
    auto cb = error_cb_;
    if (cb)
        cb(error, message);
}

plugins::Status UnixCaptureDevice::reopen() noexcept {
    close();
    core_.reset_stats();
    core_.note_device_restart();
    return open();
}

// ---------------------------------------------------------------------------
// Availability
// ---------------------------------------------------------------------------

bool screen_capture_kit_available() noexcept {
#if defined(NIMRTC_HAS_SCREEN_CAPTURE_KIT)
    return true;
#else
    return false;
#endif
}

bool pipewire_capture_available() noexcept {
#if defined(NIMRTC_HAS_PIPEWIRE_CAPTURE)
    return true;
#else
    return false;
#endif
}

bool x11_capture_available() noexcept {
#if defined(NIMRTC_HAS_X11_CAPTURE)
    return true;
#else
    return false;
#endif
}

// ---------------------------------------------------------------------------
// Factories + registration
// ---------------------------------------------------------------------------

plugins::IVideoCapture* ScreenCaptureKitFactory::create(plugins::CaptureConfig cfg) const {
    if (!screen_capture_kit_available()) {
        return new UnsupportedCapture(std::move(cfg), id(),
                                      "macOS ScreenCaptureKit (not supported by this build)");
    }
    return detail::create_screen_capture_kit(std::move(cfg));
}

plugins::IVideoCapture* PipeWireCaptureFactory::create(plugins::CaptureConfig cfg) const {
    if (!pipewire_capture_available()) {
        return new UnsupportedCapture(std::move(cfg), id(),
                                      "Linux PipeWire capture (not supported by this build)");
    }
    return detail::create_pipewire_capture(std::move(cfg));
}

plugins::IVideoCapture* X11CaptureFactory::create(plugins::CaptureConfig cfg) const {
    if (!x11_capture_available()) {
        return new UnsupportedCapture(std::move(cfg), id(),
                                      "Linux X11 capture (not supported by this build)");
    }
    return detail::create_x11_capture(std::move(cfg));
}

void register_unix_capture_plugins() noexcept {
    static bool registered = false;
    if (registered)
        return;
    registered = true;

    static const ScreenCaptureKitFactory sck_factory{};
    static const PipeWireCaptureFactory pipewire_factory{};
    static const X11CaptureFactory x11_factory{};

    // Only the backends this build actually supports are registered, so
    // a host that lists the capture plug-ins sees exactly what it can
    // use.
    if (screen_capture_kit_available()) {
        core::PluginRegistry::instance().register_video_capture(sck_factory.id(), &sck_factory);
    }
    if (pipewire_capture_available()) {
        core::PluginRegistry::instance().register_video_capture(pipewire_factory.id(),
                                                                &pipewire_factory);
    }
    if (x11_capture_available()) {
        core::PluginRegistry::instance().register_video_capture(x11_factory.id(), &x11_factory);
    }
}

} // namespace nimrtc::video_capture
