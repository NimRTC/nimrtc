// modules/video_capture/src/platform/x11_capture.cpp
// ----------------------------------------------------------------------------
// The X11 capture device (remote-desktop plan A5).
//
// This is the *fallback* capture path on Linux: it works without a
// desktop portal, without D-Bus and without PipeWire, at the cost of a
// round trip to the X server for every frame.
//
// It is a thin adapter over the portable capture layer: it fills the
// shared bounded queue with BGRA frames and does nothing else.  All the
// shared machinery (the queue, the drop policy, the format
// negotiation, the cursor layer, the capture -> frame-stream adapter)
// lives in the portable layer and is shared with the Windows and macOS
// backends.
//
// See include/nimrtc/video_capture/x11_capture.hpp for the contract.
// ----------------------------------------------------------------------------

#include <nimrtc/video_capture/x11_capture.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

#if defined(__linux__) && defined(NIMRTC_HAVE_X11)
#  define NIMRTC_X11_CAPTURE 1
#  include <X11/Xlib.h>
#  include <X11/Xutil.h>
// `Xlib.h` defines `Status` as a macro (`#define Status int`), which
// would swallow the `plugins::Status` return types below.  The macro is
// only a convenience for Xlib's own prototypes, so it can be dropped
// right after the include: from here on `plugins::Status` is the real
// type again.
#  undef Status
#endif

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/video_capture/capture_queue.hpp>

