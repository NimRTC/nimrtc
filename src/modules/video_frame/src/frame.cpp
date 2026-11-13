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
// Colour-aware conversions (remote-desktop plan C3 / C5)
// ---------------------------------------------------------------------------
//
// The RGB<->YUV maths is the textbook ITU-R BT.601 / BT.709 form:
//
//   Y  =  Kr*R + Kg*G + Kb*B
//   Cb = (B - Y) / (2*(1-Kb))
//   Cr = (R - Y) / (2*(1-Kr))
//
// with the coefficients taken from the *matrix* and the scaling /
// pedestal taken from the *range*.
//
// ## Units
//
// The luma `Y` and the chroma offsets `Cb - 128` / `Cr - 128` are
// carried as **plain 0..255-scaled integers** (i.e. the same units as
// an 8-bit RGB component).  The *coefficients* are Q8.8 (a fraction
// multiplied by 256) and the *gains* are Q16.16.
//
// Forward (RGB -> YUV):
//
//   Y8    = (Kr*R + Kg*G + Kb*B) >> 8              // 0..255, unranged
//   dCb   = (B - Y8) * (1 / (2*(1-Kb))) >> 16      // plain, signed
//   dCr   = (R - Y8) * (1 / (2*(1-Kr))) >> 16      // plain, signed
//   Y_out = Y8   * y_gain + y_offset              // into the target range
//   Cb_out= dCb  * c_gain + 128                   // into the target range
//
// The chroma differences are computed against the **unranged** luma and
// scaled **afterwards**.  Scaling first and differencing after would
// apply the range gain to the difference, which is wrong for a
// limited-range output.
//
// The inverse (YUV -> RGB) walks the same steps backwards, so a
// BGRA -> I420 -> BGRA round trip is the identity (to within the
// rounding of the fixed-point maths).

