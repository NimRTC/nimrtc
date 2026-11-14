// modules/video_capture/src/platform/linux/x11_capture.cpp
// ----------------------------------------------------------------------------
// The Linux X11 capture device (remote-desktop plan A5, the fallback).
//
// The adapter is *thin*: it opens the display, grabs the window with
// `XGetImage`, converts the `ZPixmap` to BGRA, and hands the result to
// the portable core.  The queue, the drop policy, the dirty-rect
// bookkeeping, the statistics and the negotiation all come from the
// portable core.
//
// ## The pixel format
//
// `XGetImage` returns a `ZPixmap` whose pixel layout depends on the
// server's visual: 24-bit TrueColor is nearly always BGRX (8 bits per
// channel, in blue-green-red order), and 32-bit is BGRA.  The adapter
// handles both and normalises to BGRA, because that is the format every
// desktop compositor produces natively and the one the pipeline wants.
//
// ## The dirty rectangles
//
// X11 has no "what changed" information, so the device reports
// `dirty_rects = false` and the *portable pixel-diff* supplies the
// rectangles.  The plan asks for exactly this (A7).
// ----------------------------------------------------------------------------

#if defined(__linux__)

#    include "x11_capture.hpp"

#    include <X11/Xlib.h>
#    include <X11/Xutil.h>
#    include <chrono>
#    include <cstdlib>
#    include <cstring>

