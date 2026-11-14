// modules/video_capture/src/cursor_compositor.cpp
// ----------------------------------------------------------------------------
// The portable cursor-compositing core (remote-desktop plan A6).
//
// Everything here is platform-neutral: no OS header is included, no OS
// call is made.  The interesting half of the cursor layer — the blend,
// the clipping, the recoverability and the coordinate translation — is
// therefore unit-tested on every CI leg, including the Windows leg where
// the macOS and Linux cursor sources cannot even be compiled.
//
// The *sources* (Windows `GetCursorInfo` + `DrawIconEx`, X11
// `XFixesGetCursorImage`) are thin platform adapters under
// `src/platform/`.
// ----------------------------------------------------------------------------

#include <nimrtc/video_capture/cursor_compositor.hpp>
#include <nimrtc/video_capture/cursor_source.hpp>

#include <algorithm>
#include <cstring>

namespace nimrtc::video_capture {

namespace {

/** The number of bytes per pixel (BGRA). */
constexpr std::size_t kBytesPerPixel = 4u;

/**
 * @brief The source-over blend of one byte.
 *
 * The arithmetic is exact at the extremes: `a == 0` returns `dst`
 * bit-identical, and `a == 255` returns `src` bit-identical.  The
 * rounding is "add 127 then divide", which keeps the midpoint exact
 * and never overflows (the maximum intermediate is 255*255 + 255*255 +
 * 127 = 130177, which fits in a 32-bit unsigned).
 */
inline std::uint8_t blend_byte(std::uint8_t src, std::uint8_t dst, std::uint8_t a) noexcept {
    if (a == 0) return dst;
    if (a == 255) return src;
    const std::uint32_t out =
        (static_cast<std::uint32_t>(src) * a + static_cast<std::uint32_t>(dst) * (255u - a) +
         127u) / 255u;
    return static_cast<std::uint8_t>(out);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// CursorCompositor
// ---------------------------------------------------------------------------

CursorCompositor::CompositeResult CursorCompositor::composite(const CursorImage& image,
                                                               std::int32_t x, std::int32_t y,
                                                               std::uint8_t* frame,
                                                               std::size_t size,
                                                               std::int32_t stride,
                                                               std::uint32_t width,
                                                               std::uint32_t height) noexcept {
    CompositeResult result;

    if (frame == nullptr || width == 0 || height == 0) return result;
    if (!image.valid()) return result;

    // ---- The frame's geometry --------------------------------------------
    const std::size_t packed = static_cast<std::size_t>(width) * kBytesPerPixel;
    const std::size_t frame_stride =
        (stride > 0 && static_cast<std::size_t>(stride) >= packed)
            ? static_cast<std::size_t>(stride)
            : packed;
    if (size < frame_stride * height) return result;

    // ---- The cursor's placement ------------------------------------------
    //
    // The hotspot is the point *inside* the bitmap that corresponds to
    // the pointer's position, so the bitmap is placed so that the
    // hotspot lands on (x, y).
    const std::int64_t origin_x = static_cast<std::int64_t>(x) - image.hotspot_x;
    const std::int64_t origin_y = static_cast<std::int64_t>(y) - image.hotspot_y;

    // ---- The clipping ----------------------------------------------------
    //
    // Only the part of the bitmap that lands inside the frame is
    // blended.  An out-of-bounds write would be a memory-safety bug,
    // so the intersection is computed explicitly.
    const std::int64_t clip_x0 = std::max<std::int64_t>(0, origin_x);
    const std::int64_t clip_y0 = std::max<std::int64_t>(0, origin_y);
    const std::int64_t clip_x1 = std::min<std::int64_t>(
        static_cast<std::int64_t>(width), origin_x + static_cast<std::int64_t>(image.width));
    const std::int64_t clip_y1 = std::min<std::int64_t>(
        static_cast<std::int64_t>(height), origin_y + static_cast<std::int64_t>(image.height));

    if (clip_x1 <= clip_x0 || clip_y1 <= clip_y0) {
        // The cursor is entirely off the frame: nothing is blended and
        // nothing is overwritten.  (This is not an error — it is what
        // happens when the pointer leaves the captured display.)
        return result;
    }

    const std::uint32_t touched_x = static_cast<std::uint32_t>(clip_x0);
    const std::uint32_t touched_y = static_cast<std::uint32_t>(clip_y0);
    const std::uint32_t touched_w = static_cast<std::uint32_t>(clip_x1 - clip_x0);
    const std::uint32_t touched_h = static_cast<std::uint32_t>(clip_y1 - clip_y0);

    // ---- The backup ------------------------------------------------------
    //
    // The original pixels of the region are kept, so `restore()` can
    // put them back.  This is what makes the compositor *recoverable*:
    // compositing is not a one-way door.
    backup_.clear();
    backup_.resize(static_cast<std::size_t>(touched_w) * kBytesPerPixel * touched_h);
    for (std::uint32_t row = 0; row < touched_h; ++row) {
        const std::uint8_t* src = frame + (static_cast<std::size_t>(touched_y + row) *
                                           frame_stride) +
                                  static_cast<std::size_t>(touched_x) * kBytesPerPixel;
        std::uint8_t* dst =
            backup_.data() + static_cast<std::size_t>(row) * touched_w * kBytesPerPixel;
        std::memcpy(dst, src, static_cast<std::size_t>(touched_w) * kBytesPerPixel);
    }
    backup_frame_ = frame;
    backup_stride_ = static_cast<std::int32_t>(frame_stride);
    backup_x_ = touched_x;
    backup_y_ = touched_y;
    backup_width_ = touched_w;
    backup_height_ = touched_h;
    has_backup_ = true;

    // ---- The blend -------------------------------------------------------
    const std::int32_t image_stride = image.row_stride();
    for (std::uint32_t row = 0; row < touched_h; ++row) {
        const std::int64_t image_y = clip_y0 + row - origin_y;
        const std::uint8_t* image_row =
            image.pixels.data() +
            static_cast<std::size_t>(image_y) * static_cast<std::size_t>(image_stride);
        std::uint8_t* frame_row = frame + (static_cast<std::size_t>(touched_y + row) *
                                           frame_stride) +
                                  static_cast<std::size_t>(touched_x) * kBytesPerPixel;

        for (std::uint32_t col = 0; col < touched_w; ++col) {
            const std::int64_t image_x = clip_x0 + col - origin_x;
            const std::uint8_t* src =
                image_row + static_cast<std::size_t>(image_x) * kBytesPerPixel;
            const std::uint8_t alpha = src[3];
            if (alpha == 0) {
                // Fully transparent: the destination is left bit-identical.
                continue;
            }
            std::uint8_t* dst = frame_row + static_cast<std::size_t>(col) * kBytesPerPixel;
            dst[0] = blend_byte(src[0], dst[0], alpha);
            dst[1] = blend_byte(src[1], dst[1], alpha);
            dst[2] = blend_byte(src[2], dst[2], alpha);
            // The alpha byte is left alone: the captured frames are
            // opaque, and leaving it untouched is what makes "a fully
            // transparent cursor changes nothing" true.
        }
    }

    result.applied = true;
    result.x = touched_x;
    result.y = touched_y;
    result.width = touched_w;
    result.height = touched_h;
    return result;
}

void CursorCompositor::restore() noexcept {
    if (!has_backup_) return;
    if (backup_frame_ == nullptr || backup_.empty()) {
        has_backup_ = false;
        return;
    }

    const std::size_t row_bytes = static_cast<std::size_t>(backup_width_) * kBytesPerPixel;
    for (std::uint32_t row = 0; row < backup_height_; ++row) {
        std::uint8_t* dst = backup_frame_ +
                            (static_cast<std::size_t>(backup_y_ + row) *
                             static_cast<std::size_t>(backup_stride_)) +
                            static_cast<std::size_t>(backup_x_) * kBytesPerPixel;
        std::memcpy(dst, backup_.data() + static_cast<std::size_t>(row) * row_bytes, row_bytes);
    }

    backup_.clear();
    backup_frame_ = nullptr;
    has_backup_ = false;
}

CursorPosition CursorCompositor::screen_to_frame(std::int32_t screen_x, std::int32_t screen_y,
                                                  std::int32_t origin_x, std::int32_t origin_y,
                                                  std::uint32_t width,
                                                  std::uint32_t height) noexcept {
    CursorPosition out;
    out.x = screen_x - origin_x;
    out.y = screen_y - origin_y;
    out.inside = (out.x >= 0 && out.y >= 0 &&
                  static_cast<std::uint32_t>(out.x) < width &&
                  static_cast<std::uint32_t>(out.y) < height);
    return out;
}

// ---------------------------------------------------------------------------
// CursorCompositingCapture
// ---------------------------------------------------------------------------

CursorCompositingCapture::CursorCompositingCapture(
    std::unique_ptr<plugins::IVideoCapture> inner, std::shared_ptr<ICursorSource> source)
    : inner_(std::move(inner)), source_(std::move(source)) {}

CursorCompositingCapture::~CursorCompositingCapture() = default;

const char* CursorCompositingCapture::name() const noexcept {
    return inner_ ? inner_->name() : "cursor-compositing-capture";
}

plugins::Status CursorCompositingCapture::open() noexcept {
    if (!inner_) return plugins::kErrInvalidParam;
    return inner_->open();
}

void CursorCompositingCapture::close() noexcept {
    if (inner_) inner_->close();
}

std::vector<plugins::CaptureDevice>
CursorCompositingCapture::enumerate_devices() const noexcept {
    if (!inner_) return {};
    return inner_->enumerate_devices();
}

std::vector<plugins::CaptureFormat>
CursorCompositingCapture::supported_formats(const std::string& device_id) const noexcept {
    if (!inner_) return {};
    return inner_->supported_formats(device_id);
}

plugins::CaptureCapabilities CursorCompositingCapture::capabilities() const noexcept {
    if (!inner_) return plugins::CaptureCapabilities{};
    plugins::CaptureCapabilities caps = inner_->capabilities();
    // The flag stays **honest**: it says what the *device* does.  The
    // cursor layer is a separate thing, and the engine learns about it
    // from the decorator, not from a lie in the device's capabilities.
    return caps;
}

bool CursorCompositingCapture::is_open() const noexcept {
    return inner_ && inner_->is_open();
}

plugins::CaptureConfig CursorCompositingCapture::config() const noexcept {
    if (!inner_) return plugins::CaptureConfig{};
    return inner_->config();
}

plugins::Status CursorCompositingCapture::pop_frame(plugins::CaptureFrame& out) noexcept {
    if (!inner_) return plugins::kErrNotReady;

    plugins::Status status = inner_->pop_frame(out);
    if (status != plugins::kOk) return status;

    // Only a BGRA frame is a canvas the cursor can be blended into.  A
    // camera (NV12) or an I420 capture passes through unchanged —
    // blending a BGRA bitmap into it would corrupt it.
    if (out.pixels == nullptr || out.format != plugins::VideoPixelFormat::kBGRA) {
        return plugins::kOk;
    }
    if (out.width == 0 || out.height == 0) return plugins::kOk;

    // The cursor's position and bitmap.
    //
    // The source is wrapped in a `PinnedCursorFallback` the first
    // time it is polled, so a platform cursor that becomes unavailable
    // (the pointer left the captured display, the compositor is busy)
    // does not make the pointer *disappear*: the last known shape is
    // drawn at the last known position, which is the plan's
    // "the cursor is always drawn at the last position" fallback.
    CursorSample sample;
    CursorSourceStatus source_status = CursorSourceStatus::kUnsupported;
    if (source_) {
        if (!pinned_) {
            pinned_ = std::make_shared<PinnedCursorFallback>(source_);
        }
        source_status = pinned_->poll(sample);
    }    if (source_status != CursorSourceStatus::kOk) {
        // Either the platform composites the cursor itself (so there is
        // nothing to do), or there is no usable cursor at all.  Either
        // way the frame is delivered unchanged -- a frame without a
        // pointer is better than a corrupted one.
        return plugins::kOk;
    }
    if (sample.hidden) return plugins::kOk;
    if (!sample.image.valid()) return plugins::kOk;

    // The frame's pixels are *borrowed* from the inner device, so the
    // blend is applied to a copy and the copy is what the consumer
    // sees.
    scratch_.assign(out.pixels, out.pixels + out.size);

    // The source reports the position in *screen* coordinates; the
    // frame's own coordinate space starts at its origin.  The origin
    // is the frame's top-left corner in screen coordinates, which the
    // capture device knows and the cursor source does not.
    const CursorPosition pos =
        CursorCompositor::screen_to_frame(sample.screen_x, sample.screen_y, 0, 0, out.width,
                                          out.height);

    const auto result = compositor_.composite(sample.image, pos.x, pos.y, scratch_.data(),
                                              scratch_.size(), out.stride, out.width, out.height);

    if (result.applied) {
        // The cursor's box is part of what changed, so the dirty-rect
        // bookkeeping must know about it.
        plugins::CaptureRegion region;
        region.x = result.x;
        region.y = result.y;
        region.width = result.width;
        region.height = result.height;
        if (out.dirty.full_frame) {
            out.dirty.rects.clear();
            out.dirty.full_frame = false;
        }
        out.dirty.rects.push_back(region);
    }

    out.pixels = scratch_.data();
    out.size = scratch_.size();
    return plugins::kOk;
}

void CursorCompositingCapture::set_frame_callback(plugins::CaptureFrameCallback cb) noexcept {
    if (!inner_) return;
    inner_->set_frame_callback(std::move(cb));
}

plugins::CaptureStats CursorCompositingCapture::stats() const noexcept {
    if (!inner_) return plugins::CaptureStats{};
    return inner_->stats();
}

} // namespace nimrtc::video_capture
