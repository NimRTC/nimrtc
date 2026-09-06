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

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// Video codec configuration (mirrors concrete modules when present)
// ---------------------------------------------------------------------------

/** Pixel format of raw VideoFrame data. Codecs report which formats they
 *  accept / produce via IVideoCodec::supported_pixel_format(). */
enum class VideoPixelFormat : std::uint8_t {
    kUnknown = 0,
    kI420    = 1,   ///< YUV 4:2:0 planar (YYYY... UUU... VVV...)
    kNV12    = 2,   ///< YUV 4:2:0 semi-planar (YYYY... UVUVUV...)
    kYV12    = 3,   ///< YUV 4:2:0 planar (YYYY... VVV... UUU...)
    kBGRA    = 4,   ///< 32-bit BGRA (used by some HW decoders)
    kRGBA    = 5,   ///< 32-bit RGBA
};

/** Human-readable pixel format name (for logging). */
inline std::string_view pixel_format_name(VideoPixelFormat f) noexcept {
    switch (f) {
        case VideoPixelFormat::kI420:  return "I420";
        case VideoPixelFormat::kNV12:  return "NV12";
        case VideoPixelFormat::kYV12:  return "YV12";
        case VideoPixelFormat::kBGRA:  return "BGRA";
        case VideoPixelFormat::kRGBA:  return "RGBA";
        default:                       return "Unknown";
    }
}

/** Logical codec family (independent of bitstream framing). */
enum class VideoCodecKind : std::uint8_t {
    kUnknown = 0,
    kH264    = 1,
    kH265    = 2,
    kVP8     = 3,
    kVP9     = 4,
    kAV1     = 5,
};

inline std::string_view video_codec_name(VideoCodecKind k) noexcept {
    switch (k) {
        case VideoCodecKind::kH264:  return "H264";
        case VideoCodecKind::kH265:  return "H265";
        case VideoCodecKind::kVP8:   return "VP8";
        case VideoCodecKind::kVP9:   return "VP9";
        case VideoCodecKind::kAV1:   return "AV1";
        default:                     return "Unknown";
    }
}

/** Raw (uncompressed) video frame. The codec reads `pixels` according to
 *  `format`; the buffer must remain valid for the duration of `encode()`. */
struct VideoFrame {
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    VideoPixelFormat format = VideoPixelFormat::kUnknown;

    /** Stride (bytes per row) for plane 0 (Y / R). Must be >= width. */
    std::int32_t stride_y = 0;

    /** Stride for plane 1 (U / V / UV). 0 if not applicable (e.g. BGRA). */
    std::int32_t stride_u = 0;

    /** Stride for plane 2 (V / second chroma). 0 if not applicable. */
    std::int32_t stride_v = 0;

    /** Pointer to plane 0 bytes (Y or BGRA). Owned by caller. */
    const std::uint8_t* plane_y = nullptr;

    /** Pointer to plane 1 bytes (U for I420, UV for NV12). nullptr if N/A. */
    const std::uint8_t* plane_u = nullptr;

    /** Pointer to plane 2 bytes (V for I420). nullptr if N/A. */
    const std::uint8_t* plane_v = nullptr;

    /** Capture timestamp (microseconds, monotonic clock). Required for
     *  timeline alignment per §8.4. */
    std::int64_t capture_ts_us = 0;

    /** Per-stream frame sequence number (strictly increasing per SSRC).
     *  Encoders attach it to the output EncodedVideoFrame. */
    std::uint32_t frame_seq = 0;

    /** Rotation in degrees (0, 90, 180, 270). Some codecs ignore this. */
    std::uint16_t rotation_deg = 0;

    /** Wall-clock NTP↔RTP mapping at capture time (mid-32 bits of NTP). */
    std::uint32_t rtp_timestamp = 0;
};

/** Compressed (encoded) video frame. For H.264 the payload is the
 *  concatenated NAL unit bitstream in Annex B format (length-prefixed or
 *  start-code-prefixed, depending on `nalu_format`). For VP8/VP9/AV1 the
 *  payload is the raw bitstream (no descriptor — descriptors are part of
 *  the RTP payload framing, which lives in the video_payload module). */
struct EncodedVideoFrame {
    VideoCodecKind codec = VideoCodecKind::kUnknown;

    /** Concatenated encoded bitstream. Owned by caller. */
    core::ByteSpan payload;

    /** RTP payload type (e.g. 96 for dynamic, 102 for H264 in WebRTC). */
    std::uint8_t payload_type = 0;

    /** Whether this is a key frame (IDR for H.264, keyframe for VP8/VP9). */
    bool is_keyframe = false;

    /** NAL unit format inside `payload` (only meaningful for H.264). */
    enum class NaluFormat : std::uint8_t {
        kUnknown = 0,
        kAnnexB  = 1,   ///< start codes (0x00000001) between NAL units
        kLengthPrefixed = 2, ///< 4-byte big-endian length prefix per NAL unit
    };
    NaluFormat nalu_format = NaluFormat::kUnknown;

    /** Per-frame timeline metadata (capture_ts, frame_seq, rtp_timestamp).
     *  Mirrors the same fields in video_frame::VideoFrameInfo; we duplicate
     *  the struct here so plugins/video_codec.hpp doesn't drag the
     *  video_frame module into every codec plugin. */
    struct Info {
        std::int64_t  capture_ts_us   = 0;
        std::uint32_t frame_seq       = 0;
        std::uint32_t rtp_timestamp   = 0;
    } info;
};

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
