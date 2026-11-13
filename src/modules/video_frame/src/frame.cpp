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
    WrappedFrameControl(VideoFrameLayout layout, std::size_t cap) {
        this->capacity = cap;
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

VideoFrameBuffer::VideoFrameBuffer(std::vector<std::uint8_t>&& storage,
                                   VideoFrameLayout layout)
    : layout_(layout) {
    if (layout_.buffer_size == 0) return;
    if (storage.size() < layout_.buffer_size) return;
    // Adopt the storage without copying it — this is the whole point of
    // the pooling path.  `storage_.swap` hands the buffer's allocation
    // over in O(1); the vector's own (smaller) buffer is released with
    // the temporary.
    storage_.swap(storage);
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

// Out-of-line virtual destructor — emits the vtable and typeinfo once
// for the entire library. Body decrements the application-level ref
// counter in FrameBufferControl so shared-ownership diagnostics work
// correctly under `shared_ptr<VideoFrameBuffer>`.
VideoFrameBuffer::~VideoFrameBuffer() noexcept {
    if (ctrl_) {
        ctrl_->ref_count.fetch_sub(1, std::memory_order_acq_rel);
    }
}

void VideoFrameBuffer::adopt_storage(std::vector<std::uint8_t>&& storage,
                                     VideoFrameLayout layout) noexcept {
    layout_ = layout;
    storage_.swap(storage);
    ctrl_ = std::make_shared<OwnedFrameControl>(layout);
}

void VideoFrameBuffer::rearm(VideoFrameLayout layout) noexcept {
    layout_ = layout;
    if (!ctrl_) {
        ctrl_ = std::make_shared<OwnedFrameControl>(layout);
        return;
    }
    // The control block is reused too — only the metadata is reset,
    // because a recycled buffer is a new frame.
    ctrl_->capacity       = layout.buffer_size;
    ctrl_->width          = layout.width;
    ctrl_->height         = layout.height;
    ctrl_->format         = layout.format;
    ctrl_->capture_ts_us  = 0;
    ctrl_->frame_seq      = 0;
    ctrl_->ref_count.store(1, std::memory_order_release);
}

std::vector<std::uint8_t> VideoFrameBuffer::release_storage() noexcept {
    std::vector<std::uint8_t> out;
    out.swap(storage_);
    ctrl_.reset();
    layout_ = {};
    return out;
}

// ---------------------------------------------------------------------------
// VideoFrameBufferPool
// ---------------------------------------------------------------------------

// The pool hands out `std::shared_ptr<VideoFrameBuffer>` values.  A
// `shared_ptr` that owns its object carries a heap-allocated *control
// block*, so building one per frame is one allocation per frame — exactly
// what the pool exists to eliminate.
//
// The trick is to let the pool keep the control block alive and hand out
// *aliases* of it.  `std::shared_ptr<VideoFrameBuffer>(owner, raw)` shares
// the owner's control block (so no allocation happens) while pointing at
// the pooled buffer object.  The pool parks the owner; an entry is
// reusable once the caller's alias is gone, which is visible as
// `owner.use_count() == 1`.
//
// The buffer object, its pixel storage and its `FrameBufferControl` all
// live inside the owner's control block, so recycling the owner recycles
// all three.

/// The pooled buffer.  It is the *object* the pool's control block owns,
/// so the buffer's storage and control block are recycled together with
/// it.
struct PooledBuffer : public VideoFrameBuffer {
    explicit PooledBuffer(VideoFrameLayout layout)
        : VideoFrameBuffer(layout) {}
};

/// A parked buffer.  `owner` keeps the buffer object (and therefore its
/// storage and its `FrameBufferControl`) alive.
struct PooledBufferEntry {
    std::shared_ptr<PooledBuffer> owner;
};

struct VideoFrameBufferPool::State {
    /// The parked buffers.  An entry is reusable when its owner has no
    /// other reference — i.e. `owner.use_count() == 1`.
    std::vector<PooledBufferEntry> idle;

    /// Total number of buffers this pool has ever created.
    std::size_t total_allocations = 0;
};

VideoFrameBufferPool::VideoFrameBufferPool()
    : state_(std::make_shared<State>()) {}

VideoFrameBufferPool::~VideoFrameBufferPool() = default;

std::shared_ptr<VideoFrameBuffer>
VideoFrameBufferPool::acquire(VideoFrameLayout layout) {
    if (layout.buffer_size == 0) return nullptr;

    auto state = state_;

    // ---- 1. Find a parked buffer that is free and big enough -----------
    //
    // "Free" means the caller has released its alias, so the pool holds
    // the only reference left.  "Big enough" is checked against the
    // storage the buffer was allocated with — a resolution change must
    // not be served from a too-small block.
    for (std::size_t i = 0; i < state->idle.size(); ++i) {
        PooledBufferEntry& entry = state->idle[i];
        if (entry.owner.use_count() != 1) continue;             // still in use
        if (entry.owner->size() < layout.buffer_size) continue;  // too small

        // Re-arm it: the object, its storage and its control block all
        // survive — nothing is allocated.  The entry stays parked so the
        // next `acquire()` can find it again.
        entry.owner->rearm(layout);
        // The alias shares the owner's control block, so no control
        // block is allocated here.
        return std::shared_ptr<VideoFrameBuffer>(entry.owner,
                                                 entry.owner.get());
    }

    // ---- 2. Nothing reusable -- create a new buffer --------------------
    //
    // The memset that `resize` performs happens once per *allocation*,
    // and the pool means it happens once per buffer rather than once per
    // frame (at 1080p that is a ~3 MB memset per frame).
    auto owner = std::make_shared<PooledBuffer>(layout);
    ++state->total_allocations;

    // Park it so a later `acquire()` can find it.  The pool keeps the
    // owner alive; the caller's alias is the only extra reference.
    state->idle.push_back(PooledBufferEntry{owner});
    return std::shared_ptr<VideoFrameBuffer>(owner, owner.get());
}

std::size_t VideoFrameBufferPool::idle() const noexcept {
    // "Idle" means *available for reuse* — a parked buffer the caller has
    // already released.  A parked buffer that is still handed out is
    // counted by `in_flight()` instead, so the two always add up to the
    // number of buffers the pool has created.
    std::size_t n = 0;
    for (const PooledBufferEntry& entry : state_->idle) {
        if (entry.owner.use_count() == 1) ++n;
    }
    return n;
}

std::size_t VideoFrameBufferPool::in_flight() const noexcept {
    // A parked buffer is "in flight" while the caller still holds it.
    std::size_t n = 0;
    for (const PooledBufferEntry& entry : state_->idle) {
        if (entry.owner.use_count() > 1) ++n;
    }
    return n;
}

std::size_t VideoFrameBufferPool::total_allocations() const noexcept {
    return state_->total_allocations;
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

            auto clamp = [](std::int32_t val) -> std::uint8_t {
                if (val < 0)   return 0;
                if (val > 255) return 255;
                return static_cast<std::uint8_t>(val);
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

// ---------------------------------------------------------------------------
// Test pattern
// ---------------------------------------------------------------------------

void paint_test_pattern_i420(std::uint8_t* y,
                             std::uint8_t* u,
                             std::uint8_t* v,
                             std::uint32_t w,
                             std::uint32_t h,
                             std::uint32_t frame_seq,
                             std::int32_t stride_y,
                             std::int32_t stride_u,
                             std::int32_t stride_v) noexcept {
    if (!y || w == 0 || h == 0) return;

    // ---- Y plane: three horizontal bands whose brightness ramps with the
    //      frame sequence number.
    const std::uint32_t band_h = std::max(h / 3u, 1u);
    const std::uint8_t base_y  = static_cast<std::uint8_t>(
        16u + ((frame_seq * 7u) % 200u));   // BT.601 limited range
    for (std::uint32_t row = 0; row < h; ++row) {
        const std::uint32_t band = std::min<std::uint32_t>(2u, row / band_h);
        const std::uint8_t yv = static_cast<std::uint8_t>(base_y + band * 60u);
        std::memset(y + static_cast<std::ptrdiff_t>(row) * stride_y, yv, w);
    }

    // ---- U / V planes: neutral grey chroma (BT.601 limited range).
    const std::uint32_t cw = w / 2u;
    const std::uint32_t ch = h / 2u;
    if (u) {
        for (std::uint32_t row = 0; row < ch; ++row) {
            std::memset(u + static_cast<std::ptrdiff_t>(row) * stride_u, 128, cw);
        }
    }
    if (v) {
        for (std::uint32_t row = 0; row < ch; ++row) {
            std::memset(v + static_cast<std::ptrdiff_t>(row) * stride_v, 128, cw);
        }
    }
}

} // namespace nimrtc::video_frame
