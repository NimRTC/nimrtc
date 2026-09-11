/**
 * @file nimrtc/video_frame/frame.hpp
 * @brief Video frame types — raw pixel buffers + encoded bitstream wrappers.
 *
 * Per ARCHITECTURE.md §2.3 (nimrtc_video_frame, P1 minimal):
 *   - VideoFrameBuffer: refcounted pooled pixel buffer (zero-copy SFU path, D5)
 *   - VideoFrameLayout: per-color-format stride / plane offsets
 *   - VideoFrameInfo:   capture-time metadata (per §8.4 timeline)
 *   - EncodedVideoFrame: codec bitstream + Annex B / length-prefixed framing
 *   - Conversion helpers: I420 ↔ NV12, I420 → BGRA (display path)
 *
 * The codec-agnostic `EncodedVideoFrame` lives here too so the video_payload,
 * video_jb and h264 modules all share the same struct.  Codec-specific
 * configuration (H.264 SPS / PPS, VP8 descriptor) lives in the codec modules.
 *
 * ## Why a separate module (vs putting types in plugins/video_codec.hpp)
 *
 * Plugins are interfaces; concrete frame objects are domain types.  Keeping
 * the implementation details here lets us:
 *   - Add conversions without touching the plugin layer
 *   - Unit-test layouts without instantiating an IVideoCodec
 *   - Compile SFU forwarding paths without dragging in plugin dependencies
 *
 * @note P1. Independent module, no engine integration yet.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <nimrtc/core/bytes.hpp>

namespace nimrtc::video_frame {

// ---------------------------------------------------------------------------
// Pixel format
// ---------------------------------------------------------------------------

enum class PixelFormat : std::uint8_t {
    kUnknown = 0,
    kI420    = 1,   ///< YUV 4:2:0 planar (YYYY... UUU... VVV...)
    kNV12    = 2,   ///< YUV 4:2:0 semi-planar (YYYY... UVUVUV...)
    kYV12    = 3,   ///< YUV 4:2:0 planar (YYYY... VVV... UUU...)
    kBGRA    = 4,   ///< 32-bit BGRA (display / compositing)
    kRGBA    = 5,   ///< 32-bit RGBA
    kRGB24   = 6,   ///< 24-bit RGB
};

/** Human-readable pixel format name (for logging). */
inline std::string_view pixel_format_name(PixelFormat f) noexcept {
    switch (f) {
        case PixelFormat::kI420:  return "I420";
        case PixelFormat::kNV12:  return "NV12";
        case PixelFormat::kYV12:  return "YV12";
        case PixelFormat::kBGRA:  return "BGRA";
        case PixelFormat::kRGBA:  return "RGBA";
        case PixelFormat::kRGB24: return "RGB24";
        default:                  return "Unknown";
    }
}

/** Bits per pixel for packed formats, 0 for planar (use plane strides). */
inline std::uint8_t bits_per_pixel(PixelFormat f) noexcept {
    switch (f) {
        case PixelFormat::kBGRA:
        case PixelFormat::kRGBA:  return 32;
        case PixelFormat::kRGB24: return 24;
        default:                  return 0;   // planar
    }
}

/** Whether a pixel format is planar YUV 4:2:0 (most video codecs). */
inline bool is_planar_yuv_420(PixelFormat f) noexcept {
    return f == PixelFormat::kI420
        || f == PixelFormat::kNV12
        || f == PixelFormat::kYV12;
}

// ---------------------------------------------------------------------------
// Layout — strides / plane offsets for a given (format, width, height)
// ---------------------------------------------------------------------------

/** Per-plane byte offsets for a frame of given dimensions. */
struct VideoFrameLayout {
    PixelFormat format = PixelFormat::kUnknown;

    std::uint32_t width  = 0;
    std::uint32_t height = 0;

    /** Stride (bytes per row) for each plane. Always >= width (or width/2). */
    std::int32_t strides[3] = {0, 0, 0};

    /** Plane offsets from the start of the pixel buffer (in bytes). */
    std::size_t  plane_offsets[3] = {0, 0, 0};

    /** Total buffer size needed (in bytes). */
    std::size_t  buffer_size = 0;

    /** Number of valid planes (1 = BGRA, 2 = NV12, 3 = I420/YV12). */
    std::uint8_t num_planes = 0;
};

/** Compute the layout for a (format, width, height) triple.
 *  Strides are aligned to 16 bytes for SSE/NEON-friendliness (no perf cost
 *  on modern HW, and lets decoders produce aligned buffers). */
VideoFrameLayout compute_layout(PixelFormat format,
                                std::uint32_t width,
                                std::uint32_t height) noexcept;