namespace nimrtc::video_capture {

#if defined(NIMRTC_X11_CAPTURE)

namespace {

/** How long the capture thread sleeps between grabs when the requested
 *  frame rate is 0 (i.e. "as fast as possible" would be a busy loop). */
constexpr int kDefaultFrameIntervalMs = 33; // ~30 fps

/** The X11 display number used when the caller did not name a device. */
constexpr int kDefaultDisplayNumber = 0;

/**
 * @brief The X server's pixel layout.
 *
 * X11 hands the pixels over in the server's *native* format, which the
 * `XImage` describes: a `bits_per_pixel` of 32 with a `red_mask` of
 * 0x00FF0000 is the common "32-bit BGRX" layout — i.e. the bytes in
 * memory are B, G, R, X, which is *exactly* BGRA once the fourth byte
 * is understood as the alpha slot (X11 leaves it undefined, so it is
 * forced to 0xFF).
 *
 * A different mask combination means a different byte order, and the
 * swizzle has to be written out.  The three cases below cover every
 * layout a modern X server produces:
 *
 *   - 0x00FF0000 / 0x0000FF00 / 0x000000FF → BGRX in memory → BGRA.
 *   - 0x0000FF00 / 0x00FF0000 / 0x000000FF → XRGB in memory → BGRA.
 *   - 0x000000FF / 0x0000FF00 / 0x00FF0000 → RGBX in memory → BGRA.
 *
 * Anything else is refused rather than mis-converted: a frame in the
 * wrong byte order is worse than no frame.
 */
enum class X11PixelLayout : std::uint8_t {
    kBgrx,   ///< in-memory byte order is B, G, R, X
    kXrgb,   ///< in-memory byte order is X, R, G, B
    kRgbx,   ///< in-memory byte order is R, G, B, X
    kUnsupported,
};

X11PixelLayout classify_layout(const XImage* image) {
    if (image == nullptr) return X11PixelLayout::kUnsupported;
    if (image->bits_per_pixel != 32) return X11PixelLayout::kUnsupported;

    const unsigned long rm = image->red_mask;
    const unsigned long gm = image->green_mask;
    const unsigned long bm = image->blue_mask;

    if (rm == 0x00FF0000ul && gm == 0x0000FF00ul && bm == 0x000000FFul) {
        return X11PixelLayout::kBgrx;
    }
    if (rm == 0x0000FF00ul && gm == 0x00FF0000ul && bm == 0x000000FFul) {
        return X11PixelLayout::kXrgb;
    }
    if (rm == 0x000000FFul && gm == 0x0000FF00ul && bm == 0x00FF0000ul) {
        return X11PixelLayout::kRgbx;
    }
    return X11PixelLayout::kUnsupported;
}

/** Convert one 32-bit X11 pixel into BGRA. */
inline void pack_bgra(std::uint8_t* dst, std::uint32_t src,
                      X11PixelLayout layout) {
    switch (layout) {
        case X11PixelLayout::kBgrx:
            dst[0] = static_cast<std::uint8_t>(src & 0xFFu);          // B
            dst[1] = static_cast<std::uint8_t>((src >> 8) & 0xFFu);   // G
            dst[2] = static_cast<std::uint8_t>((src >> 16) & 0xFFu);  // R
            dst[3] = 0xFFu;                                           // A
            break;
        case X11PixelLayout::kXrgb:
            dst[0] = static_cast<std::uint8_t>((src >> 16) & 0xFFu);  // B
            dst[1] = static_cast<std::uint8_t>((src >> 8) & 0xFFu);   // G
            dst[2] = static_cast<std::uint8_t>(src & 0xFFu);          // R
            dst[3] = 0xFFu;
            break;
        case X11PixelLayout::kRgbx:
            dst[0] = static_cast<std::uint8_t>((src >> 16) & 0xFFu);  // B
            dst[1] = static_cast<std::uint8_t>((src >> 8) & 0xFFu);   // G
            dst[2] = static_cast<std::uint8_t>(src & 0xFFu);          // R
            dst[3] = 0xFFu;
            break;
        default:
            dst[0] = dst[1] = dst[2] = 0u;
            dst[3] = 0xFFu;
            break;
    }
}

/**
 * @brief Enumerate the X server's screens.
 *
 * Every screen becomes one capturable device.  The device id is
 * `"x11:<screen>"` — opaque to the engine, but stable within the
 * backend and parsed back by `open()`.
 */
void enumerate_screens(Display* display,
                       std::vector<plugins::CaptureDevice>& out) {
    if (display == nullptr) return;
    const int count = ScreenCount(display);
    for (int i = 0; i < count; ++i) {
        const Screen* screen = ScreenOfDisplay(display, i);
        if (screen == nullptr) continue;

        plugins::CaptureDevice d;
        d.id   = "x11:" + std::to_string(i);
        d.name = "X11 Screen " + std::to_string(i);
        d.kind = plugins::CaptureDeviceKind::kScreen;
        d.max_width  = static_cast<std::uint32_t>(screen->width);
        d.max_height = static_cast<std::uint32_t>(screen->height);
        // The X server does not publish a maximum frame rate: the
        // capture thread asks for frames as fast as it can, bounded by
        // the configured rate.
        d.max_fps = 60;
        out.push_back(std::move(d));
    }
}

/** Parse "x11:<screen>" into the screen index. */
bool parse_device_id(const std::string& id, int& screen) {
    if (id.rfind("x11:", 0) != 0) return false;
    const std::string tail = id.substr(4);
    if (tail.empty()) return false;
    try {
        std::size_t consumed = 0;
        const int value = std::stoi(tail, &consumed);
        if (consumed != tail.size()) return false;
        screen = value;
    } catch (...) {
        return false;
    }
    return screen >= 0;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// The platform state (PIMPL)
// ---------------------------------------------------------------------------

/**
 * @brief The X11 objects the device owns, plus the capture thread.
 *
 * Kept out of the public header so that the header stays free of the X11
 * SDK (and so that the class exists on every platform).
 */
class X11CaptureState {
public:
    /** The screen being captured. */
    int screen_ = 0;

    /** The captured surface's size, in pixels. */
    std::uint32_t source_width_  = 0;
    std::uint32_t source_height_ = 0;

    /** The bounded frame queue the capture thread fills. */
    CaptureQueue queue_{2};

    /** The capture thread. */
    std::thread thread_;

    /** Set to false to ask the capture thread to stop. */
    std::atomic<bool> running_{false};

    ~X11CaptureState() {
        stop_thread();
    }

    void stop_thread() noexcept {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }
};

// ---------------------------------------------------------------------------
// X11Capture
// ---------------------------------------------------------------------------

X11Capture::X11Capture(plugins::CaptureConfig cfg) noexcept
    : cfg_(std::move(cfg)) {}

X11Capture::~X11Capture() {
    close();
}

bool X11Capture::available() noexcept {
    return true;
}

plugins::Status X11Capture::open() noexcept {
    if (open_) return plugins::kOk;

    // ---- Resolve the screen ------------------------------------------------
    int screen_index = kDefaultDisplayNumber;
    if (!cfg_.device_id.empty()) {
        if (!parse_device_id(cfg_.device_id, screen_index)) {
            core::log::Logger::instance().error(
                "x11: '" + cfg_.device_id + "' is not an X11 device id");
            return plugins::kErrInvalidParam;
        }
    }

    // ---- Open the display --------------------------------------------------
    //
    // The connection is opened here (on the caller's thread) so that a
    // missing DISPLAY is reported as a clean error *before* the capture
    // thread starts, and so that the screen enumeration can happen on
    // the same connection the capture thread will use.
    //
    // The connection is *not* thread-safe, so the capture thread creates
    // its own connection to the same display.  Two connections to one
    // X server is completely normal — that is what every X client does.
    Display* probe = XOpenDisplay(nullptr);
    if (probe == nullptr) {
        core::log::Logger::instance().error(
            "x11: the X display could not be opened (is DISPLAY set?)");
        return plugins::kErrHardwareError;
    }

    if (screen_index >= ScreenCount(probe)) {
        XCloseDisplay(probe);
        return plugins::kErrInvalidParam;
    }

    std::vector<plugins::CaptureDevice> devices;
    enumerate_screens(probe, devices);
    const std::uint32_t src_w =
        devices.empty() ? 0u
                        : devices[static_cast<std::size_t>(screen_index)].max_width;
    const std::uint32_t src_h =
        devices.empty() ? 0u
                        : devices[static_cast<std::size_t>(screen_index)].max_height;
    const std::string dev_name =
        devices.empty() ? std::string{}
                        : devices[static_cast<std::size_t>(screen_index)].name;
    XCloseDisplay(probe);

    if (src_w == 0 || src_h == 0) {
        return plugins::kErrHardwareError;
    }

    // ---- Negotiate the format ----------------------------------------------
    cfg_.device_id = "x11:" + std::to_string(screen_index);
    cfg_.format.width  = src_w;
    cfg_.format.height = src_h;
    if (cfg_.format.fps == 0) cfg_.format.fps = 60;
    cfg_.format.tag = "bgra";
    // The pixels come back from the X server over a socket, so this is
    // the software path by definition.
    cfg_.format.zero_copy = false;
    // X11 has no per-frame damage information a client can read, so the
    // backend reports the conservative "the whole frame changed".
    cfg_.format.has_dirty_rects = false;

    description_ = dev_name;

    // ---- The scratch buffer -------------------------------------------------
    const std::size_t bytes =
        static_cast<std::size_t>(src_w) * static_cast<std::size_t>(src_h) * 4u;
    scratch_.assign(bytes, 0u);

    // ---- The state ----------------------------------------------------------
    auto state = std::make_unique<X11CaptureState>();
    state->screen_ = screen_index;
    state->source_width_  = src_w;
    state->source_height_ = src_h;

    // ---- Configure the queue -------------------------------------------------
    state->queue_.set_drop_policy(cfg_.drop_policy);
    if (cfg_.queue_depth > 0) state->queue_.set_depth(cfg_.queue_depth);

    // ---- Start the capture thread --------------------------------------------
    frame_index_ = 0;
    state_ = std::move(state);
    state_->running_.store(true, std::memory_order_release);
    state_->thread_ = std::thread([this]() { capture_loop(); });

    open_ = true;
    return plugins::kOk;
}

void X11Capture::close() noexcept {
    if (!open_) return;
    open_ = false;
    if (state_) {
        // Ask the capture thread to stop and wait for it: the X
        // connection is closed *by that thread*, so it is not in use
        // when the connection is torn down.
        state_->stop_thread();
    }
    state_.reset();
    scratch_.clear();
    scratch_.shrink_to_fit();
    region_scratch_.clear();
    region_scratch_.shrink_to_fit();
}

std::vector<plugins::CaptureDevice>
X11Capture::enumerate_devices() const noexcept {
    std::vector<plugins::CaptureDevice> devices;

    // The enumeration needs a connection.  It is opened and closed here
    // (on the caller's thread) rather than on the capture thread,
    // because `enumerate_devices()` is a *const* query the engine may
    // call before the capture thread even exists.
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) return devices;
    enumerate_screens(display, devices);
    XCloseDisplay(display);
    return devices;
}

std::vector<plugins::CaptureFormat>
X11Capture::supported_formats(const std::string& device_id) const noexcept {
    std::vector<plugins::CaptureFormat> out;
    for (const auto& d : enumerate_devices()) {
        if (!device_id.empty() && d.id != device_id) continue;
        plugins::CaptureFormat f;
        f.width  = d.max_width;
        f.height = d.max_height;
        f.fps    = d.max_fps;
        f.tag    = "bgra";
        f.zero_copy = false;
        f.has_dirty_rects = false;
        out.push_back(std::move(f));
    }
    return out;
}

plugins::CaptureCapabilities X11Capture::capabilities() const noexcept {
    plugins::CaptureCapabilities c;
    c.backend = "x11-capture";
    // The pixels are read back from the X server: not zero-copy.
    c.zero_copy = false;
    // X11 offers no damage information to the client.
    c.dirty_rects = false;
    // XGetImage does NOT include the pointer — the cursor layer's job.
    c.cursor_composited = false;
    // The region is cropped out of the grabbed image.
    c.region_selection = true;
    // The screens are enumerated from the live connection.
    c.enumeration = true;
    c.max_pixels = static_cast<std::uint64_t>(state_ ? state_->source_width_ : 0u) *
                   static_cast<std::uint64_t>(state_ ? state_->source_height_ : 0u);
    return c;
}

plugins::Status X11Capture::pop_frame(plugins::CaptureFrame& out) noexcept {
    if (!open_ || !state_) return plugins::kErrNotReady;
    // The capture thread filled the queue; the engine's tick thread
    // only takes what is already there.  It never waits.
    if (!state_->queue_.pop(out)) {
        return plugins::kErrNotReady;
    }
    return plugins::kOk;
}

plugins::CaptureStats X11Capture::stats() const noexcept {
    plugins::CaptureStats s;
    s.frames_captured = frames_captured_.load(std::memory_order_relaxed);
    s.frames_dropped  = frames_dropped_.load(std::memory_order_relaxed);
    s.device_errors   = device_errors_.load(std::memory_order_relaxed);
    s.bytes_copied    = bytes_copied_.load(std::memory_order_relaxed);
    return s;
}

void X11Capture::capture_loop() noexcept {
    if (!state_) return;

    // ---- The X connection ----------------------------------------------------
    //
    // It is created *here*, on the capture thread, and used only here:
    // the X connection is not thread-safe, and the engine's tick thread
    // only ever touches the shared bounded queue.
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        core::log::Logger::instance().error(
            "x11: the capture thread could not open the X display");
        device_errors_.fetch_add(1, std::memory_order_relaxed);
        state_->running_.store(false, std::memory_order_release);
        return;
    }