namespace {

/** The YUV coefficients for a matrix, in Q8.8. */
struct YuvCoefficients {
    std::int32_t kr;       ///< red's luma coefficient, Q8.8
    std::int32_t kg;       ///< green's luma coefficient, Q8.8
    std::int32_t kb;       ///< blue's luma coefficient, Q8.8
    std::int32_t cb_gain;  ///< 1 / (2*(1-Kb)), Q16.16
    std::int32_t cr_gain;  ///< 1 / (2*(1-Kr)), Q16.16
};

/** The luma / chroma scaling for a range. */
struct RangeScaling {
    std::int32_t y_scale;   ///< luma gain, Q16.16
    std::int32_t y_offset;  ///< luma pedestal (16 for limited, 0 for full)
    std::int32_t c_scale;   ///< chroma gain, Q16.16
    std::int32_t c_offset;  ///< chroma pedestal (128 for both ranges)
};

YuvCoefficients coefficients_for(ColorMatrix m) noexcept {
    switch (m) {
        case ColorMatrix::kBT601:
            // Kr = 0.299, Kb = 0.114, Kg = 0.587  (sums to 256)
            // 1/(2*(1-Kb)) = 1/1.772  = 0.56433
            // 1/(2*(1-Kr)) = 1/1.402  = 0.71327
            return {77, 150, 29, 36979, 46751};
        case ColorMatrix::kBT2020:
            // Kr = 0.2627, Kb = 0.0593, Kg = 0.678  (sums to 256)
            // 1/(2*(1-Kb)) = 1/1.8814 = 0.53156
            // 1/(2*(1-Kr)) = 1/1.4746 = 0.67812
            return {67, 174, 15, 34842, 44448};
        case ColorMatrix::kBT709:
        default:
            // Kr = 0.2126, Kb = 0.0722, Kg = 0.7152  (sums to 256)
            // 1/(2*(1-Kb)) = 1/1.8556 = 0.53890
            // 1/(2*(1-Kr)) = 1/1.5748 = 0.63501
            return {54, 184, 18, 35327, 41618};
    }
}

RangeScaling scaling_for(ColorRange r) noexcept {
    switch (r) {
        case ColorRange::kLimited:
            // Y: 0..255 -> 16..235, so gain = 219/255 and offset = 16.
            // C: -128..127 -> -112..112, so gain = 224/255.
            //
            // The chroma gain must be *below* 1.  Limited range
            // compresses the chroma into the studio swing; a gain above
            // 1 pushes saturated colours past 0/255, where they are
            // clamped and lose their hue entirely.  (The BT.601
            // inverse coefficients 1.596027 / 2.017232 require exactly
            // this 224/255 factor.)
            return {56284, 16, 57569, 128};
        case ColorRange::kFull:
        default:
            // Full range: identity scaling.
            return {65536, 0, 65536, 128};
    }
}

/** Round a Q16.16 product to an integer, with rounding. */
inline std::int32_t mul_q16(std::int32_t value, std::int32_t gain) noexcept {
    return static_cast<std::int32_t>(
        (static_cast<std::int64_t>(value) * gain + 32768) >> 16);
}

/** Divide by a Q16.16 gain, with rounding. */
inline std::int32_t div_q16(std::int32_t value, std::int32_t gain) noexcept {
    if (gain == 0) return value;
    return static_cast<std::int32_t>(
        (static_cast<std::int64_t>(value) * 65536 + gain / 2) / gain);
}

/** The RGB components of one BGRA source pixel. */
struct BgraPixel {
    std::int32_t b;
    std::int32_t g;
    std::int32_t r;
};

inline BgraPixel load_bgra(const std::uint8_t* row, std::uint32_t i) noexcept {
    return BgraPixel{
        static_cast<std::int32_t>(row[i * 4 + 0]),
        static_cast<std::int32_t>(row[i * 4 + 1]),
        static_cast<std::int32_t>(row[i * 4 + 2]),
    };
}

/** The luma and the two chroma offsets of one RGB pixel. */
struct YuvSample {
    std::int32_t y;    ///< 0..255, unranged
    std::int32_t cb;   ///< signed, unranged, in 8-bit units
    std::int32_t cr;   ///< signed, unranged, in 8-bit units
};

inline YuvSample rgb_to_yuv(const BgraPixel& px,
                            const YuvCoefficients& c) noexcept {
    const std::int32_t y = (c.kr * px.r + c.kg * px.g + c.kb * px.b) >> 8;
    const std::int32_t cb = static_cast<std::int32_t>(
        (static_cast<std::int64_t>(px.b - y) * c.cb_gain) >> 16);
    const std::int32_t cr = static_cast<std::int32_t>(
        (static_cast<std::int64_t>(px.r - y) * c.cr_gain) >> 16);
    return YuvSample{y, cb, cr};
}

} // namespace