namespace nimrtc::video_capture {

namespace {

/** The device-id prefix the backend hands out. */
constexpr const char* kRootPrefix = "x11-root:";

/** The format tag the backend reports. */
constexpr const char* kFormatTag = "bgra-x11";

std::int64_t now_us() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/** True iff the process has an X display to talk to. */
bool have_x_display() noexcept {
    const char* display = std::getenv("DISPLAY");
    return display != nullptr && display[0] != '\0';
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// X11Capture::Impl
// ---------------------------------------------------------------------------

class X11Capture::Impl {
  public:
    Impl(plugins::CaptureConfig cfg, UnixCaptureOptions opts) : cfg_(std::move(cfg)), opts_(opts) {}

    ~Impl() { close_device(); }

    // ---- Enumeration ------------------------------------------------------

    std::vector<plugins::CaptureDevice> enumerate_devices() const noexcept {
        std::vector<plugins::CaptureDevice> devices;

        // The X11 path can capture the whole virtual desktop (the root
        // window) and any individual window.  The root window is the
        // one the engine wants by default, so it is listed first.
        Display* display = XOpenDisplay(nullptr);
        if (display == nullptr)
            return devices;

        const int screen = DefaultScreen(display);
        plugins::CaptureDevice root;
        root.id = std::string(kRootPrefix) + std::to_string(screen);
        root.name = "X11 root window (screen " + std::to_string(screen) + ")";
        root.kind = plugins::CaptureDeviceKind::kScreen;
        root.max_width = static_cast<std::uint32_t>(DisplayWidth(display, screen));
        root.max_height = static_cast<std::uint32_t>(DisplayHeight(display, screen));
        root.max_fps = 60u;
        devices.push_back(std::move(root));

        XCloseDisplay(display);
        return devices;
    }

    std::vector<plugins::CaptureFormat>
    supported_formats(const std::string& device_id) const noexcept {
        std::vector<plugins::CaptureFormat> formats;

        std::uint32_t max_w = 0;
        std::uint32_t max_h = 0;
        for (const auto& device : enumerate_devices()) {
            if (!device_id.empty() && device.id != device_id)
                continue;
            if (device.max_width > max_w)
                max_w = device.max_width;
            if (device.max_height > max_h)
                max_h = device.max_height;
        }
        if (max_w == 0 || max_h == 0)
            return formats;

        const std::uint32_t sizes[][2] = {
            {1280, 720},
            {1920, 1080},
            {max_w, max_h},
        };
        for (const auto& size : sizes) {
            if (size[0] > max_w || size[1] > max_h)
                continue;
            plugins::CaptureFormat format;
            format.width = size[0];
            format.height = size[1];
            format.fps = 30;
            format.tag = kFormatTag;
            // X11 delivers the frame through a CPU readback, so there
            // is no zero-copy path here.
            format.zero_copy = false;
            // The device reports no changed region; the portable
            // pixel-diff supplies the rectangles instead.
            format.has_dirty_rects = opts_.enable_pixel_diff;
            formats.push_back(std::move(format));
        }
        return formats;
    }

    // ---- Lifecycle --------------------------------------------------------

    plugins::Status open_device(UnixCaptureError& error) noexcept {
        if (!have_x_display()) {
            // Neither Wayland nor X11: the host has no display server.
            error = UnixCaptureError::kDisplayUnavailable;
            return plugins::kErrHardwareError;
        }

        display_ = XOpenDisplay(nullptr);
        if (display_ == nullptr) {
            error = UnixCaptureError::kDisplayUnavailable;
            return plugins::kErrHardwareError;
        }

        screen_ = DefaultScreen(display_);

        // The capture target: the root window of the screen, unless the
        // caller named a window.
        const std::string& id = cfg_.device_id;
        if (id.rfind(kRootPrefix, 0) == 0 || id.empty()) {
            window_ = RootWindow(display_, screen_);
        } else {
            // A window id ("0x...") is accepted as well.
            std::uint64_t raw = 0;
            bool parsed = false;
            try {
                raw = std::stoull(id, nullptr, 0);
                parsed = true;
            } catch (const std::exception&) {
                parsed = false;
            }
            if (!parsed) {
                error = UnixCaptureError::kDeviceNotFound;
                return plugins::kErrInvalidParam;
            }
            window_ = static_cast<Window>(raw);
        }

        // ---- The size ----------------------------------------------------
        if (window_ == RootWindow(display_, screen_)) {
            width_ = static_cast<std::uint32_t>(DisplayWidth(display_, screen_));
            height_ = static_cast<std::uint32_t>(DisplayHeight(display_, screen_));
        } else {
            XWindowAttributes attrs{};
            if (XGetWindowAttributes(display_, window_, &attrs) == 0) {
                error = UnixCaptureError::kDeviceNotFound;
                return plugins::kErrInvalidParam;
            }
            width_ = static_cast<std::uint32_t>(attrs.width);
            height_ = static_cast<std::uint32_t>(attrs.height);
        }

        if (width_ == 0 || height_ == 0) {
            error = UnixCaptureError::kDeviceNotFound;
            return plugins::kErrInvalidParam;
        }

        // The requested size is honoured when it is *smaller*; a larger
        // request is clamped down (never upscaled -- the portable
        // negotiation does the same).
        if (cfg_.format.width != 0 && cfg_.format.height != 0) {
            width_ = std::min(cfg_.format.width, width_);
            height_ = std::min(cfg_.format.height, height_);
        }

        plugins::CaptureFormat negotiated;
        negotiated.width = width_;
        negotiated.height = height_;
        negotiated.fps = cfg_.format.fps != 0 ? cfg_.format.fps : 30u;
        negotiated.tag = kFormatTag;
        negotiated.zero_copy = false;
        negotiated.has_dirty_rects = opts_.enable_pixel_diff;
        negotiated_format_ = negotiated;

        return plugins::kOk;
    }

    void close_device() noexcept {
        if (display_ != nullptr) {
            XCloseDisplay(display_);
            display_ = nullptr;
        }
        window_ = 0;
        width_ = 0;
        height_ = 0;
    }

    void start() noexcept {
        // The X11 path polls on the capture thread; there is no
        // separate producer to start.
    }

    void stop() noexcept {
        // Nothing to stop: the poll loop is the capture thread.
    }

    // ---- The frame pump ---------------------------------------------------

    plugins::Status poll_frame() noexcept {
        if (display_ == nullptr)
            return plugins::kErrHardwareError;

        XImage* image = XGetImage(display_, window_, 0, 0, static_cast<unsigned int>(width_),
                                  static_cast<unsigned int>(height_), AllPlanes, ZPixmap);
        if (image == nullptr) {
            // The window may have gone away (a closed application, a
            // changed layout).
            return plugins::kErrHardwareError;
        }

        const std::int64_t ts = now_us();
        const plugins::Status status = convert_to_bgra(image, ts);

        XDestroyImage(image);
        return status;
    }

    plugins::CaptureCapabilities capabilities() const noexcept {
        plugins::CaptureCapabilities caps;
        // X11 delivers the frame through a CPU readback.
        caps.zero_copy = false;
        // No native dirty rectangles -- the portable pixel-diff supplies
        // them.
        caps.dirty_rects = opts_.enable_pixel_diff;
        // X11 does not composite the pointer into the grab.
        caps.cursor_composited = false;
        caps.region_selection = true;
        caps.enumeration = true;
        caps.max_pixels = 0; // bounded by the device itself
        caps.max_fps = 0;
        caps.backend = "linux-x11";
        return caps;
    }

    [[nodiscard]] const plugins::CaptureFormat& negotiated_format() const noexcept {
        return negotiated_format_;
    }

    void set_core(UnixCaptureCore* core) noexcept { core_ptr_ = core; }

  private:
    /**
     * @brief Convert the `ZPixmap` into BGRA and hand it to the core.
     *
     * The `ZPixmap` is BGRX (24-bit) or BGRA (32-bit) depending on the
     * server's visual; the alpha byte is forced to 0xFF, because the
     * source picture leaves it undefined.
     */
    plugins::Status convert_to_bgra(XImage* image, std::int64_t ts) noexcept {
        if (image == nullptr)
            return plugins::kErrHardwareError;

        const std::uint32_t width = static_cast<std::uint32_t>(image->width);
        const std::uint32_t height = static_cast<std::uint32_t>(image->height);
        const std::uint32_t src_stride = static_cast<std::uint32_t>(image->bytes_per_line);

        if (width == 0 || height == 0 || image->data == nullptr) {
            return plugins::kErrHardwareError;
        }

        const bool has_alpha = (image->depth == 32);
        const std::uint32_t bpp = has_alpha ? 4u : 3u;

        bgra_.resize(static_cast<std::size_t>(width) * height * 4u);
        const std::uint8_t* src = image->data;

        for (std::uint32_t y = 0; y < height; ++y) {
            const std::uint8_t* src_row = src + static_cast<std::size_t>(y) * src_stride;
            std::uint8_t* dst_row = bgra_.data() + static_cast<std::size_t>(y) * width * 4u;
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::uint8_t* p = src_row + static_cast<std::size_t>(x) * bpp;
                std::uint8_t* d = dst_row + static_cast<std::size_t>(x) * 4u;
                d[0] = p[0]; // B
                d[1] = p[1]; // G
                d[2] = p[2]; // R
                d[3] = 0xFF; // A
            }
        }

        // The copy is counted: the statistics must show that the frame
        // was staged to host memory.
        core_ptr_->add_bytes_copied(static_cast<std::uint64_t>(bgra_.size()));

        return core_ptr_->submit_frame(bgra_.data(), bgra_.size(),
                                       static_cast<std::int32_t>(width * 4u), width, height, ts,
                                       nullptr);
    }

    plugins::CaptureConfig cfg_;
    UnixCaptureOptions opts_;
    plugins::CaptureFormat negotiated_format_{};

    Display* display_ = nullptr;
    int screen_ = 0;
    Window window_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;

    std::vector<std::uint8_t> bgra_;

    UnixCaptureCore* core_ptr_ = nullptr;
};

// ---------------------------------------------------------------------------
// X11Capture
// ---------------------------------------------------------------------------

X11Capture::X11Capture(plugins::CaptureConfig cfg, UnixCaptureOptions opts) noexcept
    : UnixCaptureDevice(cfg, opts) {
    impl_ = std::make_unique<Impl>(std::move(cfg), opts);
    impl_->set_core(&core());
}

X11Capture::~X11Capture() = default;

std::vector<plugins::CaptureDevice> X11Capture::platform_enumerate_devices() const noexcept {
    return impl_ ? impl_->enumerate_devices() : std::vector<plugins::CaptureDevice>{};
}

std::vector<plugins::CaptureFormat>
X11Capture::platform_supported_formats(const std::string& device_id) const noexcept {
    return impl_ ? impl_->supported_formats(device_id) : std::vector<plugins::CaptureFormat>{};
}

plugins::Status X11Capture::platform_open_device(UnixCaptureError& error) noexcept {
    if (!impl_) {
        error = UnixCaptureError::kCaptureFailed;
        return plugins::kErrInternal;
    }
    const plugins::Status status = impl_->open_device(error);
    if (status == plugins::kOk) {
        set_negotiated_format(impl_->negotiated_format());
    }
    return status;
}

void X11Capture::platform_close_device() noexcept {
    if (impl_)
        impl_->close_device();
}

plugins::CaptureCapabilities X11Capture::platform_capabilities() const noexcept {
    return impl_ ? impl_->capabilities() : plugins::CaptureCapabilities{};
}

void X11Capture::platform_start() noexcept {
    if (impl_)
        impl_->start();
}

void X11Capture::platform_stop() noexcept {
    if (impl_)
        impl_->stop();
}

plugins::Status X11Capture::platform_poll_frame() noexcept {
    if (!impl_)
        return plugins::kErrNotReady;
    return impl_->poll_frame();
}

} // namespace nimrtc::video_capture

#endif // defined(__linux__)