    const int screen_index = state_->screen_;
    const std::uint32_t src_w = state_->source_width_;
    const std::uint32_t src_h = state_->source_height_;

    // The frame interval comes from the negotiated frame rate.
    const int interval_ms =
        cfg_.format.fps > 0 ? (1000 / static_cast<int>(cfg_.format.fps))
                            : kDefaultFrameIntervalMs;

    // The region crop, computed once: the source surface does not move.
    const bool has_region = cfg_.region.width != 0 && cfg_.region.height != 0;
    const std::uint32_t rx = std::min(cfg_.region.x, src_w);
    const std::uint32_t ry = std::min(cfg_.region.y, src_h);
    const std::uint32_t rw = std::min(cfg_.region.width, src_w - rx);
    const std::uint32_t rh = std::min(cfg_.region.height, src_h - ry);
    if (has_region && (rw == 0 || rh == 0)) {
        core::log::Logger::instance().error(
            "x11: the requested region lies outside the screen");
        device_errors_.fetch_add(1, std::memory_order_relaxed);
        XCloseDisplay(display);
        state_->running_.store(false, std::memory_order_release);
        return;
    }

    const std::size_t full_bytes =
        static_cast<std::size_t>(src_w) * static_cast<std::size_t>(src_h) * 4u;
    std::vector<std::uint8_t> full(full_bytes, 0u);

