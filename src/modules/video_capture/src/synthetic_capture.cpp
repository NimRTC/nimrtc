// modules/video_capture/src/synthetic_capture.cpp
// ----------------------------------------------------------------------------
// The synthetic capture device - the portable capture layer's reference
// implementation.
//
// It exists so the *portable* half of the capture stack (the queue, the
// dirty-rect model, the negotiation, the drop policy) can be exercised on
// every CI leg on every platform, with no display, no camera and no
// permission prompt.  The three platform backends planned for M1-c are
// thin adapters that produce exactly what this produces.
//
// See include/nimrtc/video_capture/synthetic_capture.hpp for the contract.
// ----------------------------------------------------------------------------

#include <nimrtc/video_capture/synthetic_capture.hpp>

#include <chrono>
#include <cstring>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/video_capture/dxgi_desktop_dup.hpp>
#include <nimrtc/video_capture/portal_capture.hpp>
#include <nimrtc/video_capture/wgc_capture.hpp>
#include <nimrtc/video_capture/x11_capture.hpp>

namespace nimrtc::video_capture {

namespace {

// The synthetic device offers exactly one device.
constexpr const char* kDeviceId   = "synthetic:0";
constexpr const char* kDeviceName = "Synthetic display";

// The pixel layout the synthetic device delivers: 8-bit BGRA, the
// format every desktop compositor produces natively.
constexpr const char* kPixelTag = "bgra";

// The highlight box that moves across the frame.  Its size is a fraction
// of the frame so the dirty rect is meaningful at any resolution.
constexpr std::uint32_t kBoxDivisor = 4;

/** The moving highlight box for a given frame index, in pixels. */
struct Box {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
};

Box box_for(std::uint32_t frame_index, std::uint32_t width,
            std::uint32_t height) noexcept {
    Box b;
    b.w = width / kBoxDivisor;
    b.h = height / kBoxDivisor;
    if (b.w == 0) b.w = 1;
    if (b.h == 0) b.h = 1;
    // Walk the box diagonally, wrapping around the frame.  The modulo
    // keeps the motion deterministic, so a test can compute the expected
    // dirty rect itself.
    const std::uint32_t span_x = (width > b.w) ? (width - b.w) : 1;
    const std::uint32_t span_y = (height > b.h) ? (height - b.h) : 1;
    b.x = (frame_index * 7u) % span_x;
    b.y = (frame_index * 5u) % span_y;
    return b;
}

/** The union of two rectangles (the dirty region between them). */
plugins::CaptureRegion union_of(const plugins::CaptureRegion& a,
                                const plugins::CaptureRegion& b) noexcept {
    plugins::CaptureRegion r;
    const std::uint32_t x0 = a.x < b.x ? a.x : b.x;
    const std::uint32_t y0 = a.y < b.y ? a.y : b.y;
    const std::uint32_t x1 = (a.x + a.width) > (b.x + b.width)
        ? (a.x + a.width) : (b.x + b.width);
    const std::uint32_t y1 = (a.y + a.height) > (b.y + b.height)
        ? (a.y + a.height) : (b.y + b.height);
    r.x = x0;
    r.y = y0;
    r.width  = x1 - x0;
    r.height = y1 - y0;
    return r;
}

std::int64_t now_us() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// SyntheticCapture
// ---------------------------------------------------------------------------

SyntheticCapture::SyntheticCapture(plugins::CaptureConfig cfg) noexcept
    : cfg_(std::move(cfg)) {
    queue_.set_drop_policy(cfg_.drop_policy);
    if (cfg_.queue_depth > 0) queue_.set_depth(cfg_.queue_depth);
}

SyntheticCapture::~SyntheticCapture() = default;

plugins::Status SyntheticCapture::open() noexcept {
    if (open_) return plugins::kOk;

    // A zero-sized request means "the device's best", which for the
    // synthetic device is 1280x720.
    if (cfg_.format.width == 0)  cfg_.format.width  = 1280;
    if (cfg_.format.height == 0) cfg_.format.height = 720;
    if (cfg_.format.fps == 0)    cfg_.format.fps    = 30;

    if (cfg_.format.width == 0 || cfg_.format.height == 0) {
        return plugins::kErrInvalidParam;
    }

    // The device id is opaque; an unknown one is an error, not a silent
    // fallback, because the caller asked for a device that does not exist.
    if (!cfg_.device_id.empty() && cfg_.device_id != kDeviceId) {
        return plugins::kErrInvalidParam;
    }
    cfg_.device_id = kDeviceId;

    // A region narrows the captured surface.  The device reports the
    // region it was asked for, so the consumer knows the frame's origin
    // as well as its size.  A zero-sized region means "the whole
    // surface", so the negotiated format is left alone.
    if (cfg_.region.width != 0 && cfg_.region.height != 0) {
        cfg_.format.width  = cfg_.region.width;
        cfg_.format.height = cfg_.region.height;
    }

    // The negotiated format names the pixel layout the device will
    // deliver.  The synthetic device produces BGRA -- the format every
    // desktop compositor hands over natively -- and the tag is what
    // tells the consumer (the capture -> frame-stream adapter) how to
    // read the bytes.  A caller that asked for a specific layout is
    // told the truth instead of being silently given a different one.
    if (cfg_.format.tag.empty()) {
        cfg_.format.tag = kPixelTag;
    } else if (cfg_.format.tag != kPixelTag) {
        // The synthetic device cannot deliver another layout.
        return plugins::kErrInvalidParam;
    }
    cfg_.format.zero_copy     = false;
    cfg_.format.has_dirty_rects = true;

    const std::size_t bytes = static_cast<std::size_t>(cfg_.format.width) *
                              static_cast<std::size_t>(cfg_.format.height) * 4u;
    scratch_.assign(bytes, 0u);
    frame_index_ = 0;
    open_ = true;
    return plugins::kOk;
}

void SyntheticCapture::close() noexcept {
    if (!open_) return;
    open_ = false;
    queue_.clear();
    scratch_.clear();
    scratch_.shrink_to_fit();
}

std::vector<plugins::CaptureDevice>
SyntheticCapture::enumerate_devices() const noexcept {
    plugins::CaptureDevice d;
    d.id   = kDeviceId;
    d.name = kDeviceName;
    d.kind = plugins::CaptureDeviceKind::kScreen;
    d.max_width  = 1920;
    d.max_height = 1080;
    d.max_fps    = 60;
    return {d};
}

std::vector<plugins::CaptureFormat>
SyntheticCapture::supported_formats(const std::string& device_id) const noexcept {
    (void)device_id;
    std::vector<plugins::CaptureFormat> out;
    // The synthetic device offers BGRA (what every desktop compositor
    // produces natively) at a few common sizes.
    const std::uint32_t sizes[][2] = {
        {640, 480}, {1280, 720}, {1920, 1080},
    };
    for (const auto& s : sizes) {
        plugins::CaptureFormat f;
        f.width  = s[0];
        f.height = s[1];
        f.fps    = 30;
        f.tag    = kPixelTag;
        f.zero_copy = false;   // a synthetic device has no GPU surface
        f.has_dirty_rects = true;
        out.push_back(std::move(f));
    }
    return out;
}

plugins::CaptureCapabilities SyntheticCapture::capabilities() const noexcept {
    plugins::CaptureCapabilities c;
    c.zero_copy = false;
    c.dirty_rects = true;
    c.cursor_composited = false;
    c.region_selection = true;
    c.enumeration = true;
    c.max_pixels = 1920ull * 1080ull;
    c.backend = "synthetic";
    return c;
}

plugins::Status SyntheticCapture::pop_frame(plugins::CaptureFrame& out) noexcept {
    if (!open_) return plugins::kErrNotReady;
    if (!queue_.pop(out)) return plugins::kErrNotReady;
    return plugins::kOk;
}

void SyntheticCapture::render_frame(std::uint8_t* dst, std::size_t size,
                                    std::uint32_t frame_index) const noexcept {
    const std::uint32_t w = cfg_.format.width;
    const std::uint32_t h = cfg_.format.height;
    if (dst == nullptr || size == 0 || w == 0 || h == 0) return;

    // A vertical gradient whose brightness advances with the frame index,
    // so successive frames differ and the encoder has something to
    // compress.  The moving highlight box gives the dirty-rect test
    // something exact to assert on.
    const Box box = box_for(frame_index, w, h);
    const std::uint8_t luma_base = static_cast<std::uint8_t>(
        16u + ((frame_index * 3u) % 200u));

    for (std::uint32_t y = 0; y < h; ++y) {
        std::uint8_t* row = dst + static_cast<std::size_t>(y) * w * 4u;
        const std::uint8_t luma = static_cast<std::uint8_t>(
            luma_base + ((y * 64u) / (h == 0 ? 1u : h)));
        for (std::uint32_t x = 0; x < w; ++x) {
            const bool in_box = (x >= box.x && x < box.x + box.w &&
                                 y >= box.y && y < box.y + box.h);
            // BGR order: the synthetic device produces BGRA, the format
            // every desktop compositor hands over natively.
            row[x * 4u + 0u] = in_box ? 0xFFu : luma;   // B
            row[x * 4u + 1u] = in_box ? 0x80u : luma;   // G
            row[x * 4u + 2u] = in_box ? 0x20u : luma;   // R
            row[x * 4u + 3u] = 0xFFu;                  // A
        }
    }
}

plugins::Status SyntheticCapture::produce_one() noexcept {
    if (!open_) return plugins::kErrNotReady;

    if (budget_ != 0 && frame_index_ >= budget_) {
        // The device is exhausted, which models the "device lost" path.
        ++stats_.device_errors;
        return plugins::kErrHardwareError;
    }

    const std::uint32_t w = cfg_.format.width;
    const std::uint32_t h = cfg_.format.height;
    if (scratch_.size() < static_cast<std::size_t>(w) * h * 4u) {
        return plugins::kErrBufferTooSmall;
    }

    // The dirty region is the union of the box's old and new positions.
    plugins::CaptureDirtyRects dirty;
    if (static_) {
        // Nothing moved: the picture is unchanged, so there is no dirty
        // region at all.  This is the property that makes a static
        // desktop nearly free to send.
        dirty.full_frame = false;
        dirty.rects.clear();
    } else {
        const Box prev = box_for(frame_index_ == 0 ? 0u : frame_index_ - 1u, w, h);
        const Box cur  = box_for(frame_index_, w, h);
        plugins::CaptureRegion a{prev.x, prev.y, prev.w, prev.h};
        plugins::CaptureRegion b{cur.x, cur.y, cur.w, cur.h};
        dirty.full_frame = false;
        dirty.rects.push_back(union_of(a, b));
    }

    render_frame(scratch_.data(), scratch_.size(), frame_index_);

    const std::int64_t ts = now_us();
    const auto outcome = queue_.push(scratch_.data(), scratch_.size(),
                                     static_cast<std::int32_t>(w * 4u),
                                     w, h, ts, frame_index_, dirty);
    switch (outcome) {
        case CaptureQueue::PushResult::kQueued:
            ++stats_.frames_captured;
            break;
        case CaptureQueue::PushResult::kDroppedOldest:
            // The frame itself is queued, but the oldest one was evicted
            // to make room -- that eviction is the drop.
            ++stats_.frames_captured;
            ++stats_.frames_dropped;
            break;
        case CaptureQueue::PushResult::kRejected:
            // The arrival was refused; the queue still holds what it had.
            ++stats_.frames_dropped;
            break;
    }

    ++frame_index_;

    if (cb_) {
        // The callback fires with a view of the frame that was just
        // queued.  It is the *same* bytes the consumer will later pop,
        // not a second copy, so the callback and the consumer agree on
        // the picture.  The bytes are valid until the next push/pop.
        plugins::CaptureFrame view;
        if (!queue_.peek(view)) {
            // The frame was dropped by the policy, so there is nothing to
            // show.  The drop was already counted.
            return plugins::kOk;
        }
        cb_(view);
    }

    return plugins::kOk;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

namespace {

const SyntheticCaptureFactory kSyntheticFactory{};

} // anonymous namespace

void register_default_plugins() noexcept {
    static bool registered = false;
    if (registered) return;
    registered = true;
    core::PluginRegistry::instance().register_video_capture(
        kSyntheticFactory.id(), &kSyntheticFactory);

    // Remote-desktop plan A3: the Windows display-capture backend
    // registers itself under "dxgi_desktop".  On a non-Windows host
    // the call is a no-op — there is no DXGI to talk to, and a host
    // that asks for "dxgi_desktop" there gets the engine's "capture
    // plugin missing" error rather than a silent no-op device.
    register_dxgi_desktop_dup();

    // Remote-desktop plan A3: the Windows Graphics Capture backend
    // registers itself under "wgc".  It is the *window* capture path
    // and the one that composites the pointer into the frame.
    register_wgc_capture();

    // Remote-desktop plan A5: the two Linux backends.  Both register
    // themselves only where their platform libraries exist:
    //   - "x11"        — the XGetImage fallback (X11 + Xfixes),
    //   - "xdg_portal" — the Wayland path (GIO + PipeWire).
    register_x11_capture();
    register_portal_capture();
}

} // namespace nimrtc::video_capture