void convert_bgra_to_i420_color(
                          std::uint32_t width,
                          std::uint32_t height,
                          const std::uint8_t* src,
                          std::int32_t src_stride,
                          ColorDescription color,
                          std::uint8_t* y_plane, std::int32_t y_stride,
                          std::uint8_t* u_plane, std::int32_t u_stride,
                          std::uint8_t* v_plane, std::int32_t v_stride,
                          ColorDescription* out_color) noexcept {
    if (width == 0 || height == 0 || src == nullptr) return;
    if (y_plane == nullptr || u_plane == nullptr || v_plane == nullptr) return;

    const ColorDescription desc = resolve_color_description(color);
    const YuvCoefficients c = coefficients_for(desc.matrix);
    const RangeScaling   s = scaling_for(desc.range);

    const std::uint32_t chroma_w = (width + 1u) / 2u;
    const std::uint32_t chroma_h = (height + 1u) / 2u;

    for (std::uint32_t cy = 0; cy < chroma_h; ++cy) {
        const std::uint32_t j0 = cy * 2u;
        const std::uint32_t j1 = (j0 + 1u < height) ? (j0 + 1u) : j0;
        const std::uint8_t* const row0 =
            src + static_cast<std::ptrdiff_t>(j0) * src_stride;
        const std::uint8_t* const row1 =
            src + static_cast<std::ptrdiff_t>(j1) * src_stride;
        std::uint8_t* const y0 =
            y_plane + static_cast<std::ptrdiff_t>(j0) * y_stride;
        std::uint8_t* const y1 =
            y_plane + static_cast<std::ptrdiff_t>(j1) * y_stride;

        for (std::uint32_t cx = 0; cx < chroma_w; ++cx) {
            const std::uint32_t i0 = cx * 2u;
            const std::uint32_t i1 = (i0 + 1u < width) ? (i0 + 1u) : i0;

            // Luma is full resolution: one sample per pixel.
            const YuvSample s00 = rgb_to_yuv(load_bgra(row0, i0), c);
            y0[i0] = static_cast<std::uint8_t>(
                std::clamp(mul_q16(s00.y, s.y_scale) + s.y_offset, 0, 255));

            std::int64_t cb_sum = s00.cb;
            std::int64_t cr_sum = s00.cr;
            std::int32_t samples = 1;

            if (i1 != i0) {
                const YuvSample s01 = rgb_to_yuv(load_bgra(row0, i1), c);
                y0[i1] = static_cast<std::uint8_t>(
                    std::clamp(mul_q16(s01.y, s.y_scale) + s.y_offset, 0, 255));
                cb_sum += s01.cb;
                cr_sum += s01.cr;
                ++samples;
            }

            if (j1 != j0) {
                const YuvSample s10 = rgb_to_yuv(load_bgra(row1, i0), c);
                y1[i0] = static_cast<std::uint8_t>(
                    std::clamp(mul_q16(s10.y, s.y_scale) + s.y_offset, 0, 255));
                cb_sum += s10.cb;
                cr_sum += s10.cr;
                ++samples;

                if (i1 != i0) {
                    const YuvSample s11 = rgb_to_yuv(load_bgra(row1, i1), c);
                    y1[i1] = static_cast<std::uint8_t>(
                        std::clamp(mul_q16(s11.y, s.y_scale) + s.y_offset, 0, 255));
                    cb_sum += s11.cb;
                    cr_sum += s11.cr;
                    ++samples;
                }
            }

            // Chroma is one sample per 2x2 block: the *average* of the
            // block's chroma offsets.  Point-sampling (taking only the
            // top-left pixel of the block) is what a desktop
            // compositor's own scaler does, but it aliases on anything
            // with colour detail and it makes the round trip lossy in a
            // way the caller cannot predict.  Averaging the *unranged*
            // offsets and scaling afterwards is the same operation the
            // range scaling does, so the two stay consistent.
            const std::int32_t cb = static_cast<std::int32_t>(cb_sum / samples);
            const std::int32_t cr = static_cast<std::int32_t>(cr_sum / samples);

            u_plane[static_cast<std::size_t>(cy) *
                        static_cast<std::size_t>(u_stride) + cx] =
                static_cast<std::uint8_t>(
                    std::clamp(mul_q16(cb, s.c_scale) + s.c_offset, 0, 255));
            v_plane[static_cast<std::size_t>(cy) *
                        static_cast<std::size_t>(v_stride) + cx] =
                static_cast<std::uint8_t>(
                    std::clamp(mul_q16(cr, s.c_scale) + s.c_offset, 0, 255));
        }
    }

    if (out_color != nullptr) *out_color = desc;
}