/** Compute the layout with custom stride alignment. */
VideoFrameLayout compute_layout_aligned(PixelFormat format,
                                       std::uint32_t width,
                                       std::uint32_t height,
                                       std::uint32_t stride_align) noexcept;

// ---------------------------------------------------------------------------
// VideoFrameBuffer — pooled refcounted pixel storage
// ---------------------------------------------------------------------------

/** Internal buffer control block. */
struct FrameBufferControl {
    std::atomic<std::uint32_t> ref_count{1};
    std::size_t   capacity = 0;     ///< total bytes allocated
    std::uint32_t width    = 0;
    std::uint32_t height   = 0;
    PixelFormat   format   = PixelFormat::kUnknown;
    std::int64_t  capture_ts_us = 0;
    std::uint32_t frame_seq    = 0;

    virtual ~FrameBufferControl() = default;

protected:
    FrameBufferControl() = default;
    FrameBufferControl(const FrameBufferControl&) = delete;
    FrameBufferControl& operator=(const FrameBufferControl&) = delete;
};

/**
 * @brief Reference-counted pixel buffer (zero-copy forwarding primitive).
 *
 * Mirrors WebRTC's VideoFrameBuffer (and Chrome's media::VideoFrame).  The
 * `data()` span is valid as long as one shared_ptr holds the buffer.
 *
 * Used by:
 *   - Decoder: produces an owned VideoFrameBuffer
 *   - JitterBuffer: emits frames as VideoFrame (with shared buffer)
 *   - SFU forwarder: shares the same VideoFrameBuffer across N peers (D5:
 *     "reference-counted media buffer, zero-copy forward path")
 *
 * Thread-safety: ref count is atomic; `data()` access requires the caller to
 * keep a shared_ptr alive (which is the normal pattern).
 */
class VideoFrameBuffer {
public:
    /** Probe used by `VideoFrame::has_gpu()` to disambiguate the type-
     *  erased buffer. Default CPU-side implementation returns false.
     *  A derived / re-typed buffer (e.g. a GpuBuffer viewed through a
     *  shared_ptr<void>) overrides this to return true when a real
     *  GPU handle is attached. */
    [[nodiscard]] virtual bool has_gpu_handle() const noexcept {
        return false;
    }
    /// Virtual destructor — required so derived (GpuBuffer) and base
    /// (VideoFrameBuffer) can coexist in a shared_ptr<void> storage and
    /// delete via the right vtable entry. Out-of-line empty body in
    /// frame.cpp so the vtable and typeinfo are emitted in exactly one
    /// translation unit (the library's).
    virtual ~VideoFrameBuffer() noexcept;

    VideoFrameBuffer() noexcept = default;

    /** Allocate a new buffer with the given layout. */
    explicit VideoFrameBuffer(VideoFrameLayout layout);

    /** Wrap an externally-owned block (no ownership transfer; we hold a
     *  control block that does NOT free data). Used by decoders that
     *  produce from a fixed pool. */
    VideoFrameBuffer(std::uint8_t* external_data,
                     std::size_t  external_capacity,
                     VideoFrameLayout layout,
                     std::shared_ptr<FrameBufferControl> ctrl);

    // Copy / move: must update ctrl_->ref_count manually because the
    // default-generated operator= only copies the shared_ptr (which has
    // its own internal ref count) but NOT our application-level ref_count
    // counter in FrameBufferControl.
    VideoFrameBuffer(const VideoFrameBuffer& other) noexcept
        : layout_(other.layout_),
          storage_(other.storage_),
          ctrl_(other.ctrl_) {
        if (ctrl_) ctrl_->ref_count.fetch_add(1, std::memory_order_acq_rel);
    }
    VideoFrameBuffer& operator=(const VideoFrameBuffer& other) noexcept {
        if (this == &other) return *this;
        // Decrement our current ref count.
        if (ctrl_) {
            const auto prev =
                ctrl_->ref_count.fetch_sub(1, std::memory_order_acq_rel);
            (void)prev;   // shared_ptr will free when its count hits zero
        }
        layout_  = other.layout_;
        storage_ = other.storage_;
        ctrl_    = other.ctrl_;
        if (ctrl_) ctrl_->ref_count.fetch_add(1, std::memory_order_acq_rel);
        return *this;
    }
    VideoFrameBuffer(VideoFrameBuffer&& other) noexcept
        : layout_(other.layout_),
          storage_(std::move(other.storage_)),
          ctrl_(std::move(other.ctrl_)) {
        other.layout_ = {};
        other.storage_.clear();
        // ctrl_ moved; ref_count unchanged because ownership transferred.
    }
    VideoFrameBuffer& operator=(VideoFrameBuffer&& other) noexcept {
        if (this == &other) return *this;
        if (ctrl_) {
            const auto prev =
                ctrl_->ref_count.fetch_sub(1, std::memory_order_acq_rel);
            (void)prev;
        }
        layout_  = other.layout_;
        storage_ = std::move(other.storage_);
        ctrl_    = std::move(other.ctrl_);
        other.layout_ = {};
        other.storage_.clear();
        return *this;
    }

