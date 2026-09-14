/**
 * @file nimrtc/plugins/video_codec.hpp
 * @brief IVideoCodec — pluggable video encoder/decoder interface.
 *
 * Per ARCHITECTURE.md §2.3 + §1.4 + §6: NimRTC's core does NOT bundle any
 * video encoder or decoder (H.264 / VP8 / VP9 / AV1 / H.265 all have patent
 * or licence concerns, see §11.3). The shipped `nimrtc_h264` library is one
 * possible plugin implementation (a stub decoder behind this interface,
 * registered under id `"h264"`); users can register their own codec by:
 *
 * @code
 * class MyH264 : public plugins::IVideoCodec { ... };
 * class MyH264Factory : public plugins::IVideoCodecFactory { ... };
 * nimrtc::core::PluginRegistry::instance().register_video_codec(
 *     "my_h264", &factory);
 * @endcode
 *
 * and selecting it via `EngineConfig::video_codec_name = "my_h264"`.
 *
 * ## Codec kind
 *
 * `IVideoCodec::kind()` distinguishes encoder-only, decoder-only, or both.
 * Native RTC stacks usually need both (encode outbound frames, decode inbound
 * frames); some specialised gateways are send-only or receive-only.
 *
 * ## Frame I/O
 *
 * Two operations on raw frames:
 *   - `encode(raw, encoded_out)`     — raw VideoFrame → compressed EncodedVideoFrame
 *   - `decode(encoded, raw_out)`     — compressed EncodedVideoFrame → raw VideoFrame
 *
 * `EncodedVideoFrame` is codec-agnostic (NAL units for H.264, raw bitstream
 * for VP8/VP9/AV1); the codec implementation does the format conversion.
 *
 * ## Type identity vs the concrete h264::Decoder
 *
 * The concrete h264 module exposes a single `Decoder` class; IVideoCodec
 * exposes both encode and decode paths in one factory. If a user needs
 * asymmetric encoding/decoding (e.g. send-only), they can construct two
 * separate `IVideoCodec` instances (one per direction).
 *
 * @note P1 R2-Batch1 candidate. Interface is stable; binary layout TBD P3.
 */

// base.hpp must be before include guard — see transport.hpp for rationale.
#include "nimrtc/plugins/base.hpp"

#ifndef NIMRTC_PLUGINS_VIDEO_CODEC_HPP
#define NIMRTC_PLUGINS_VIDEO_CODEC_HPP

#include <cstdint>
#include <cstddef>
#include <string_view>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/video_frame/frame.hpp>  // VideoFrame / EncodedVideoFrame — single source of truth

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// Pixel format / codec kind — re-exported from video_frame so plugin code
// can stay in the `nimrtc::plugins` namespace.
// ---------------------------------------------------------------------------

using VideoPixelFormat = video_frame::PixelFormat;
using VideoCodecKind   = video_frame::CodecKind;
using NaluFormat       = video_frame::NaluFormat;
using GpuBackend       = video_frame::GpuBackend;
using GpuHandle        = video_frame::GpuHandle;
using GpuBuffer        = video_frame::GpuBuffer;
using GpuBufferPool    = video_frame::GpuBufferPool;

inline std::string_view pixel_format_name(VideoPixelFormat f) noexcept {
    return video_frame::pixel_format_name(f);
}
inline std::string_view video_codec_name(VideoCodecKind k) noexcept {
    return video_frame::codec_name(k);
}
inline std::string_view gpu_backend_name(GpuBackend b) noexcept {
    return video_frame::gpu_backend_name(b);
}

/** Per-pixel-format compatibility shims — video_frame uses PixelFormat,
 *  but old plugin code may still reference VideoPixelFormat (same values). */

// ---------------------------------------------------------------------------
// VideoFrame — unified frame reference (CPU + GPU + info)
// ---------------------------------------------------------------------------
//
// Canonical definition lives in video_frame::VideoFrame (with GpuBuffer,
// GpuHandle, etc.). `plugins::VideoFrame` is a typedef so all plugin code
// can keep using `plugins::VideoFrame`.
//
// Plugins SHOULD now access GPU zero-copy via VideoFrame::has_gpu() and
// VideoFrame::gpu_buffer(); CPU fallback via VideoFrame::cpu_buffer().
//
// The old CPU-only fields (plane_y/u/v, stride_y/u/v) are gone — use the
// GpuBuffer's `cpu_mirror` plane pointers instead. See codec_plugin.cpp's
// StubEncoder below for a migration example.

using VideoFrame       = video_frame::VideoFrame;
using EncodedVideoFrame = video_frame::EncodedVideoFrame;