void convert_bgra_to_nv12_color(
                          std::uint32_t width,
                          std::uint32_t height,
                          const std::uint8_t* src,
                          std::int32_t src_stride,
                          ColorDescription color,
                          std::uint8_t* y_plane, std::int32_t y_stride,
                          std::uint8_t* uv_plane, std::int32_t uv_stride,
                          ColorDescription* out_color) noexcept {
    if (width == 0 || height == 0 || src == nullptr) return;
    if (y_plane == nullptr || uv_plane == nullptr) return;

    const ColorDescription desc = resolve_color_description(color);
    const YuvCoefficients c = coefficients_for(desc.matrix);
    const RangeScaling   s = scaling_for(desc.range);

    const std::uint32_t chroma_w = (width + 1u) / 2u;
    const std::uint32_t chroma_h = (height + 1u) / 2u;

    for (std::uint32_t cy = 0; cy < chroma_h; ++cy) {
        const std::uint32_t j0 = cy * 2u;
        const std::uint32_t j1 = (j0 + 1u < height) ? (j0 + 1u) : j0;
        const std::uint8_t* const row0 =
            src + static_cast<std::ptrdiff_t>(j0) * src_stride;
        const std::uint8_t* const row1 =
            src + static_cast<std::ptrdiff_t>(j1) * src_stride;
        std::uint8_t* const y0 =
            y_plane + static_cast<std::ptrdiff_t>(j0) * y_stride;
        std::uint8_t* const y1 =
            y_plane + static_cast<std::ptrdiff_t>(j1) * y_stride;

        for (std::uint32_t cx = 0; cx < chroma_w; ++cx) {
            const std::uint32_t i0 = cx * 2u;
            const std::uint32_t i1 = (i0 + 1u < width) ? (i0 + 1u) : i0;

            const YuvSample s00 = rgb_to_yuv(load_bgra(row0, i0), c);
            y0[i0] = static_cast<std::uint8_t>(
                std::clamp(mul_q16(s00.y, s.y_scale) + s.y_offset, 0, 255));

            std::int64_t cb_sum = s00.cb;
            std::int64_t cr_sum = s00.cr;
            std::int32_t samples = 1;

            if (i1 != i0) {
                const YuvSample s01 = rgb_to_yuv(load_bgra(row0, i1), c);
                y0[i1] = static_cast<std::uint8_t>(
                    std::clamp(mul_q16(s01.y, s.y_scale) + s.y_offset, 0, 255));
                cb_sum += s01.cb;
                cr_sum += s01.cr;
                ++samples;
            }

            if (j1 != j0) {
                const YuvSample s10 = rgb_to_yuv(load_bgra(row1, i0), c);
                y1[i0] = static_cast<std::uint8_t>(
                    std::clamp(mul_q16(s10.y, s.y_scale) + s.y_offset, 0, 255));
                cb_sum += s10.cb;
                cr_sum += s10.cr;
                ++samples;

                if (i1 != i0) {
                    const YuvSample s11 = rgb_to_yuv(load_bgra(row1, i1), c);
                    y1[i1] = static_cast<std::uint8_t>(
                        std::clamp(mul_q16(s11.y, s.y_scale) + s.y_offset, 0, 255));
                    cb_sum += s11.cb;
                    cr_sum += s11.cr;
                    ++samples;
                }
            }

            // Averaged 2x2 chroma — see convert_bgra_to_i420_color.
            const std::int32_t cb = static_cast<std::int32_t>(cb_sum / samples);
            const std::int32_t cr = static_cast<std::int32_t>(cr_sum / samples);

            const std::size_t idx =
                static_cast<std::size_t>(cy) * static_cast<std::size_t>(uv_stride)
                + static_cast<std::size_t>(cx) * 2u;
            uv_plane[idx + 0] = static_cast<std::uint8_t>(
                std::clamp(mul_q16(cb, s.c_scale) + s.c_offset, 0, 255));
            uv_plane[idx + 1] = static_cast<std::uint8_t>(
                std::clamp(mul_q16(cr, s.c_scale) + s.c_offset, 0, 255));
        }
    }

    if (out_color != nullptr) *out_color = desc;
}

