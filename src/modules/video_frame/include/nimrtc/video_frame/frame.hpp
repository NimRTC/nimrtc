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
    /** Construct an empty buffer (zero width / height). */
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

    ~VideoFrameBuffer() {
        // Decrement our application-level ref count when this buffer goes
        // out of scope. The shared_ptr<FrameBufferControl> has its own
        // ref count that handles actual deallocation; this counter is for
        // diagnostics and tests that want to observe shared ownership.
        if (ctrl_) {
            ctrl_->ref_count.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

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

    /** Drop our ref — caller can keep using their own shared_ptr. */
    void reset() noexcept { ctrl_.reset(); storage_.clear(); layout_ = {}; }

private:
    VideoFrameLayout layout_{};
    std::vector<std::uint8_t> storage_;             ///< owned when not wrapping external
    std::shared_ptr<FrameBufferControl> ctrl_;       ///< refcount + metadata
};

// ---------------------------------------------------------------------------
// VideoFrameInfo — timeline / capture metadata
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