    /** Returns true iff this buffer holds valid pixel data. */
    [[nodiscard]] bool valid() const noexcept { return ctrl_ != nullptr; }

    /** Layout (format, dimensions, strides, offsets). */
    [[nodiscard]] const VideoFrameLayout& layout() const noexcept { return layout_; }

    /** Mutable raw byte pointer (for decoder writes). Lifetime: same as buffer. */
    [[nodiscard]] std::uint8_t* data() noexcept {
        return storage_.empty() ? nullptr : storage_.data();
    }

    /** Read-only raw byte pointer. */
    [[nodiscard]] const std::uint8_t* data() const noexcept {
        return storage_.empty() ? nullptr : storage_.data();
    }

    /** Read-only span over the whole buffer. */
    [[nodiscard]] core::ByteSpan as_bytes() const noexcept {
        return core::ByteSpan{storage_.data(), storage_.size()};
    }

    /** Pointer to plane N (0-based). */
    [[nodiscard]] std::uint8_t* plane(std::uint8_t n) noexcept {
        if (n >= 3 || !ctrl_) return nullptr;
        return data() + layout_.plane_offsets[n];
    }

    [[nodiscard]] const std::uint8_t* plane(std::uint8_t n) const noexcept {
        if (n >= 3 || !ctrl_) return nullptr;
        return data() + layout_.plane_offsets[n];
    }

    /** Current ref count (for tests / diagnostics). */
    [[nodiscard]] std::uint32_t ref_count() const noexcept {
        return ctrl_ ? ctrl_->ref_count.load(std::memory_order_acquire) : 0;
    }

    /** Width / height (cached from layout). */
    [[nodiscard]] std::uint32_t width()  const noexcept { return layout_.width;  }
    [[nodiscard]] std::uint32_t height() const noexcept { return layout_.height; }
    [[nodiscard]] PixelFormat   format() const noexcept { return layout_.format; }

    /** Total buffer size in bytes. */
    [[nodiscard]] std::size_t   size() const noexcept { return storage_.size(); }

    /** Capture timestamp (microseconds). */
    [[nodiscard]] std::int64_t  capture_ts_us() const noexcept {
        return ctrl_ ? ctrl_->capture_ts_us : 0;
    }
    void set_capture_ts_us(std::int64_t v) noexcept {
        if (ctrl_) ctrl_->capture_ts_us = v;
    }

    /** Frame sequence number (per SSRC). */
    [[nodiscard]] std::uint32_t frame_seq() const noexcept {
        return ctrl_ ? ctrl_->frame_seq : 0;
    }
    void set_frame_seq(std::uint32_t v) noexcept {
        if (ctrl_) ctrl_->frame_seq = v;
    }

    /** Drop our ref — caller can keep using their own shared_ptr.
     *  Also decrements the application-level ref counter so external
     *  diagnostics see the same lifetime the shared_ptr observes. */
    void reset() noexcept {
        if (ctrl_) {
            ctrl_->ref_count.fetch_sub(1, std::memory_order_acq_rel);
        }
        ctrl_.reset();
        storage_.clear();
        layout_ = {};
    }

private:
    VideoFrameLayout layout_{};
    std::vector<std::uint8_t> storage_;             ///< owned when not wrapping external
    std::shared_ptr<FrameBufferControl> ctrl_;       ///< refcount + metadata
};

// ---------------------------------------------------------------------------
// GPU handle — platform-agnostic wrapper around HW surface / texture / buffer
// ---------------------------------------------------------------------------
//
// Per ARCHITECTURE.md §6.4 (zero-copy HW path):
//   - capture (camera/screen)   → HW encoder → bitstream over the wire
//   - bitstream over the wire    → HW decoder → HW surface/texture → display
//
// `GpuHandle` is a `void*` + `GpuBackend` enum — it does NOT depend on any
// platform SDK. Concrete plugins cast it to:
//   - ANativeWindow*          (Android MediaCodec / Camera2)
//   - CVPixelBufferRef        (Apple VideoToolbox / AVFoundation)
//   - ID3D11Texture2D* / IMFSample  (Windows MediaFoundation / DXVA / NVENC)
//   - VASurfaceID / VAImage   (Linux VAAPI)
//   - CUarray / CUdeviceptr   (NVIDIA NVDEC/NVENC)
//   - MMAL_BUFFER_HEADER_T*   (Raspberry Pi MMAL)
//   - AHardwareBuffer*        (Android Vulkan / cross-API zero-copy)
//
// The handle's lifetime is managed by a shared `GpuBuffer` (next section).
// `GpuBufferPool` is the per-backend arena that allocates + recycles handles.