void convert_i420_to_bgra_color(
                          std::uint32_t width,
                          std::uint32_t height,
                          const std::uint8_t* y_plane, std::int32_t y_stride,
                          const std::uint8_t* u_plane, std::int32_t u_stride,
                          const std::uint8_t* v_plane, std::int32_t v_stride,
                          ColorDescription color,
                          std::uint8_t* bgra_out,
                          std::int32_t bgra_stride) noexcept {
    if (width == 0 || height == 0) return;
    if (y_plane == nullptr || u_plane == nullptr || v_plane == nullptr) return;
    if (bgra_out == nullptr) return;

    const ColorDescription desc = resolve_color_description(color);
    const YuvCoefficients c = coefficients_for(desc.matrix);
    const RangeScaling   s = scaling_for(desc.range);

    // Back to RGB:
    //   R = Y + 2*(1-Kr)*Cr
    //   B = Y + 2*(1-Kb)*Cb
    //   G = Y - (2*Kr*(1-Kr)/Kg)*Cr - (2*Kb*(1-Kb)/Kg)*Cb
    // The gains are Q8.8.
    const std::int32_t kr8 = c.kr;
    const std::int32_t kb8 = c.kb;
    const std::int32_t kg8 = c.kg;
    const std::int32_t r_gain = 512 - 2 * kr8;                 // 2*(1-Kr), Q8.8
    const std::int32_t b_gain = 512 - 2 * kb8;                 // 2*(1-Kb), Q8.8
    // G = Y - [2*Kr*(1-Kr)/Kg]*Cr - [2*Kb*(1-Kb)/Kg]*Cb
    //
    // The coefficient is 2*Kr*(1-Kr)/Kg.  With Kr = kr8/256 and
    // (1-Kr) = r_gain/512, that expands to
    //
    //   2*Kr*(1-Kr)/Kg = kr8 * r_gain / kg8      (Q8.8)
    //
    // — note the *single* factor of 2.  Folding `r_gain` (which
    // already carries a 2) into a second explicit 2 doubles the green
    // correction and turns every saturated colour into a hue shift.
    const std::int32_t g_r_gain = (kr8 * r_gain) / kg8;    // Q8.8
    const std::int32_t g_b_gain = (kb8 * b_gain) / kg8;    // Q8.8

    for (std::uint32_t j = 0; j < height; ++j) {
        const std::uint8_t* y_row =
            y_plane + static_cast<std::ptrdiff_t>(j) * y_stride;
        const std::uint8_t* u_row =
            u_plane + static_cast<std::ptrdiff_t>(j / 2u) * u_stride;
        const std::uint8_t* v_row =
            v_plane + static_cast<std::ptrdiff_t>(j / 2u) * v_stride;
        std::uint8_t* dst =
            bgra_out + static_cast<std::ptrdiff_t>(j) * bgra_stride;

        for (std::uint32_t i = 0; i < width; ++i) {
            // Undo the range scaling: recover the unranged luma and the
            // signed, unranged chroma offsets.
            const std::int32_t y = div_q16(
                static_cast<std::int32_t>(y_row[i]) - s.y_offset, s.y_scale);
            const std::int32_t cb = div_q16(
                static_cast<std::int32_t>(u_row[i / 2u]) - s.c_offset,
                s.c_scale);
            const std::int32_t cr = div_q16(
                static_cast<std::int32_t>(v_row[i / 2u]) - s.c_offset,
                s.c_scale);

            const std::int32_t r = y + ((r_gain * cr) >> 8);
            const std::int32_t b = y + ((b_gain * cb) >> 8);
            const std::int32_t g = y - ((g_r_gain * cr) >> 8)
                                     - ((g_b_gain * cb) >> 8);

            dst[i * 4 + 0] = static_cast<std::uint8_t>(std::clamp(b, 0, 255));  // B
            dst[i * 4 + 1] = static_cast<std::uint8_t>(std::clamp(g, 0, 255));  // G
            dst[i * 4 + 2] = static_cast<std::uint8_t>(std::clamp(r, 0, 255));  // R
            dst[i * 4 + 3] = 0xFF;                                              // A
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