/** Video codec configuration (passed at open() time). Mirrors the
 *  concrete module's `EncoderConfig` / `DecoderConfig`. */
struct VideoCodecConfig {
    /** Target width in pixels. */
    std::uint32_t width = 0;

    /** Target height in pixels. */
    std::uint32_t height = 0;

    /** Target frame rate in frames per second. */
    std::uint32_t fps = 30;

    /** Target bitrate in bits per second (0 = codec default). */
    std::uint32_t bitrate_bps = 0;

    /** Keyframe interval (max frames between IDR frames). */
    std::uint32_t keyframe_interval = 60;

    /** Pixel format codec accepts as input / produces as output. */
    VideoPixelFormat pixel_format = VideoPixelFormat::kI420;

    /** Codec family this config applies to. */
    VideoCodecKind codec = VideoCodecKind::kUnknown;

    /** RTP payload type (e.g. 102 for H264, 96 for VP8 in WebRTC). */
    std::uint8_t payload_type = 0;

    /** Human-readable codec name (e.g. "h264"). */
    std::string_view name;
};

// ---------------------------------------------------------------------------
// Codec statistics
// ---------------------------------------------------------------------------

struct VideoCodecStats {
    /** Total frames successfully encoded. */
    std::uint64_t frames_encoded = 0;
    /** Total bytes of encoded payload output. */
    std::uint64_t bytes_encoded = 0;
    /** Total frames successfully decoded. */
    std::uint64_t frames_decoded = 0;
    /** Total bytes of encoded payload consumed by decoder. */
    std::uint64_t bytes_decoded = 0;
    /** Encode errors. */
    std::uint64_t encode_errors = 0;
    /** Decode errors (malformed packet, codec error). */
    std::uint64_t decode_errors = 0;
    /** Total frames skipped by the encoder (e.g. rate-limiting). */
    std::uint64_t frames_skipped = 0;
};

// ---------------------------------------------------------------------------
// IVideoCodec
// ---------------------------------------------------------------------------

/**
 * @brief Encoder + decoder for one video codec implementation.
 *
 * One `IVideoCodec` instance owns the encoder state and decoder state for
 * one peer-connection / one stream. Thread-safety: NOT thread-safe; caller
 * must serialize encode() and decode() calls (engine does this).
 */
class IVideoCodec : public IPlugin {
public:
    // ---- Direction flags (one instance may support both) -------------------

    /** Whether this instance supports encoding (raw → compressed). */
    virtual bool can_encode() const noexcept = 0;

    /** Whether this instance supports decoding (compressed → raw). */
    virtual bool can_decode() const noexcept = 0;

    // ---- Codec metadata ----------------------------------------------------

    /** Codec family this implementation handles. */
    virtual VideoCodecKind kind() const noexcept = 0;

    /** Human-readable codec name (e.g. "h264", "vp8"). */
    virtual std::string_view codec_name() const noexcept = 0;

    // ---- Encode / decode ---------------------------------------------------

    /**
     * @brief Encode one raw frame to compressed bitstream.
     *
     * @param raw            Raw frame (must remain valid for the call).
     * @param output         Caller-owned buffer for compressed output.
     * @param output_capacity Capacity of @p output in bytes.
     * @param encoded_out    On success, populated with the encoded frame.
     * @return kOk on success; error code on failure.
     */
    virtual Status encode(const VideoFrame& raw,
                          std::uint8_t* output,
                          std::size_t   output_capacity,
                          EncodedVideoFrame& encoded_out) noexcept = 0;

    /**
     * @brief Decode one compressed frame to raw pixels.
     *
     * @param encoded       Compressed payload (codec-specific format).
     * @param raw_out       Caller-owned raw frame descriptor. The codec
     *                      writes into @p output_buffers (must remain valid).
     * @param output_buffers Pre-allocated pixel buffers. For I420 this is
     *                       {plane_y, plane_u, plane_v}; for NV12 it's
     *                       {plane_y, plane_uv, nullptr}.
     * @return kOk on success; error code on failure.
     */
    virtual Status decode(const EncodedVideoFrame& encoded,
                          VideoFrame& raw_out,
                          std::uint8_t* const* output_buffers) noexcept = 0;

    /** Force the encoder to emit a keyframe on the next encode() call.
     *  Used by PLI / FIR feedback handlers. No-op for decoder-only
     *  implementations. */
    virtual Status force_keyframe() noexcept = 0;