/** Platform-specific GPU resource identifier. */
enum class GpuBackend : std::uint8_t {
    kNone           = 0,   ///< CPU path (no GPU handle); `handle` is nullptr.
    kAndroidNativeWindow = 1,  ///< ANativeWindow* (Android)
    kAndroidSurfaceTexture = 2, ///< ASurfaceTexture (Android Vulkan / GL)
    kAndroidHardwareBuffer = 3,///< AHardwareBuffer* (Android Vulkan / cross-API)
    kAppleCFPixelBuffer = 4,   ///< CVPixelBufferRef (Apple)
    kAppleMTLTexture    = 5,   ///< id<MTLTexture> (Apple Metal)
    kWindowsD3D11Texture = 6,  ///< ID3D11Texture2D* (Windows DXVA / NVENC)
    kWindowsMFTransform  = 7,  ///< IMFSample* / IMFTransform* (Windows MF)
    kLinuxVaapiSurface   = 8,  ///< VASurfaceID (Linux VAAPI)
    kLinuxVaapiDRM       = 9,  ///< VASurfaceID + DRM fd (Linux DMABUF)
    kNvidiaCudaArray     = 10, ///< CUarray (NVIDIA NVDEC/NVENC)
    kNvidiaCudaDevicePtr = 11, ///< CUdeviceptr (NVIDIA video memory)
    kLinuxV4l2Buffer     = 12, ///< V4L2 DMA-buf fd (Linux V4L2 M2M)
    kRpiMmalBuffer       = 13, ///< MMAL_BUFFER_HEADER_T* (Raspberry Pi)
    kOpenGLTexture       = 14, ///< GLuint texture name (cross-API shared)
    kVulkanImage         = 15, ///< VkImage + VkDeviceMemory (cross-API)
    kMetalIOSurface      = 16, ///< IOSurfaceRef (Apple cross-API)
    kCustom              = 254,///< vendor-private
};

/** One GPU resource handle. Owned by a GpuBuffer; never standalone. */
struct GpuHandle {
    GpuBackend backend = GpuBackend::kNone;
    void*      handle  = nullptr;  ///< type per `backend`
    /** Native row stride / pitch in bytes (0 if not applicable). Some
     *  backends (D3D11, Metal, Vulkan) have implicit layout; some
     *  (VAAPI, CUDA) expose explicit pitch. */
    std::uint32_t native_pitch_bytes = 0;
    /** DMA-buf fd for cross-process / cross-API sharing (Linux only,
     *  Android AHardwareBuffer). -1 if not applicable. */
    int native_fd = -1;
};

