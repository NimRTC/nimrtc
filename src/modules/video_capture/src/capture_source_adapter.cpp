// modules/video_capture/src/capture_source_adapter.cpp
// ----------------------------------------------------------------------------
// The shared "capture device -> frame stream" adapter.
//
// It is the portable half of the remote-desktop capture path: the part
// that does not change between Windows, macOS and Linux.  A platform
// backend produces frames; this adapter turns them into the
// `VideoSourceFrame` stream the engine's video path already consumes.
//
// See include/nimrtc/video_capture/capture_source_adapter.hpp for the
// contract, and the remote-desktop plan's A2 / A2b rows for the
// acceptance criteria this implements.
// ----------------------------------------------------------------------------

#include <nimrtc/core/log.hpp>
#include <nimrtc/video_capture/capture_source_adapter.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

#include <nimrtc/video_frame/frame.hpp>

namespace nimrtc::video_capture {

namespace {

// ---------------------------------------------------------------------------
// Format helpers
// ---------------------------------------------------------------------------

/** The plugin-level pixel format the adapter converts *into*. */
plugins::VideoPixelFormat to_plugin_format(CapturePixelFormat f) noexcept {
    switch (f) {
        case CapturePixelFormat::kBGRA: return plugins::VideoPixelFormat::kBGRA;
        case CapturePixelFormat::kRGBA: return plugins::VideoPixelFormat::kRGBA;
        case CapturePixelFormat::kNV12: return plugins::VideoPixelFormat::kNV12;
        case CapturePixelFormat::kI420: return plugins::VideoPixelFormat::kI420;
        default:                        return plugins::VideoPixelFormat::kUnknown;
    }
}

// ---------------------------------------------------------------------------
// BGRA -> I420 (BT.601 limited range)
// ---------------------------------------------------------------------------

/**
 * Convert a packed BGRA buffer into planar I420.
 *
 * This is the portable CPU conversion the plan's §B.6 budget table names
 * ("Format conversion (BGRA/NV12 -> I420) ... libyuv ARGBToI420 is
 * SIMD").  The SIMD version is plan item C2; until it lands, this
 * scalar version keeps the *mechanism* correct and portable, which is
 * what A2b asks for -- the platform backends must be thin adapters over
 * one mechanism, not three parallel implementations of the maths.
 */
void bgra_to_i420(std::uint32_t width,
                  std::uint32_t height,
                  const std::uint8_t* src,
                  std::int32_t src_stride,
                  std::uint8_t* y_plane,  std::int32_t y_stride,
                  std::uint8_t* u_plane,  std::int32_t u_stride,
                  std::uint8_t* v_plane,  std::int32_t v_stride) noexcept {
    if (width == 0 || height == 0 || src == nullptr) return;

    // BT.601 limited-range coefficients, in 8.8 fixed point.
    constexpr std::int32_t kYScale  = 66;    //  16..235
    constexpr std::int32_t kUScale  = 43;    //  16..240
    constexpr std::int32_t kVScale  = 43;
    constexpr std::int32_t kYOffset = 16;
    constexpr std::int32_t kUVOffset = 128;

    for (std::uint32_t j = 0; j < height; ++j) {
        const std::uint8_t* src_row =
            src + static_cast<std::ptrdiff_t>(j) * src_stride;
        std::uint8_t* y_row =
            y_plane + static_cast<std::ptrdiff_t>(j) * y_stride;

        for (std::uint32_t i = 0; i < width; ++i) {
            // The source is BGRA: blue first.
            const std::int32_t b = src_row[i * 4 + 0];
            const std::int32_t g = src_row[i * 4 + 1];
            const std::int32_t r = src_row[i * 4 + 2];

            const std::int32_t y =
                ((kYScale * r + kYScale * g + kYScale * b) >> 8) + kYOffset;
            y_row[i] = static_cast<std::uint8_t>(std::clamp(y, 0, 255));

            // The chroma planes are subsampled 2x2; one sample per 2x2
            // block is enough for a screen's flat regions and is what
            // every desktop compositor's own scaler does.
            if ((j % 2u) == 0u && (i % 2u) == 0u) {
                const std::int32_t u =
                    ((-kUScale * r - kVScale * g + 9184 * b) >> 16) + kUVOffset;
                const std::int32_t v =
                    ((9184 * r - kUScale * g - kVScale * b) >> 16) + kUVOffset;

                const std::size_t cx = i / 2u;
                const std::size_t cy = j / 2u;
                u_plane[static_cast<std::size_t>(cy) *
                             static_cast<std::size_t>(u_stride) + cx] =
                    static_cast<std::uint8_t>(std::clamp(u, 0, 255));
                v_plane[static_cast<std::size_t>(cy) *
                             static_cast<std::size_t>(v_stride) + cx] =
                    static_cast<std::uint8_t>(std::clamp(v, 0, 255));
            }
        }
    }
}

/** Convert a packed BGRA buffer into semi-planar NV12. */
void bgra_to_nv12(std::uint32_t width,
                  std::uint32_t height,
                  const std::uint8_t* src,
                  std::int32_t src_stride,
                  std::uint8_t* y_plane,  std::int32_t y_stride,
                  std::uint8_t* uv_plane, std::int32_t uv_stride) noexcept {
    if (width == 0 || height == 0 || src == nullptr) return;

    constexpr std::int32_t kYScale  = 66;
    constexpr std::int32_t kUScale  = 43;
    constexpr std::int32_t kVScale  = 43;
    constexpr std::int32_t kYOffset = 16;
    constexpr std::int32_t kUVOffset = 128;

    for (std::uint32_t j = 0; j < height; ++j) {
        const std::uint8_t* src_row =
            src + static_cast<std::ptrdiff_t>(j) * src_stride;
        std::uint8_t* y_row =
            y_plane + static_cast<std::ptrdiff_t>(j) * y_stride;

        for (std::uint32_t i = 0; i < width; ++i) {
            const std::int32_t b = src_row[i * 4 + 0];
            const std::int32_t g = src_row[i * 4 + 1];
            const std::int32_t r = src_row[i * 4 + 2];

            const std::int32_t y =
                ((kYScale * r + kYScale * g + kYScale * b) >> 8) + kYOffset;
            y_row[i] = static_cast<std::uint8_t>(std::clamp(y, 0, 255));

            if ((j % 2u) == 0u && (i % 2u) == 0u) {
                const std::int32_t u =
                    ((-kUScale * r - kVScale * g + 9184 * b) >> 16) + kUVOffset;
                const std::int32_t v =
                    ((9184 * r - kUScale * g - kVScale * b) >> 16) + kUVOffset;
                const std::size_t idx =
                    static_cast<std::size_t>(j / 2u) *
                        static_cast<std::size_t>(uv_stride) +
                    static_cast<std::size_t>(i / 2u) * 2u;
                uv_plane[idx + 0] = static_cast<std::uint8_t>(std::clamp(u, 0, 255));
                uv_plane[idx + 1] = static_cast<std::uint8_t>(std::clamp(v, 0, 255));
            }
        }
    }
}

/** Convert a packed BGRA buffer into packed RGBA. */
void bgra_to_rgba(std::uint32_t width,
                  std::uint32_t height,
                  const std::uint8_t* src,
                  std::int32_t src_stride,
                  std::uint8_t* dst,
                  std::int32_t dst_stride) noexcept {
    if (width == 0 || height == 0 || src == nullptr || dst == nullptr) return;
    for (std::uint32_t j = 0; j < height; ++j) {
        const std::uint8_t* s = src + static_cast<std::ptrdiff_t>(j) * src_stride;
        std::uint8_t* d = dst + static_cast<std::ptrdiff_t>(j) * dst_stride;
        for (std::uint32_t i = 0; i < width; ++i) {
            d[i * 4 + 0] = s[i * 4 + 2];   // R
            d[i * 4 + 1] = s[i * 4 + 1];   // G
            d[i * 4 + 2] = s[i * 4 + 0];   // B
            d[i * 4 + 3] = s[i * 4 + 3];   // A
        }
    }
}

/** De-interleave an NV12 buffer into planar I420. */
void nv12_to_i420(std::uint32_t width,
                  std::uint32_t height,
                  const std::uint8_t* y_plane, std::int32_t y_stride,
                  const std::uint8_t* uv_plane, std::int32_t uv_stride,
                  std::uint8_t* out_y, std::int32_t out_y_stride,
                  std::uint8_t* out_u, std::int32_t out_u_stride,
                  std::uint8_t* out_v, std::int32_t out_v_stride) noexcept {
    if (width == 0 || height == 0) return;
    for (std::uint32_t j = 0; j < height; ++j) {
        std::memcpy(out_y + static_cast<std::ptrdiff_t>(j) * out_y_stride,
                    y_plane + static_cast<std::ptrdiff_t>(j) * y_stride,
                    width);
    }
    const std::uint32_t cw = width / 2u;
    const std::uint32_t ch = height / 2u;
    for (std::uint32_t j = 0; j < ch; ++j) {
        const std::uint8_t* uv = uv_plane + static_cast<std::ptrdiff_t>(j) * uv_stride;
        std::uint8_t* u = out_u + static_cast<std::ptrdiff_t>(j) * out_u_stride;
        std::uint8_t* v = out_v + static_cast<std::ptrdiff_t>(j) * out_v_stride;
        for (std::uint32_t i = 0; i < cw; ++i) {
            u[i] = uv[i * 2 + 0];
            v[i] = uv[i * 2 + 1];
        }
    }
}

/** True iff the format is planar I420 (three planes). */
bool is_planar_i420(plugins::VideoPixelFormat f) noexcept {
    return f == plugins::VideoPixelFormat::kI420 ||
           f == plugins::VideoPixelFormat::kYV12;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Format mapping
// ---------------------------------------------------------------------------

CapturePixelFormat capture_pixel_format_from_tag(std::string_view tag) noexcept {
    // The tags are lower-case by convention; the comparison is
    // case-insensitive because a platform backend may spell them
    // differently ("BGRA" vs "bgra").
    if (tag.size() != 4) return CapturePixelFormat::kUnknown;
    const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(tag[0])));
    const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(tag[1])));
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(tag[2])));
    const char d = static_cast<char>(std::tolower(static_cast<unsigned char>(tag[3])));

    if (a == 'b' && b == 'g' && c == 'r' && d == 'a') return CapturePixelFormat::kBGRA;
    if (a == 'r' && b == 'g' && c == 'b' && d == 'a') return CapturePixelFormat::kRGBA;
    if (a == 'n' && b == 'v' && c == '1' && d == '2') return CapturePixelFormat::kNV12;
    if (a == 'i' && b == '4' && c == '2' && d == '0') return CapturePixelFormat::kI420;
    return CapturePixelFormat::kUnknown;
}