    while (state_->running_.load(std::memory_order_acquire)) {
        // ---- Grab -------------------------------------------------------------
        //
        // `XGetImage` on the root window: the whole desktop, at the
        // server's native pixel format.  This is the round trip that
        // makes this the slow path.
        XImage* image = XGetImage(display, RootWindow(display, screen_index),
                                   0, 0, src_w, src_h, AllPlanes, ZPixmap);
        if (image == nullptr) {
            // The server may have gone away (the user logged out, the
            // X server was restarted).  That is a device error, not a
            // crash.
            device_errors_.fetch_add(1, std::memory_order_relaxed);
            core::log::Logger::instance().error(
                "x11: the screen could not be grabbed");
            break;
        }

        const X11PixelLayout layout = classify_layout(image);
        if (layout == X11PixelLayout::kUnsupported) {
            core::log::Logger::instance().error(
                "x11: the server's pixel format is not a supported "
                "32-bit layout");
            device_errors_.fetch_add(1, std::memory_order_relaxed);
            XDestroyImage(image);
            break;
        }

        // ---- Convert ------------------------------------------------------------
        const auto* src = reinterpret_cast<const std::uint8_t*>(image->data);
        const std::size_t src_stride =
            static_cast<std::size_t>(image->bytes_per_line);
        for (std::uint32_t row = 0; row < src_h; ++row) {
            const std::uint8_t* src_row =
                src + static_cast<std::size_t>(row) * src_stride;
            std::uint8_t* dst_row =
                full.data() + static_cast<std::size_t>(row) *
                                  static_cast<std::size_t>(src_w) * 4u;
            for (std::uint32_t col = 0; col < src_w; ++col) {
                std::uint32_t px;
                std::memcpy(&px, src_row + static_cast<std::size_t>(col) * 4u,
                            sizeof(px));
                pack_bgra(dst_row + static_cast<std::size_t>(col) * 4u, px,
                          layout);
            }
        }

        XDestroyImage(image);

        // ---- Crop ---------------------------------------------------------------
        const std::uint8_t* pixels = full.data();
        std::uint32_t frame_w = src_w;
        std::uint32_t frame_h = src_h;
        std::size_t frame_bytes = full_bytes;
        std::int32_t stride = static_cast<std::int32_t>(src_w) * 4;

        if (has_region) {
            const std::size_t region_row = static_cast<std::size_t>(rw) * 4u;
            const std::size_t region_bytes = region_row * rh;
            if (region_scratch_.size() != region_bytes) {
                region_scratch_.assign(region_bytes, 0u);
            }
            for (std::uint32_t row = 0; row < rh; ++row) {
                std::memcpy(region_scratch_.data() +
                                static_cast<std::size_t>(row) * region_row,
                            full.data() +
                                (static_cast<std::size_t>(ry + row) * src_w + rx) *
                                    4u,
                            region_row);
            }
            pixels = region_scratch_.data();
            frame_w = rw;
            frame_h = rh;
            frame_bytes = region_bytes;
            stride = static_cast<std::int32_t>(region_row);
        }

        bytes_copied_.fetch_add(static_cast<std::uint64_t>(frame_bytes),
                                std::memory_order_relaxed);

        // ---- Queue ---------------------------------------------------------------
        //
        // X11 gives the client no damage information, so the honest
        // answer is "the whole frame changed".
        plugins::CaptureDirtyRects dirty;
        dirty.full_frame = true;

        const std::int64_t ts = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        const std::uint32_t seq = frame_index_++;
        const auto outcome = state_->queue_.push(
            pixels, frame_bytes, stride, frame_w, frame_h, ts, seq, dirty);
        switch (outcome) {
            case CaptureQueue::PushResult::kQueued:
                frames_captured_.fetch_add(1, std::memory_order_relaxed);
                break;
            case CaptureQueue::PushResult::kDroppedOldest:
                frames_captured_.fetch_add(1, std::memory_order_relaxed);
                frames_dropped_.fetch_add(1, std::memory_order_relaxed);
                break;
            case CaptureQueue::PushResult::kRejected:
                frames_dropped_.fetch_add(1, std::memory_order_relaxed);
                break;
        }

        if (cb_) {
            plugins::CaptureFrame view;
            view.pixels = pixels;
            view.size = frame_bytes;
            view.stride = stride;
            view.width = frame_w;
            view.height = frame_h;
            view.capture_ts_us = ts;
            view.frame_seq = seq;
            view.dirty = dirty;
            view.has_gpu_handle = false;
            cb_(view);
        }

        // ---- Pace -----------------------------------------------------------------
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }

    // The connection is closed *here*, on the thread that owns it.
    XCloseDisplay(display);
}

#else // !NIMRTC_X11_CAPTURE

// ---------------------------------------------------------------------------
// The non-Linux stub
// ---------------------------------------------------------------------------

class X11CaptureState {};

X11Capture::X11Capture(plugins::CaptureConfig cfg) noexcept
    : cfg_(std::move(cfg)) {}

X11Capture::~X11Capture() = default;

bool X11Capture::available() noexcept {
    return false;
}

plugins::Status X11Capture::open() noexcept {
    // X11 is a Linux/X11 API.  On any other host this device simply does
    // not exist, and saying so is clearer than pretending.
    return plugins::kErrUnsupported;
}

void X11Capture::close() noexcept {}

std::vector<plugins::CaptureDevice>
X11Capture::enumerate_devices() const noexcept {
    return {};
}

std::vector<plugins::CaptureFormat>
X11Capture::supported_formats(const std::string&) const noexcept {
    return {};
}

plugins::CaptureCapabilities X11Capture::capabilities() const noexcept {
    plugins::CaptureCapabilities c;
    c.backend = "x11-capture";
    // The stub describes the backend *as it would be* on a Linux host,
    // so a host that queries the capabilities before deciding where to
    // run gets the same answer on every platform.  What it cannot
    // promise on this host is the pixels — `open()` refuses.
    c.zero_copy = false;          // the pixels come back over a socket
    c.dirty_rects = false;        // X11 offers no damage information
    c.cursor_composited = false;  // XGetImage does not draw the pointer
    c.region_selection = true;    // the region is cropped out of the grab
    c.enumeration = true;         // the screens come from the live server
    return c;
}

void X11Capture::capture_loop() noexcept {}

plugins::Status X11Capture::pop_frame(plugins::CaptureFrame&) noexcept {
    return plugins::kErrUnsupported;
}

plugins::CaptureStats X11Capture::stats() const noexcept {
    return plugins::CaptureStats{};
}

#endif // NIMRTC_X11_CAPTURE

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void register_x11_capture() noexcept {
#if defined(NIMRTC_X11_CAPTURE)
    static const X11CaptureFactory kFactory{};
    core::PluginRegistry::instance().register_video_capture(kFactory.id(),
                                                            &kFactory);
#endif
}

} // namespace nimrtc::video_capture