/** Human-readable GpuBackend name (for logging). */
inline std::string_view gpu_backend_name(GpuBackend b) noexcept {
    switch (b) {
        case GpuBackend::kNone:                  return "none";
        case GpuBackend::kAndroidNativeWindow:   return "android-surface";
        case GpuBackend::kAndroidSurfaceTexture: return "android-st";
        case GpuBackend::kAndroidHardwareBuffer: return "android-hwbuf";
        case GpuBackend::kAppleCFPixelBuffer:    return "apple-cvpx";
        case GpuBackend::kAppleMTLTexture:       return "apple-mtl";
        case GpuBackend::kWindowsD3D11Texture:   return "win-d3d11";
        case GpuBackend::kWindowsMFTransform:    return "win-mf";
        case GpuBackend::kLinuxVaapiSurface:     return "vaapi";
        case GpuBackend::kLinuxVaapiDRM:         return "vaapi-drm";
        case GpuBackend::kNvidiaCudaArray:       return "cuda-array";
        case GpuBackend::kNvidiaCudaDevicePtr:   return "cuda-ptr";
        case GpuBackend::kLinuxV4l2Buffer:       return "v4l2";
        case GpuBackend::kRpiMmalBuffer:         return "rpi-mmal";
        case GpuBackend::kOpenGLTexture:         return "gl-texture";
        case GpuBackend::kVulkanImage:           return "vk-image";
        case GpuBackend::kMetalIOSurface:        return "iosurface";
        case GpuBackend::kCustom:                return "custom";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// GpuBuffer — refcounted GPU surface / texture with optional CPU mirror
// ---------------------------------------------------------------------------
//
// One GpuBuffer corresponds to one decoded picture. Two access modes:
//   1. **GPU-only** — handle is set, `cpu_mirror` is null. Used in the
//      zero-copy path: camera → encoder → network → decoder → display,
//      where every hop stays in GPU memory.
//   2. **CPU mirror** — handle is set AND `cpu_mirror` is a valid I420/BGRA
//      pointer (for fallback rendering, snapshot for analytics, or when
//      the next consumer doesn't know how to consume the GPU handle).
//
// The `cpu_mirror` is a **lazy** write-through: a consumer may populate it
// once (e.g. a software decoder that always produces CPU pixels). A
// subsequent GPU consumer may still read the handle without going through
// `cpu_mirror`. The mirror is invalidated (set to null) on the next frame
// recycle from the pool — see GpuBufferPool.

/// `GpuBuffer` carries the platform-native GPU handle (CVPixelBuffer,
/// VASurface, ID3D11Texture, …). Pool-allocated, refcounted, and tied
/// to a backend-specific `GpuBufferPool`. Subclass of `VideoFrameBuffer`
/// so `dynamic_pointer_cast<GpuBuffer>` works correctly inside the
/// type-erased `VideoFrame::buffer` slot.
class GpuBuffer : public VideoFrameBuffer {
public:
    /// Inherit the virtual destructor so the polymorphic type stays
    /// polymorphic when deleted through a `shared_ptr<VideoFrameBuffer>`.
    ~GpuBuffer() override = default;

    GpuBuffer() noexcept = default;
    GpuBuffer(GpuBackend backend, void* handle, std::uint32_t width,
              std::uint32_t height, PixelFormat format) noexcept
        : handle_{backend, handle, 0, -1},
          width_(width), height_(height), format_(format) {}

    /// Construct with a pre-filled handle (for sharing DMABUF / pitch).
    GpuBuffer(GpuHandle h, std::uint32_t width, std::uint32_t height,
              PixelFormat format) noexcept
        : handle_(h), width_(width), height_(height), format_(format) {}

    // ---- Pool refcount -----------------------------------------------------

    /** Refcount. Decoders/encoders call `acquire_ref()` when reusing the
     *  buffer; the last to call `release_ref()` returns it to the pool. */
    void acquire_ref() noexcept {
        ref_count_.fetch_add(1, std::memory_order_acq_rel);
    }
    void release_ref() noexcept {
        if (ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            // Last ref — recycle to pool via the pool's `recycle(this)`.
            if (recycle_callback_) recycle_callback_(this);
        }
    }
    [[nodiscard]] std::uint32_t ref_count() const noexcept {
        return ref_count_.load(std::memory_order_acquire);
    }

    /** Install a pool-side callback fired when ref_count drops to zero.
     *  Called by GpuBufferPool::recycle_buffer() at construction. */
    using RecycleFn = void(*)(GpuBuffer*);
    void set_recycle_callback(RecycleFn fn) noexcept { recycle_callback_ = fn; }

    // ---- GPU handle --------------------------------------------------------

    [[nodiscard]] const GpuHandle& handle() const noexcept { return handle_; }
    void set_handle(GpuHandle h) noexcept { handle_ = h; }
    void clear_handle() noexcept { handle_ = {}; }

    /** True iff this buffer carries a live GPU handle (zero-copy path
     *  is available). */
    [[nodiscard]] bool has_gpu_handle() const noexcept override {
        return handle_.backend != GpuBackend::kNone && handle_.handle != nullptr;
    }

    // ---- Dimensions / format ----------------------------------------------

    [[nodiscard]] std::uint32_t width()  const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] PixelFormat   format() const noexcept { return format_; }

    void set_dimensions(std::uint32_t w, std::uint32_t h) noexcept {
        width_ = w; height_ = h;
    }
    void set_format(PixelFormat f) noexcept { format_ = f; }

    // ---- CPU mirror (optional, for software fallback / snapshot) ----------

    /** Set the CPU mirror — caller owns the memory until reset. */
    void set_cpu_mirror(std::uint8_t* y, std::uint8_t* u, std::uint8_t* v,
                        std::int32_t stride_y, std::int32_t stride_u,
                        std::int32_t stride_v) noexcept {
        cpu_y_ = y; cpu_u_ = u; cpu_v_ = v;
        stride_y_ = stride_y; stride_u_ = stride_u; stride_v_ = stride_v;
    }
    void clear_cpu_mirror() noexcept {
        cpu_y_ = cpu_u_ = cpu_v_ = nullptr;
        stride_y_ = stride_u_ = stride_v_ = 0;
    }
    [[nodiscard]] bool has_cpu_mirror() const noexcept {
        return cpu_y_ != nullptr;
    }
    [[nodiscard]] std::uint8_t* cpu_y() const noexcept { return cpu_y_; }
    [[nodiscard]] std::uint8_t* cpu_u() const noexcept { return cpu_u_; }
    [[nodiscard]] std::uint8_t* cpu_v() const noexcept { return cpu_v_; }
    [[nodiscard]] std::int32_t stride_y() const noexcept { return stride_y_; }
    [[nodiscard]] std::int32_t stride_u() const noexcept { return stride_u_; }
    [[nodiscard]] std::int32_t stride_v() const noexcept { return stride_v_; }

    // ---- Zero-copy statistics (for benchmarks / monitoring) ----------------

    /** Total bytes copied CPU↔GPU over this buffer's lifetime (0 if path
     *  was fully zero-copy). Set by HW backends when they fall back to
     *  CPU staging. */
    [[nodiscard]] std::uint64_t bytes_copied() const noexcept {
        return bytes_copied_.load(std::memory_order_acquire);
    }
    void add_bytes_copied(std::uint64_t n) noexcept {
        bytes_copied_.fetch_add(n, std::memory_order_acq_rel);
    }

private:
    GpuHandle handle_{};
    std::uint32_t width_  = 0;
    std::uint32_t height_ = 0;
    PixelFormat   format_ = PixelFormat::kUnknown;

    // CPU mirror (optional)
    std::uint8_t* cpu_y_ = nullptr;
    std::uint8_t* cpu_u_ = nullptr;
    std::uint8_t* cpu_v_ = nullptr;
    std::int32_t  stride_y_ = 0;
    std::int32_t  stride_u_ = 0;
    std::int32_t  stride_v_ = 0;

    // Refcount + recycle
    std::atomic<std::uint32_t> ref_count_{1};
    RecycleFn recycle_callback_ = nullptr;

    // Diagnostics
    std::atomic<std::uint64_t> bytes_copied_{0};
};

// ---------------------------------------------------------------------------
// GpuBufferPool — per-backend arena that owns the actual GPU resources
// ---------------------------------------------------------------------------
//
// Each HW backend (MediaCodec / VideoToolbox / D3D11 / VAAPI / CUDA / MMAL)
// provides its own pool implementation; this base class is the contract:
//
//   - `acquire(width, height, format)` returns a buffer ready for the
//     encoder/decoder to write into (or for a fresh capture frame).
//   - `recycle` is called by the buffer's last `release_ref()`.
//   - `preallocate(n)` ensures N buffers are GPU-allocated up-front so
//     the first frame doesn't stall on driver-side allocation.
//
// The base class is abstract — concrete plugins subclass with the platform
// SDK call (cuArrayCreate, AMediaCodec_createSurface, ID3D11Device_Create
// Texture2D, vaCreateSurfaces, MMAL_PORT_ENABLE, etc.).

class GpuBufferPool {
public:
    virtual ~GpuBufferPool() = default;

    /// Get the backend identifier this pool serves.
    [[nodiscard]] virtual GpuBackend backend() const noexcept = 0;

    /// Pre-allocate `count` buffers of the given layout. Returns the
    /// number actually allocated (may be < `count` if driver is OOM).
    [[nodiscard]] virtual std::uint32_t preallocate(
        std::uint32_t count, std::uint32_t width,
        std::uint32_t height, PixelFormat format) noexcept = 0;

    /// Acquire one buffer. If the pool is empty, the implementation may
    /// allocate a new one on the fly (acceptable but undesirable in
    /// steady state — call `preallocate` to avoid). Returns nullptr only
    /// if hardware is exhausted.
    [[nodiscard]] virtual GpuBuffer* acquire(
        std::uint32_t width, std::uint32_t height,
        PixelFormat format) noexcept = 0;

    /// Return a buffer to the pool (called by GpuBuffer::release_ref()).
    virtual void recycle(GpuBuffer* buf) noexcept = 0;

    /// Number of buffers currently available (idle) in the pool.
    [[nodiscard]] virtual std::uint32_t available() const noexcept = 0;

    /// Number of buffers currently checked out (ref_count > 0 minus idle).
    [[nodiscard]] virtual std::uint32_t in_flight() const noexcept = 0;

    /// Total number of buffer allocations (cumulative). Useful to detect
    /// pool leaks / sizing issues.
    [[nodiscard]] virtual std::uint64_t total_allocations() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// VideoFrameInfo — timeline / capture metadata (used by VideoFrame.info and
// EncodedVideoFrame.info — declared here, BEFORE VideoFrame, so VideoFrame's
// by-value `info` member has a complete type).
// ---------------------------------------------------------------------------

/** Capture-time and presentation metadata (per §8.4 timeline). */
struct VideoFrameInfo {
    /** Wall-clock capture timestamp (microseconds, monotonic clock). */
    std::int64_t  capture_ts_us   = 0;

    /** Per-SSRC frame sequence number (strictly increasing). */
    std::uint32_t frame_seq       = 0;

    /** RTP timestamp (32-bit, media clock). */
    std::uint32_t rtp_timestamp   = 0;

    /** How long the frame has been on screen (microseconds), maintained
     *  by the renderer. Set to 0 at decode time. */
    std::int64_t  presentation_age_us = 0;

    /** NTP↔RTP mapping at capture (mid-32 bits of NTP, from latest SR).
     *  Used to reconstruct absolute capture time on the receiver. */
    std::uint32_t ntp_mid_32      = 0;
};

// ---------------------------------------------------------------------------
// VideoFrame — unified frame reference (CPU + GPU + info)
// ---------------------------------------------------------------------------
//
// This replaces the old CPU-only `struct VideoFrame { plane_y/u/v ... }`.
// A VideoFrame is now a lightweight wrapper:
//   - `buffer`     — refcounted buffer (VideoFrameBuffer or GpuBuffer).
//   - `info`       — capture metadata.
//   - `gpu_handle` — convenience: shortcut to buffer's GPU handle when
//                    `buffer` is a GpuBuffer; otherwise null.
//
// `VideoFrame` is the canonical handle that flows through the entire
// pipeline. Each hop (encoder input, decoder output, SFU forward,
// display sink) reads either `gpu_handle` or `cpu_y/u/v` based on its
// capability, **never both** — the chosen path is fully zero-copy.
//
// `VideoFrameInfo` (capture / timeline metadata) is declared above this
// struct so the by-value `info` member has a complete type. Both
// `VideoFrame::info` and `EncodedVideoFrame::info` reuse the same type.

struct VideoFrame {
    /** Either a `VideoFrameBuffer` (CPU-only) or a `GpuBuffer` (GPU-first
     *  with optional CPU mirror). We use `std::shared_ptr<void>` so
     *  both shapes share one slot; concrete plugins cast based on the
     *  type-id attached to the buffer. The frame itself is a plain POD
     *  so copying is cheap. */
    std::shared_ptr<void> buffer;

    /** Capture metadata. */
    VideoFrameInfo info;

    /** True iff `buffer` carries a live GPU handle. Set during decode /
     *  capture — does NOT require the caller to inspect the buffer's
     *  dynamic type.
     *
     *  Implementation note: `buffer` is `shared_ptr<void>`. We probe the
     *  handle via `cpu_buffer()->has_gpu_handle()` — both GpuBuffer and
     *  VideoFrameBuffer expose this method (it's a no-op on the CPU side
     *  and returns true only when a real GPU handle is attached). When
     *  the caller's `buffer` is a `shared_ptr<GpuBuffer>` it still goes
     *  through `cpu_buffer()` because the underlying storage is a void*
     *  shared_ptr; the probe returns true exactly when the buffer is a
     *  GpuBuffer with a non-null handle. */
    [[nodiscard]] bool has_gpu() const noexcept {
        if (!buffer) return false;
        // Type-erased dispatch: `buffer` is shared_ptr<void>. Cast to the
        // common base first (VideoFrameBuffer), then dynamic_pointer_cast
        // to GpuBuffer. The latter returns null when the underlying object
        // is a CPU-only VideoFrameBuffer; in that case we report `false`.
        auto base = std::static_pointer_cast<VideoFrameBuffer>(buffer);
        auto gb   = std::dynamic_pointer_cast<GpuBuffer>(base);
        if (!gb) return false;
        return gb->has_gpu_handle();
    }

    /** Resolve to a GpuBuffer (returns null if buffer is CPU-only). */
    [[nodiscard]] std::shared_ptr<GpuBuffer> gpu_buffer() const noexcept {
        if (!buffer) return nullptr;
        auto base = std::static_pointer_cast<VideoFrameBuffer>(buffer);
        return std::dynamic_pointer_cast<GpuBuffer>(base);
    }

    /** Resolve to a VideoFrameBuffer (returns null only if `buffer` is
     *  null). For a GpuBuffer this returns the same control block typed
     *  as the base; the caller can downcast back to GpuBuffer via
     *  `gpu_buffer()` if needed. */
    [[nodiscard]] std::shared_ptr<VideoFrameBuffer> cpu_buffer() const noexcept {
        if (!buffer) return nullptr;
        return std::static_pointer_cast<VideoFrameBuffer>(buffer);
    }

    /// Width/height/format passthrough — convenience that probes the buffer.
    [[nodiscard]] std::uint32_t width() const noexcept {
        if (auto gb = gpu_buffer(); gb && gb.use_count()) return gb->width();
        if (auto cb = cpu_buffer(); cb && cb.use_count()) return cb->layout().width;
        return 0;
    }
    [[nodiscard]] std::uint32_t height() const noexcept {
        if (auto gb = gpu_buffer(); gb && gb.use_count()) return gb->height();
        if (auto cb = cpu_buffer(); cb && cb.use_count()) return cb->layout().height;
        return 0;
    }
    [[nodiscard]] PixelFormat format() const noexcept {
        if (auto gb = gpu_buffer(); gb && gb.use_count()) return gb->format();
        if (auto cb = cpu_buffer(); cb && cb.use_count()) return cb->layout().format;
        return PixelFormat::kUnknown;
    }
};

// ---------------------------------------------------------------------------
// EncodedVideoFrame — codec-agnostic compressed payload
// ---------------------------------------------------------------------------

enum class CodecKind : std::uint8_t {
    kUnknown = 0,
    kH264    = 1,
    kH265    = 2,
    kVP8     = 3,
    kVP9     = 4,
    kAV1     = 5,
};

inline std::string_view codec_name(CodecKind k) noexcept {
    switch (k) {
        case CodecKind::kH264:  return "H264";
        case CodecKind::kH265:  return "H265";
        case CodecKind::kVP8:   return "VP8";
        case CodecKind::kVP9:   return "VP9";
        case CodecKind::kAV1:   return "AV1";
        default:                return "Unknown";
    }
}

enum class NaluFormat : std::uint8_t {
    kUnknown         = 0,
    kAnnexBStartCode = 1,   ///< 3- or 4-byte 0x00 00 01 / 0x00 00 00 01 prefix
    kLengthPrefixed  = 2,   ///< 4-byte big-endian length prefix per NAL unit
};

/**
 * @brief Compressed (encoded) video frame.
 *
 * One EncodedVideoFrame corresponds to one decoded picture (one frame).
 * The `payload` is a single concatenated bitstream in the format selected
 * by `nalu_format`:
 *   - H.264 + kAnnexBStartCode: `00 00 00 01 [nalu] 00 00 00 01 [nalu] ...`
 *   - H.264 + kLengthPrefixed : `len32 | nalu | len32 | nalu | ...`
 *   - VP8/VP9/AV1: raw bitstream (no descriptor — descriptors live in the
 *     RTP payload framing, handled by the video_payload module)
 */
struct EncodedVideoFrame {
    CodecKind         codec         = CodecKind::kUnknown;
    NaluFormat        nalu_format   = NaluFormat::kUnknown;
    core::ByteSpan    payload;                 ///< codec bitstream
    bool              is_keyframe   = false;
    std::uint8_t      payload_type  = 0;
    VideoFrameInfo    info;
};

/** Convenience: total payload size in bytes. */
inline std::size_t encoded_size(const EncodedVideoFrame& f) noexcept {
    return f.payload.size();
}

// ---------------------------------------------------------------------------
// Pixel format conversion
// ---------------------------------------------------------------------------

/** Convert I420 (YYYY... UUU... VVV...) to BGRA. Output buffer must be at
 *  least width*height*4 bytes. Alpha channel is filled with 0xFF. */
void convert_i420_to_bgra(std::uint32_t width,
                          std::uint32_t height,
                          const std::uint8_t* y_plane, std::int32_t y_stride,
                          const std::uint8_t* u_plane, std::int32_t u_stride,
                          const std::uint8_t* v_plane, std::int32_t v_stride,
                          std::uint8_t* bgra_out,
                          std::int32_t bgra_stride) noexcept;

/** Convert NV12 (YYYY... UVUV...) to I420 (de-interleave UV). */
void convert_nv12_to_i420(std::uint32_t width,
                          std::uint32_t height,
                          const std::uint8_t* y_plane, std::int32_t y_stride,
                          const std::uint8_t* uv_plane, std::int32_t uv_stride,
                          std::uint8_t* y_out, std::int32_t y_out_stride,
                          std::uint8_t* u_out, std::int32_t u_out_stride,
                          std::uint8_t* v_out, std::int32_t v_out_stride) noexcept;

/** Convert I420 to NV12 (interleave UV). */
void convert_i420_to_nv12(std::uint32_t width,
                          std::uint32_t height,
                          const std::uint8_t* y_plane, std::int32_t y_stride,
                          const std::uint8_t* u_plane, std::int32_t u_stride,
                          const std::uint8_t* v_plane, std::int32_t v_stride,
                          std::uint8_t* y_out, std::int32_t y_out_stride,
                          std::uint8_t* uv_out, std::int32_t uv_out_stride) noexcept;

} // namespace nimrtc::video_frame