std::string_view capture_pixel_format_name(CapturePixelFormat f) noexcept {
    switch (f) {
        case CapturePixelFormat::kBGRA: return "BGRA";
        case CapturePixelFormat::kRGBA: return "RGBA";
        case CapturePixelFormat::kNV12: return "NV12";
        case CapturePixelFormat::kI420: return "I420";
        default:                        return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// CaptureSourceAdapter
// ---------------------------------------------------------------------------

CaptureSourceAdapter::CaptureSourceAdapter(plugins::IVideoCapture* capture,
                                           plugins::VideoSourceConfig cfg) noexcept
    : capture_(capture), cfg_(std::move(cfg)) {
    if (cfg_.format == plugins::VideoPixelFormat::kUnknown) {
        // I420 is what every encoder in the tree accepts.
        cfg_.format = plugins::VideoPixelFormat::kI420;
    }
    // A capture device's content is its own, not a test pattern.
    cfg_.pattern = plugins::VideoSourcePattern::kCustom;
}

CaptureSourceAdapter::~CaptureSourceAdapter() {
    stop();
}

plugins::Status CaptureSourceAdapter::start() noexcept {
    if (capture_ == nullptr) return plugins::kErrInvalidParam;
    if (running_) return plugins::kOk;

    const plugins::Status st = capture_->open();
    if (st != plugins::kOk) return st;

    // The device reports the format it actually negotiated.  The
    // adapter must read it back rather than assume the request was
    // granted -- a device may have fallen back to a smaller size.
    const plugins::CaptureConfig dev_cfg = capture_->config();
    device_format_ = capture_pixel_format_from_tag(dev_cfg.format.tag);
    if (device_format_ == CapturePixelFormat::kUnknown) {
        capture_->close();
        return plugins::kErrInvalidParam;
    }

    // The device is the authority on its own geometry: it reports the
    // size it actually negotiated, which may be smaller than what was
    // asked for.  The adapter delivers the device's frames as they
    // are -- resizing them is the scaler's job (remote-desktop plan
    // C1 / C2), and doing it here would hide the mismatch instead of
    // reporting it.
    cfg_.width  = dev_cfg.format.width;
    cfg_.height = dev_cfg.format.height;
    if (cfg_.fps == 0) cfg_.fps = dev_cfg.format.fps;

    // The backend identifier is cached: `capabilities()` returns it by
    // value, and a `string_view` into a temporary would dangle.
    backend_ = capture_->capabilities().backend;

    scratch_.clear();
    running_ = true;

    install_cursor_layer();
    return plugins::kOk;
}

void CaptureSourceAdapter::stop() noexcept {
    if (!running_) return;
    running_ = false;
    if (capture_ != nullptr) capture_->close();
    scratch_.clear();
    scratch_.shrink_to_fit();

    // The automatic cursor layer belongs to the *open* session: a
    // closed device has no surface, so a pointer position would be
    // meaningless.  A host-installed source and compositor survive the
    // close, because the host owns them.
    owned_cursor_source_.reset();
    owned_cursor_compositor_.reset();
    cursor_layer_installed_ = false;
    if (!host_installed_cursor_) {
        cursor_source_ = nullptr;
        cursor_compositor_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// The automatic cursor layer (remote-desktop plan A6)
// ---------------------------------------------------------------------------

void CaptureSourceAdapter::install_cursor_layer() noexcept {
    if (capture_ == nullptr) return;

    const plugins::CaptureConfig cfg = capture_->config();

    // A host that already installed its own cursor keeps it: the host
    // knows something about the pointer that the adapter cannot know
    // (a remote agent that tracks the *remote* pointer, a test that
    // wants a deterministic picture).
    if (!host_installed_cursor_) {
        // The device may have composited the pointer into the frame
        // itself (Windows Graphics Capture, macOS
        // ScreenCaptureKit).  Drawing it again would double-draw it.
        const plugins::CaptureCapabilities caps = capture_->capabilities();
        if (!caps.cursor_composited && cfg.want_cursor) {
            // The zero-copy path cannot draw the pointer: the picture
            // lives in GPU memory and there are no CPU pixels to blend
            // into.  Installing a cursor layer here would make the
            // adapter *drop* every GPU frame (see `produce_one`), so
            // the layer is not installed at all and the picture is
            // delivered without a pointer.
            if (cfg.format.zero_copy) {
                NIMRTC_LOG_WARN("capture: the zero-copy path is active, so "
                                "the pointer is not composited into the "
                                "captured picture");
            } else {
                // The platform's own pointer reader.  A platform without
                // one (macOS composites it itself, a build without the
                // pointer libraries) yields nullptr, and there is then
                // nothing to draw.
                owned_cursor_source_ = make_platform_cursor_source();
                if (owned_cursor_source_) {
                    // The drawing is the portable alpha blend, with the
                    // pinned fallback: when the platform cannot report a
                    // live pointer, the last known one stays on screen
                    // rather than vanishing.
                    owned_cursor_compositor_ =
                        std::make_unique<AlphaCursorCompositor>(true);
                    cursor_source_ = owned_cursor_source_.get();
                    cursor_compositor_ = owned_cursor_compositor_.get();
                    cursor_layer_installed_ = true;
                }
            }
        }
    }

    // The pointer is reported in the platform's *global* space, while
    // the captured frame is one surface inside it.  Whichever source
    // is installed has to know where that surface sits — the
    // adapter's own and a host's alike.
    if (cursor_source_ != nullptr) {
        cursor_source_->set_surface_origin(cfg.surface_origin_x,
                                           cfg.surface_origin_y);
    }
}

void CaptureSourceAdapter::produce_one() noexcept {
    if (!running_ || capture_ == nullptr) return;

    plugins::CaptureFrame in{};
    if (capture_->pop_frame(in) != plugins::kOk) {
        // The capture queue is empty.  That is not an error: a quiet
        // device must not park the engine's tick loop.
        return;
    }

    last_dirty_ = in.dirty;
    in_pixels_base_ = in.pixels;

    // The device states the colour description of what it produces
    // (full-range BT.709 for a desktop compositor, limited-range BT.601
    // for a camera).  The conversion below uses it, so the encoder
    // receives a picture whose luma / chroma scaling it can trust.
    source_color_ = video_frame::resolve_color_description(in.color);

    // ---- Zero-copy passthrough (remote-desktop plan B10) --------------------
    //
    // A frame whose picture lives in GPU memory has no CPU pixels to
    // convert: the *surface* is the picture.  Such a frame is published
    // as-is (the handle travels with it) so the encoder can consume the
    // native surface directly.
    //
    // The one thing the adapter cannot do on this path is draw the
    // pointer: alpha blending needs the CPU pixels, and a GPU surface
    // has none.  A device that does not composite the pointer itself
    // (DXGI) therefore forces the *CPU* path for that frame -- but a
    // cursor layer is optional, and a host that wants the zero-copy
    // path simply leaves it uninstalled or installs a device that
    // composites the pointer.
    if (in.gpu_surface.backend != video_frame::GpuBackend::kNone &&
        in.gpu_surface.handle != nullptr) {
        const bool can_draw_cursor =
            cursor_source_ != nullptr && cursor_compositor_ != nullptr;
        if (!can_draw_cursor || !cursor_compositor_->should_composite()) {
            if (publish_gpu_surface(in)) {
                ++produced_;
                ++zero_copy_frames_;
                if (cb_) cb_(out_);
                return;
            }
            // A surface the adapter cannot describe (no dimensions, or
            // a format it does not know) falls through to the CPU
            // path, which will drop it if there are no pixels either.
            ++dropped_;
            return;
        }
        // The pointer has to be drawn and the picture is on the GPU:
        // there is no CPU copy to draw into, so the frame is dropped
        // rather than published with a missing pointer.
        NIMRTC_LOG_WARN("capture: a GPU frame was dropped because the "
                        "cursor layer must draw into CPU pixels");
        ++dropped_;
        return;
    }

    // The cursor is drawn into the *device's* pixels, before the
    // format conversion.
    //
    // The order matters: alpha blending needs a packed 32-bit layout,
    // and the device delivers one (BGRA), while the *output* is
    // usually planar I420 — which has no alpha channel to blend into.
    // Compositing after the conversion would therefore never draw
    // anything on the engine's own path.  Drawing first also means the
    // pointer is converted along with the rest of the picture, so it
    // lands in the encoder in the right colours.
    cursor_scratch_ready_ = false;
    const bool drew = composite_cursor(in);

    if (!convert(in)) {
        ++dropped_;
        return;
    }

    ++produced_;
    if (drew) {
        ++cursor_frames_;
    }
    if (cb_) cb_(out_);
}

bool CaptureSourceAdapter::publish_gpu_surface(
        const plugins::CaptureFrame& in) noexcept {
    if (in.width == 0 || in.height == 0) return false;

    out_ = {};
    out_.width  = in.width;
    out_.height = in.height;
    out_.format = to_plugin_format(device_format_);
    if (out_.format == plugins::VideoPixelFormat::kUnknown) {
        // The GPU path carries the picture in its native layout, so the
        // *device's* tag is only used for the metadata.  A device with
        // an unknown tag cannot describe its surface, and the encoder
        // would have nothing to work with.
        return false;
    }
    out_.stride_y = in.stride;
    out_.buffer_size = in.size;
    out_.capture_ts_us = in.capture_ts_us;
    out_.frame_seq = in.frame_seq;
    // The GPU surface carries the device's native pixels, so the device's
    // own colour description travels with it unchanged.
    out_.color = source_color_;

    // The GPU handle is *borrowed* from the device: it stays valid
    // until the device's next frame.  The consumer (the encoder) uses
    // it within the callback, which is exactly the lifetime the seam
    // documents.
    out_.gpu_surface = in.gpu_surface;
    return true;
}

bool CaptureSourceAdapter::composite_cursor(
        plugins::CaptureFrame& in) noexcept {
    if (cursor_source_ == nullptr || cursor_compositor_ == nullptr) return false;
    if (!cursor_compositor_->should_composite()) return false;

    // Alpha blending needs the pixels' alpha channel, so only a packed
    // 32-bit layout can carry the pointer.  The *device* delivers one;
    // the output format is irrelevant here, because the drawing happens
    // before the conversion.
    if (device_format_ != CapturePixelFormat::kBGRA &&
        device_format_ != CapturePixelFormat::kRGBA) {
        return false;
    }
    if (in.pixels == nullptr || in.size == 0) return false;

    const std::uint32_t w = in.width;
    const std::uint32_t h = in.height;
    if (w == 0 || h == 0) return false;

    const std::int32_t src_stride = in.stride > 0
        ? in.stride
        : static_cast<std::int32_t>(w * 4u);

    // The pointer is asked for *before* the copy: a pointer that is
    // hidden costs nothing, and a hidden pointer must not force a
    // full-frame copy on every tick.
    const CursorState state = cursor_source_->query();
    if (!state.visible) return false;

    // A frame that points into the *device's* buffer must not be
    // written into: the device owns those pixels and may recycle them
    // the moment the callback returns.  In that case the frame is
    // copied into the adapter's own scratch buffer first — and the
    // conversion then reads *that* copy, so the pointer becomes part
    // of the published picture.
    if (!cursor_scratch_ready_) {
        // The copy is exactly the device's own buffer size, so the
        // conversion sees the same geometry it would have seen
        // without the cursor.
        if (in.size == 0) return false;
        if (cursor_scratch_.size() != in.size) cursor_scratch_.assign(in.size, 0u);
        std::memcpy(cursor_scratch_.data(), in.pixels, in.size);
        cursor_scratch_ready_ = true;
        // The conversion must read the copy, not the device's buffer.
        in.pixels = cursor_scratch_.data();
    }
    const std::uint8_t* pixels = in.pixels;

    // The device may have captured a *region* of a larger surface, so
    // the pointer's surface-relative position has to be shifted by the
    // region's origin to land in the frame's own coordinates.
    std::int32_t origin_x = 0;
    std::int32_t origin_y = 0;
    if (capture_ != nullptr) {
        const plugins::CaptureConfig cfg = capture_->config();
        origin_x = static_cast<std::int32_t>(cfg.region.x);
        origin_y = static_cast<std::int32_t>(cfg.region.y);
    }

    return cursor_compositor_->composite(
        const_cast<std::uint8_t*>(pixels),
        in.size,
        src_stride,
        w,
        h,
        state,
        origin_x,
        origin_y);
}

bool CaptureSourceAdapter::convert(const plugins::CaptureFrame& in) noexcept {
    if (in.pixels == nullptr || in.size == 0) {
        // A metadata-only frame (a dirty-rect notification) has no
        // pixels to publish.
        return false;
    }

    const std::uint32_t w = in.width;
    const std::uint32_t h = in.height;
    if (w == 0 || h == 0) return false;

    const std::int32_t src_stride = in.stride > 0
        ? in.stride
        : static_cast<std::int32_t>(w * 4u);

    const plugins::VideoPixelFormat want = cfg_.format;
    const plugins::VideoPixelFormat have = to_plugin_format(device_format_);
    if (have == plugins::VideoPixelFormat::kUnknown) return false;

    // -- Fast path: the device already delivers the requested layout. ----
    if (have == want) {
        out_ = {};
        out_.width  = w;
        out_.height = h;
        out_.format = want;
        out_.stride_y = src_stride;
        out_.plane_y = in.pixels;
        out_.buffer_size = in.size;
        out_.capture_ts_us = in.capture_ts_us;
        out_.frame_seq = in.frame_seq;
        // No pixel values change, so the description travels unchanged.
        // (A packed RGB output carries no luma / chroma scaling, so it
        // does not travel at all.)
        out_.color = (want == plugins::VideoPixelFormat::kBGRA ||
                      want == plugins::VideoPixelFormat::kRGBA)
                   ? video_frame::ColorDescription{}
                   : source_color_;

        if (is_planar_i420(want)) {
            // The capture reports one stride; the chroma planes of a
            // planar 4:2:0 buffer use half of it.
            const std::int32_t chroma_stride = src_stride / 2;
            const std::size_t y_bytes =
                static_cast<std::size_t>(h) * static_cast<std::size_t>(src_stride);
            const std::size_t u_bytes =
                static_cast<std::size_t>(h / 2u) *
                static_cast<std::size_t>(chroma_stride);
            out_.plane_u = in.pixels + y_bytes;
            out_.plane_v = in.pixels + y_bytes + u_bytes;
            out_.stride_u = chroma_stride;
            out_.stride_v = chroma_stride;
        }
        return true;
    }

    // -- Slow path: convert into the scratch buffer. ----------------------
    const video_frame::VideoFrameLayout layout =
        video_frame::compute_layout(want, w, h);
    if (layout.buffer_size == 0) return false;

    if (scratch_.size() != layout.buffer_size) {
        scratch_.assign(layout.buffer_size, 0u);
    }

    std::uint8_t* y_plane = scratch_.data() + layout.plane_offsets[0];
    std::uint8_t* u_plane = scratch_.data() + layout.plane_offsets[1];
    std::uint8_t* v_plane = scratch_.data() + layout.plane_offsets[2];

    switch (device_format_) {
        case CapturePixelFormat::kBGRA:
            if (want == plugins::VideoPixelFormat::kI420) {
                video_frame::convert_bgra_to_i420_color(
                    w, h, in.pixels, src_stride,
                    source_color_,
                    y_plane, layout.strides[0],
                    u_plane, layout.strides[1],
                    v_plane, layout.strides[2],
                    &out_color_);
            } else if (want == plugins::VideoPixelFormat::kNV12) {
                video_frame::convert_bgra_to_nv12_color(
                    w, h, in.pixels, src_stride,
                    source_color_,
                    y_plane, layout.strides[0],
                    u_plane, layout.strides[1],
                    &out_color_);
            } else if (want == plugins::VideoPixelFormat::kRGBA) {
                bgra_to_rgba(w, h, in.pixels, src_stride,
                             y_plane, layout.strides[0]);
                // A packed RGB output carries no luma/chroma scaling, so
                // the description does not travel with it.
                out_color_ = video_frame::ColorDescription{};
            } else {
                return false;
            }
            break;

        case CapturePixelFormat::kNV12:
            if (want == plugins::VideoPixelFormat::kI420) {
                // The UV plane of an NV12 buffer follows the Y plane.
                const std::uint8_t* uv_plane = in.pixels +
                    static_cast<std::size_t>(h) *
                    static_cast<std::size_t>(src_stride);
                nv12_to_i420(w, h, in.pixels, src_stride, uv_plane, src_stride,
                             y_plane, layout.strides[0],
                             u_plane, layout.strides[1],
                             v_plane, layout.strides[2]);
                // A layout conversion does not change the pixel values,
                // so the description travels unchanged.
                out_color_ = source_color_;
            } else {
                return false;
            }
            break;

        case CapturePixelFormat::kI420:
            if (want == plugins::VideoPixelFormat::kNV12) {
                // The capture reports one stride; the chroma planes of
                // a planar 4:2:0 buffer use half of it.
                const std::int32_t chroma_stride = src_stride / 2;
                const std::size_t y_bytes =
                    static_cast<std::size_t>(h) * static_cast<std::size_t>(src_stride);
                const std::size_t u_bytes =
                    static_cast<std::size_t>(h / 2u) *
                    static_cast<std::size_t>(chroma_stride);
                video_frame::convert_i420_to_nv12(
                    w, h,
                    in.pixels, src_stride,
                    in.pixels + y_bytes, chroma_stride,
                    in.pixels + y_bytes + u_bytes, chroma_stride,
                    y_plane, layout.strides[0],
                    u_plane, layout.strides[1]);
                out_color_ = source_color_;
            } else {
                return false;
            }
            break;

        default:
            return false;
    }

    out_ = {};
    out_.width  = w;
    out_.height = h;
    out_.format = want;
    out_.stride_y = layout.strides[0];
    out_.plane_y = y_plane;
    out_.buffer_size = layout.buffer_size;
    out_.capture_ts_us = in.capture_ts_us;
    out_.frame_seq = in.frame_seq;
    out_.color = out_color_;

    if (is_planar_i420(want)) {
        out_.stride_u = layout.strides[1];
        out_.stride_v = layout.strides[2];
        out_.plane_offset_u = layout.plane_offsets[1];
        out_.plane_offset_v = layout.plane_offsets[2];
        out_.plane_u = u_plane;
        out_.plane_v = v_plane;
    }
    return true;
}

plugins::IVideoSource::Stats CaptureSourceAdapter::stats() const noexcept {
    Stats s;
    s.frames_produced = produced_;
    s.frames_dropped  = dropped_;
    s.ticks = produced_ + dropped_;
    return s;
}

bool CaptureSourceAdapter::is_hardware_accelerated() const noexcept {
    if (capture_ == nullptr) return false;
    return capture_->capabilities().zero_copy;
}

std::string_view CaptureSourceAdapter::hardware_backend() const noexcept {
    if (backend_.empty()) return "software";
    return backend_;
}

} // namespace nimrtc::video_capture
