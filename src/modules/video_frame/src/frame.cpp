/**
 * @file src/modules/video_frame/src/frame.cpp
 * @brief Video frame utilities — layout computation, conversions, buffer ctor.
 *
 * Implements the conversion helpers declared in
 * `nimrtc/video_frame/frame.hpp`.  Conversion paths are intentionally simple
 * (no SIMD); the real high-perf paths will live in dedicated HW backends
 * (VA-API / DXVA / VideoToolbox) — this module is the portable reference.
 */

#include <nimrtc/video_frame/frame.hpp>

#include <algorithm>
#include <cstring>

namespace nimrtc::video_frame {

namespace {

constexpr std::uint32_t kDefaultStrideAlign = 16;

inline std::int32_t align_up(std::int32_t v, std::uint32_t a) noexcept {
    return static_cast<std::int32_t>((static_cast<std::uint32_t>(v) + a - 1u) & ~(a - 1u));
}

inline void fill_planar_yuv_420_layout(VideoFrameLayout& out,
                                       std::uint32_t w, std::uint32_t h,
                                       std::uint32_t align) noexcept {
    out.format  = out.format;          // already set
    out.width   = w;
    out.height  = h;

    // Y plane: full resolution.
    out.strides[0]    = align_up(static_cast<std::int32_t>(w), align);
    out.plane_offsets[0] = 0;

    // U/V planes: half width (4:2:0 subsampling), half height.
    const std::uint32_t half_w = (w + 1u) / 2u;
    const std::uint32_t half_h = (h + 1u) / 2u;

    out.strides[1] = align_up(static_cast<std::int32_t>(half_w), align);
    out.strides[2] = out.strides[1];

    out.plane_offsets[1] = static_cast<std::size_t>(out.strides[0]) * h;
    out.plane_offsets[2] = out.plane_offsets[1]
                          + static_cast<std::size_t>(out.strides[1]) * half_h;

    out.buffer_size = out.plane_offsets[2]
                    + static_cast<std::size_t>(out.strides[2]) * half_h;
    out.num_planes   = 3;
}

inline void fill_nv12_layout(VideoFrameLayout& out,
                             std::uint32_t w, std::uint32_t h,
                             std::uint32_t align) noexcept {
    out.format  = out.format;
    out.width   = w;
    out.height  = h;

    out.strides[0]    = align_up(static_cast<std::int32_t>(w), align);
    out.plane_offsets[0] = 0;

    // UV plane: half width × 2 (interleaved), half height.
    const std::uint32_t half_w = (w + 1u) / 2u;
    const std::uint32_t half_h = (h + 1u) / 2u;
    out.strides[1] = align_up(static_cast<std::int32_t>(half_w * 2u), align);
    out.strides[2] = 0;
    out.plane_offsets[1] = static_cast<std::size_t>(out.strides[0]) * h;
    out.plane_offsets[2] = 0;
    out.buffer_size = out.plane_offsets[1]
                    + static_cast<std::size_t>(out.strides[1]) * half_h;
    out.num_planes   = 2;
}

inline void fill_packed_layout(VideoFrameLayout& out,
                               std::uint32_t w, std::uint32_t h,
                               std::uint8_t bpp, std::uint32_t align) noexcept {
    const std::int32_t row_bytes = align_up(
        static_cast<std::int32_t>(w * (bpp / 8u)), align);
    out.format  = out.format;
    out.width   = w;
    out.height  = h;
    out.strides[0] = row_bytes;
    out.strides[1] = 0;
    out.strides[2] = 0;
    out.plane_offsets[0] = 0;
    out.plane_offsets[1] = 0;
    out.plane_offsets[2] = 0;
    out.buffer_size = static_cast<std::size_t>(row_bytes) * h;
    out.num_planes   = 1;
}

} // namespace

// ---------------------------------------------------------------------------
// Layout computation
// ---------------------------------------------------------------------------

VideoFrameLayout compute_layout(PixelFormat format,
                                std::uint32_t width,
                                std::uint32_t height) noexcept {
    return compute_layout_aligned(format, width, height, kDefaultStrideAlign);
}

VideoFrameLayout compute_layout_aligned(PixelFormat format,
                                        std::uint32_t width,
                                        std::uint32_t height,
                                        std::uint32_t stride_align) noexcept {
    VideoFrameLayout out{};
    out.format = format;
    if (width == 0 || height == 0) return out;

    stride_align = std::max<std::uint32_t>(stride_align, 1u);

    switch (format) {
        case PixelFormat::kI420:
        case PixelFormat::kYV12:
            fill_planar_yuv_420_layout(out, width, height, stride_align);
            break;
        case PixelFormat::kNV12:
            fill_nv12_layout(out, width, height, stride_align);
            break;
        case PixelFormat::kBGRA:
        case PixelFormat::kRGBA:
            fill_packed_layout(out, width, height, 32, stride_align);
            break;
        case PixelFormat::kRGB24:
            fill_packed_layout(out, width, height, 24, stride_align);
            break;
        default:
            out = {};
            break;
    }
    return out;
}

// ---------------------------------------------------------------------------
// VideoFrameBuffer — ctor + refcount helpers
// ---------------------------------------------------------------------------

namespace {

// We use a small helper to make FrameBufferControl directly constructible.
// Wrapping a separate subclass only adds boilerplate without changing the
// semantics (the atomic ref_count lives in the base class regardless).
struct OwnedFrameControl : public FrameBufferControl {
    OwnedFrameControl(VideoFrameLayout layout) {
        capacity       = layout.buffer_size;
        width          = layout.width;
        height         = layout.height;
        format         = layout.format;
    }
};

struct WrappedFrameControl : public FrameBufferControl {
    WrappedFrameControl(VideoFrameLayout layout, std::size_t capacity) {
        this->capacity = capacity;
        this->width    = layout.width;
        this->height   = layout.height;
        this->format   = layout.format;
    }
};

} // namespace

VideoFrameBuffer::VideoFrameBuffer(VideoFrameLayout layout)
    : layout_(layout) {
    if (layout_.buffer_size == 0) return;
    storage_.resize(layout_.buffer_size);
    ctrl_ = std::make_shared<OwnedFrameControl>(layout);
}

VideoFrameBuffer::VideoFrameBuffer(std::uint8_t* external_data,
                                   std::size_t  external_capacity,
                                   VideoFrameLayout layout,
                                   std::shared_ptr<FrameBufferControl> ctrl)
    : layout_(layout) {
    if (!external_data || external_capacity == 0) return;
    // We do NOT copy external data — we just expose it through data(). This
    // is a "view" with lifetime managed by the external caller.  We use
    // a non-owning vector to keep data() returning a valid pointer.
    storage_.assign(external_data, external_data + external_capacity);
    ctrl_ = std::move(ctrl);
}

// ---------------------------------------------------------------------------
// I420 → BGRA
// ---------------------------------------------------------------------------

void convert_i420_to_bgra(std::uint32_t width,
                          std::uint32_t height,
                          const std::uint8_t* y_plane, std::int32_t y_stride,
                          const std::uint8_t* u_plane, std::int32_t u_stride,
                          const std::uint8_t* v_plane, std::int32_t v_stride,
                          std::uint8_t* bgra_out,
                          std::int32_t bgra_stride) noexcept {
    if (!y_plane || !u_plane || !v_plane || !bgra_out) return;
    if (width == 0 || height == 0) return;

    for (std::uint32_t j = 0; j < height; ++j) {
        const std::uint8_t* y_row = y_plane + static_cast<std::ptrdiff_t>(j) * y_stride;
        const std::uint8_t* u_row = u_plane + static_cast<std::ptrdiff_t>(j / 2) * u_stride;
        const std::uint8_t* v_row = v_plane + static_cast<std::ptrdiff_t>(j / 2) * v_stride;
        std::uint8_t*       dst   = bgra_out + static_cast<std::ptrdiff_t>(j) * bgra_stride;

        for (std::uint32_t i = 0; i < width; ++i) {
            const std::int32_t y = static_cast<std::int32_t>(y_row[i]) - 16;
            const std::int32_t u = static_cast<std::int32_t>(u_row[i / 2]) - 128;
            const std::int32_t v = static_cast<std::int32_t>(v_row[i / 2]) - 128;

            // BT.601 limited-range YUV → RGB (no rounding for perf — perceptual
            // quality is fine for non-HDR preview).
            std::int32_t c = y * 298 + 128;
            std::int32_t r = (c           + 409 * v) >> 8;
            std::int32_t g = (c - 100 * u - 208 * v) >> 8;
            std::int32_t b = (c + 516 * u          ) >> 8;

            auto clamp = [](std::int32_t v) -> std::uint8_t {
                if (v < 0)   return 0;
                if (v > 255) return 255;
                return static_cast<std::uint8_t>(v);
            };

            dst[i * 4 + 0] = clamp(b);   // B
            dst[i * 4 + 1] = clamp(g);   // G
            dst[i * 4 + 2] = clamp(r);   // R
            dst[i * 4 + 3] = 0xFF;       // A
        }
    }
}

// ---------------------------------------------------------------------------
// NV12 → I420 (de-interleave UV)
// ---------------------------------------------------------------------------

void convert_nv12_to_i420(std::uint32_t width,
                          std::uint32_t height,
                          const std::uint8_t* y_plane, std::int32_t y_stride,
                          const std::uint8_t* uv_plane, std::int32_t uv_stride,
                          std::uint8_t* y_out, std::int32_t y_out_stride,
                          std::uint8_t* u_out, std::int32_t u_out_stride,
                          std::uint8_t* v_out, std::int32_t v_out_stride) noexcept {
    if (!y_plane || !uv_plane || !y_out || !u_out || !v_out) return;
    if (width == 0 || height == 0) return;

    const std::uint32_t half_h = (height + 1u) / 2u;
    const std::uint32_t half_w = (width  + 1u) / 2u;

    // Y plane is byte-for-byte identical.
    for (std::uint32_t j = 0; j < height; ++j) {
        std::memcpy(y_out + static_cast<std::ptrdiff_t>(j) * y_out_stride,
                    y_plane + static_cast<std::ptrdiff_t>(j) * y_stride,
                    width);
    }

    // UV plane: split into U and V.
    for (std::uint32_t j = 0; j < half_h; ++j) {
        const std::uint8_t* uv_row = uv_plane + static_cast<std::ptrdiff_t>(j) * uv_stride;
        std::uint8_t* u_row = u_out + static_cast<std::ptrdiff_t>(j) * u_out_stride;
        std::uint8_t* v_row = v_out + static_cast<std::ptrdiff_t>(j) * v_out_stride;
        for (std::uint32_t i = 0; i < half_w; ++i) {
            u_row[i] = uv_row[i * 2 + 0];
            v_row[i] = uv_row[i * 2 + 1];
        }
    }
}

// ---------------------------------------------------------------------------
// I420 → NV12 (interleave UV)
// ---------------------------------------------------------------------------

void convert_i420_to_nv12(std::uint32_t width,
                          std::uint32_t height,
                          const std::uint8_t* y_plane, std::int32_t y_stride,
                          const std::uint8_t* u_plane, std::int32_t u_stride,
                          const std::uint8_t* v_plane, std::int32_t v_stride,
                          std::uint8_t* y_out, std::int32_t y_out_stride,
                          std::uint8_t* uv_out, std::int32_t uv_out_stride) noexcept {
    if (!y_plane || !u_plane || !v_plane || !y_out || !uv_out) return;
    if (width == 0 || height == 0) return;

    const std::uint32_t half_h = (height + 1u) / 2u;
    const std::uint32_t half_w = (width  + 1u) / 2u;

    // Y plane is byte-for-byte identical.
    for (std::uint32_t j = 0; j < height; ++j) {
        std::memcpy(y_out + static_cast<std::ptrdiff_t>(j) * y_out_stride,
                    y_plane + static_cast<std::ptrdiff_t>(j) * y_stride,
                    width);
    }

    // UV plane: merge U and V.
    for (std::uint32_t j = 0; j < half_h; ++j) {
        const std::uint8_t* u_row = u_plane + static_cast<std::ptrdiff_t>(j) * u_stride;
        const std::uint8_t* v_row = v_plane + static_cast<std::ptrdiff_t>(j) * v_stride;
        std::uint8_t* uv_row = uv_out + static_cast<std::ptrdiff_t>(j) * uv_out_stride;
        for (std::uint32_t i = 0; i < half_w; ++i) {
            uv_row[i * 2 + 0] = u_row[i];
            uv_row[i * 2 + 1] = v_row[i];
        }
    }
}

} // namespace nimrtc::video_frame