    // ---- Zero-copy (HW surface) path --------------------------------------
    //
    // Per ARCHITECTURE.md §6.4 the zero-copy path is:
    //
    //   capture_src ──► GpuBuffer ──► encode_zero_copy ──► bitstream
    //   bitstream   ──► decode_zero_copy ──► GpuBuffer ──► display_sink
    //
    // Default implementations of the methods below operate in CPU-staging
    // mode (read from `gpu_y/u/v` mirrors or call the regular encode/decode
    // with a CPU-backed VideoFrame). Concrete HW codecs override them with
    // implementations that pass the GPU handle directly to the HW encoder/
    // decoder API without staging to RAM.
    //
    // When the backend's `zero_copy_supported = true`, the engine uses
    // these entry points instead of the CPU `encode`/`decode`. When
    // false (or for software codecs), the engine stays on the CPU path.
    //
    // **Lifetime contract**:
    //   - For `encode_zero_copy`, the caller (capture pipeline) keeps the
    //     `GpuBuffer*` alive (ref-counted) until the encoder returns.
    //   - For `decode_zero_copy`, the callee (decoder) acquires one ref
    //     on `out_surface` (decoded picture ready to be consumed) and
    //     releases it when the next decode is submitted / the codec is
    //     destroyed. Caller must therefore acquire an additional ref
    //     before holding onto the surface past the call.

    /// True if this codec instance can accept a GPU handle directly. When
    /// false, the engine must stage CPU pixels via `encode()` instead.
    virtual bool supports_zero_copy_encode() const noexcept { return false; }

    /// True if this codec instance can output a GPU surface. When false,
    /// the engine must call `decode()` and consume `cpu_y/u/v` mirrors.
    virtual bool supports_zero_copy_decode() const noexcept { return false; }

    /** Encode one GPU-handle frame. Caller MUST keep `gpu` alive until
     *  this returns. Default implementation returns kErrUnsupported. */
    virtual Status encode_zero_copy(const GpuBuffer& gpu,
                                    EncodedVideoFrame& encoded_out) noexcept {
        (void)gpu; (void)encoded_out;
        return kErrUnsupported;
    }

    /** Decode one bitstream frame into a GPU surface. `out_surface` must
     *  come from `pool_for_encode().acquire(w,h,format)` and is filled
     *  by the decoder. Default implementation returns kErrUnsupported. */
    virtual Status decode_zero_copy(const EncodedVideoFrame& encoded,
                                    GpuBuffer& out_surface) noexcept {
        (void)encoded; (void)out_surface;
        return kErrUnsupported;
    }

    /** Pool used by this codec for zero-copy decode output. May be null
     *  for codecs that don't support zero-copy. The engine uses this pool
     *  to pre-allocate output surfaces in the steady state. */
    virtual std::shared_ptr<GpuBufferPool> output_pool() const noexcept {
        return nullptr;
    }

    /** Pool used by this codec to acquire zero-copy input surfaces
     *  (e.g. for encoder input pre-allocation). May be null. */
    virtual std::shared_ptr<GpuBufferPool> input_pool() const noexcept {
        return nullptr;
    }

    // ---- Configuration / stats --------------------------------------------

    /** Get current configuration (for diagnostics / logs). */
    virtual VideoCodecConfig config() const noexcept = 0;

    /** Update configuration live (e.g. adaptive bitrate change). */
    virtual Status update_config(VideoCodecConfig cfg) noexcept = 0;

    /** Snapshot of codec statistics. */
    virtual VideoCodecStats stats() const noexcept = 0;

    /** RTP payload type this codec emits. */
    virtual std::uint8_t payload_type() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

class IVideoCodecFactory {
public:
    virtual ~IVideoCodecFactory() = default;
    virtual std::string_view id()           const noexcept = 0;
    virtual std::string_view display_name() const noexcept = 0;

    /**
     * @brief Create a video codec instance with the given configuration.
     *
     * The returned IVideoCodec is in kConstructed state; caller must call
     * `open()` before encode()/decode(). Engine's open() does this.
     */
    virtual IVideoCodec* create(VideoCodecConfig cfg) const = 0;
};

template<class T>
class SimpleVideoCodecFactory : public IVideoCodecFactory {
    std::string_view id_;
    std::string_view name_;
public:
    explicit SimpleVideoCodecFactory(std::string_view id,
                                     std::string_view name) noexcept
        : id_(id), name_(name) {}

    std::string_view id()           const noexcept override { return id_; }
    std::string_view display_name() const noexcept override { return name_; }

    IVideoCodec* create(VideoCodecConfig cfg) const override {
        return new T(std::move(cfg));
    }
};

} // namespace nimrtc::plugins
#endif // NIMRTC_PLUGINS_VIDEO_CODEC_HPP
