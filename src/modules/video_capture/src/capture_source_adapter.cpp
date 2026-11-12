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
                u_plane[static_cast<std::ptrdiff_t>(cy) * u_stride + cx] =
                    static_cast<std::uint8_t>(std::clamp(u, 0, 255));
                v_plane[static_cast<std::ptrdiff_t>(cy) * v_stride + cx] =
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
                    static_cast<std::size_t>(j / 2u) * uv_stride + (i / 2u) * 2u;
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
    return plugins::kOk;
}

void CaptureSourceAdapter::stop() noexcept {
    if (!running_) return;
    running_ = false;
    if (capture_ != nullptr) capture_->close();
    scratch_.clear();
    scratch_.shrink_to_fit();
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

    if (!convert(in)) {
        ++dropped_;
        return;
    }

    ++produced_;
    if (composite_cursor(out_)) {
        ++cursor_frames_;
    }
    if (cb_) cb_(out_);
}

bool CaptureSourceAdapter::composite_cursor(
        plugins::VideoSourceFrame& out) noexcept {
    if (cursor_source_ == nullptr || cursor_compositor_ == nullptr) return false;
    if (!cursor_compositor_->should_composite()) return false;

    // Alpha blending needs the pixels' alpha channel, so only a packed
    // 32-bit layout can carry the pointer.  A planar YUV frame has no
    // alpha, and compositing into one would mean a full colour-space
    // conversion — that is the plan's C2 (SIMD format conversion) work,
    // not the cursor's.
    if (out.format != plugins::VideoPixelFormat::kBGRA &&
        out.format != plugins::VideoPixelFormat::kRGBA) {
        return false;
    }
    if (out.plane_y == nullptr) return false;

    // A frame that points into the *device's* buffer must not be
    // written into: the device owns those pixels and may recycle them
    // the moment the callback returns.  In that case the frame is
    // copied into the adapter's own scratch buffer first.
    if (out.plane_y == in_pixels_base_) {
        const std::size_t needed =
            static_cast<std::size_t>(out.height) *
            static_cast<std::size_t>(out.stride_y);
        if (needed == 0 || needed > out.buffer_size) return false;
        if (scratch_.size() < needed) scratch_.assign(needed, 0u);
        std::memcpy(scratch_.data(), out.plane_y, needed);
        out.plane_y = scratch_.data();
        out.buffer_size = needed;
    }

    const CursorState state = cursor_source_->query();
    if (!state.visible) return false;

    // The device may have captured a *region* of a larger surface, so
    // the pointer's source-space position has to be shifted by the
    // region's origin.  The adapter does not know the origin (the
    // device owns it), so the origin is zero unless the device
    // negotiated one — which it reports through its config.
    std::int32_t origin_x = 0;
    std::int32_t origin_y = 0;
    if (capture_ != nullptr) {
        const plugins::CaptureConfig cfg = capture_->config();
        origin_x = static_cast<std::int32_t>(cfg.region.x);
        origin_y = static_cast<std::int32_t>(cfg.region.y);
    }

    return cursor_compositor_->composite(
        const_cast<std::uint8_t*>(out.plane_y),
        out.buffer_size,
        out.stride_y,
        out.width,
        out.height,
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

        if (is_planar_i420(want)) {
            // The capture reports one stride; the chroma planes of a
            // planar 4:2:0 buffer use half of it.
            const std::int32_t chroma_stride = src_stride / 2;
            const std::size_t y_bytes = static_cast<std::size_t>(h) * src_stride;
            const std::size_t u_bytes =
                static_cast<std::size_t>(h / 2u) * chroma_stride;
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
                bgra_to_i420(w, h, in.pixels, src_stride,
                             y_plane, layout.strides[0],
                             u_plane, layout.strides[1],
                             v_plane, layout.strides[2]);
            } else if (want == plugins::VideoPixelFormat::kNV12) {
                bgra_to_nv12(w, h, in.pixels, src_stride,
                             y_plane, layout.strides[0],
                             u_plane, layout.strides[1]);
            } else if (want == plugins::VideoPixelFormat::kRGBA) {
                bgra_to_rgba(w, h, in.pixels, src_stride,
                             y_plane, layout.strides[0]);
            } else {
                return false;
            }
            break;

        case CapturePixelFormat::kNV12:
            if (want == plugins::VideoPixelFormat::kI420) {
                // The UV plane of an NV12 buffer follows the Y plane.
                const std::uint8_t* uv_plane = in.pixels +
                    static_cast<std::size_t>(h) * src_stride;
                nv12_to_i420(w, h, in.pixels, src_stride, uv_plane, src_stride,
                             y_plane, layout.strides[0],
                             u_plane, layout.strides[1],
                             v_plane, layout.strides[2]);
            } else {
                return false;
            }
            break;

        case CapturePixelFormat::kI420:
            if (want == plugins::VideoPixelFormat::kNV12) {
                // The capture reports one stride; the chroma planes of
                // a planar 4:2:0 buffer use half of it.
                const std::int32_t chroma_stride = src_stride / 2;
                const std::size_t y_bytes = static_cast<std::size_t>(h) * src_stride;
                const std::size_t u_bytes =
                    static_cast<std::size_t>(h / 2u) * chroma_stride;
                video_frame::convert_i420_to_nv12(
                    w, h,
                    in.pixels, src_stride,
                    in.pixels + y_bytes, chroma_stride,
                    in.pixels + y_bytes + u_bytes, chroma_stride,
                    y_plane, layout.strides[0],
                    u_plane, layout.strides[1]);
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
