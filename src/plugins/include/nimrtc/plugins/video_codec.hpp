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
#include <nimrtc/core/api_version.hpp>
#include "nimrtc/plugins/base.hpp"

#ifndef NIMRTC_PLUGINS_VIDEO_CODEC_HPP
#define NIMRTC_PLUGINS_VIDEO_CODEC_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.

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

using NIMRTC_API_STABLE VideoPixelFormat = video_frame::PixelFormat;
using NIMRTC_API_STABLE VideoCodecKind   = video_frame::CodecKind;
using NIMRTC_API_STABLE NaluFormat       = video_frame::NaluFormat;
using NIMRTC_API_STABLE GpuBackend       = video_frame::GpuBackend;
using NIMRTC_API_STABLE GpuHandle        = video_frame::GpuHandle;
using NIMRTC_API_STABLE GpuBuffer        = video_frame::GpuBuffer;
using NIMRTC_API_STABLE GpuBufferPool    = video_frame::GpuBufferPool;

inline NIMRTC_API_STABLE std::string_view pixel_format_name(VideoPixelFormat f) noexcept {
    return video_frame::pixel_format_name(f);
}
inline NIMRTC_API_STABLE std::string_view video_codec_name(VideoCodecKind k) noexcept {
    return video_frame::codec_name(k);
}
inline NIMRTC_API_STABLE std::string_view gpu_backend_name(GpuBackend b) noexcept {
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

using NIMRTC_API_STABLE VideoFrame       = video_frame::VideoFrame;
using NIMRTC_API_STABLE EncodedVideoFrame = video_frame::EncodedVideoFrame;

// ---------------------------------------------------------------------------
// Codec backend selection (remote-desktop plan B2)
// ---------------------------------------------------------------------------

/**
 * @brief Which class of codec backend the engine should use.
 *
 * ## Why this is a separate knob from `video_codec_name`
 *
 * `video_codec_name` names a *codec* ("h264", "vp9", ...).  This names
 * a *class of implementation* for that codec.  The two are orthogonal:
 * "H.264" can be encoded by NVENC, by VA-API, by VideoToolbox or by
 * OpenH264, and a host may want to pin the choice.
 *
 * ## The three values
 *
 * - `kAuto` — pick the best available backend: hardware first, then
 *   software.  This is the default, and it is what the plan's
 *   "hardware-first" claim means.  The choice is reported in the stats,
 *   so a host can see which backend was actually used.
 * - `kHardware` — **only** hardware backends are eligible.  When none
 *   is available the selection **fails** rather than silently falling
 *   back to software: a host that asked for hardware and got software
 *   has a 1080p30 frame budget it cannot meet, and the failure must be
 *   loud.
 * - `kSoftware` — **only** software backends are eligible.  This is the
 *   explicit override, and it is what the plan's acceptance criterion
 *   ("an explicit `software` override really does use software") needs.
 *
 * ## The reporting rule
 *
 * Whatever the selection, the engine reports the outcome:
 * `Stats::video_codec_backend` names the backend and
 * `Stats::video_codec_hardware_accelerated` says whether it is hardware.
 * A machine with no hardware encoder running in `kAuto` mode therefore
 * "still runs (software path) and *says so* in the stats".
 */
enum class NIMRTC_API_STABLE VideoCodecBackendPreference : std::uint8_t {
    kAuto = 0,      ///< hardware first, software as the fallback
    kHardware = 1,  ///< hardware only — fail rather than fall back
    kSoftware = 2,  ///< software only — the explicit override
};

/**
 * @brief Parse a backend preference from its configuration string.
 *
 * Accepts "auto" (the default), "hardware" and "software", case
 * insensitively.  An empty string means "auto".
 *
 * @return The preference, and whether the string was understood.
 */
inline NIMRTC_API_STABLE bool parse_video_codec_backend_preference(
        std::string_view text, VideoCodecBackendPreference& out) noexcept {
    if (text.empty() || text == "auto") {
        out = VideoCodecBackendPreference::kAuto;
        return true;
    }
    if (text == "hardware" || text == "hw") {
        out = VideoCodecBackendPreference::kHardware;
        return true;
    }
    if (text == "software" || text == "sw") {
        out = VideoCodecBackendPreference::kSoftware;
        return true;
    }
    return false;
}

/**
 * @brief The configuration-string spelling of a backend preference.
 */
inline NIMRTC_API_STABLE std::string_view video_codec_backend_preference_name(
        VideoCodecBackendPreference pref) noexcept {
    switch (pref) {
        case VideoCodecBackendPreference::kHardware: return "hardware";
        case VideoCodecBackendPreference::kSoftware: return "software";
        case VideoCodecBackendPreference::kAuto:     break;
    }
    return "auto";
}

// ---------------------------------------------------------------------------
// Video codec configuration
// ---------------------------------------------------------------------------

/** Video codec configuration (passed at open() time). Mirrors the
 *  concrete module's `EncoderConfig` / `DecoderConfig`. */
struct NIMRTC_API_STABLE VideoCodecConfig {
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

    /**
     * @brief Which class of backend to use (remote-desktop plan B2).
     *
     * The engine fills this from `EngineConfig::video_codec_backend` and
     * the module's backend selector honours it.  `kAuto` (the default)
     * is "hardware first, software as the fallback"; `kHardware` and
     * `kSoftware` restrict the selection to one class and *fail* rather
     * than fall back.
     */
    VideoCodecBackendPreference backend_preference =
        VideoCodecBackendPreference::kAuto;
};

// ---------------------------------------------------------------------------
// Colour description (remote-desktop plan C5)
// ---------------------------------------------------------------------------

/** The range of the luma and chroma values (ITU-T H.273 "video_full_range"
 *  / "video_range").
 *
 *  This is not a cosmetic detail: getting it wrong produces a picture
 *  that looks washed out or crushed, and the usual reaction is "the
 *  codec is broken".  A desktop compositor delivers **full range**
 *  (the whole 0..255 code range carries signal); a camera in the
 *  default BT.601 studio setup delivers **limited range** (16..235).
 *  Conflating the two shifts the picture by a visible amount. */
enum class NIMRTC_API_STABLE VideoColorRange : std::uint8_t {
    kUnknown = 0,   ///< not stated — the consumer picks the default
    kLimited = 1,   ///< 16..235 luma, 16..240 chroma (studio / camera)
    kFull    = 2,   ///< 0..255 luma and chroma (desktop / screen content)
};

/** The matrix that maps RGB to YUV (ITU-T H.273 matrix_coefficients). */
enum class NIMRTC_API_STABLE VideoColorMatrix : std::uint8_t {
    kUnknown = 0,
    kBt601   = 1,   ///< BT.601 — the classic camera matrix
    kBt709   = 2,   ///< BT.709 — HD / screen content
    kBt2020  = 3,   ///< BT.2020 non-constant luminance — UHD
};

/** The colour description of a video stream.
 *
 *  The description travels with the picture from the capture device, to
 *  the encoder, to the wire, and to the renderer.  The point is that
 *  the **encoder and the renderer agree**: if the encoder codes a
 *  full-range picture and the renderer interprets it as limited, the
 *  contrast is wrong in a way that is very hard to diagnose from the
 *  symptoms.
 *
 *  `kUnknown` is the honest default and means "nobody said". */
struct NIMRTC_API_STABLE VideoColorDescription {
    /** The luma / chroma range. */
    VideoColorRange range = VideoColorRange::kUnknown;

    /** The RGB↔YUV matrix. */
    VideoColorMatrix matrix = VideoColorMatrix::kUnknown;

    /** True iff anything was stated at all. */
    [[nodiscard]] bool known() const noexcept {
        return range != VideoColorRange::kUnknown ||
               matrix != VideoColorMatrix::kUnknown;
    }

    /** True iff the two descriptions agree on everything that was
     *  stated on both sides.  Two `kUnknown` halves compare equal. */
    [[nodiscard]] bool agrees_with(const VideoColorDescription& other) const noexcept {
        if (range != VideoColorRange::kUnknown &&
            other.range != VideoColorRange::kUnknown &&
            range != other.range) {
            return false;
        }
        if (matrix != VideoColorMatrix::kUnknown &&
            other.matrix != VideoColorMatrix::kUnknown &&
            matrix != other.matrix) {
            return false;
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// Encoder parameter set (remote-desktop plan B12)
// ---------------------------------------------------------------------------

/** The *one* parameter set the engine computes and pushes to the encoder.
 *
 *  ## Why this struct exists
 *
 *  The encoder is a **passive** component: it does not know about the
 *  bandwidth estimator, the rate controller, the feedback stream, or the
 *  resolution ladder.  Everything it needs to know about the stream it
 *  is coding arrives through this one struct, pushed by the engine
 *  through `IVideoCodec::set_params()`.
 *
 *  Before this struct the encoder learned its parameters implicitly, at
 *  `open()` time, through `VideoCodecConfig` — and then the only live
 *  update path was `update_config()`, which took a whole *config* and
 *  was used for one thing only (the bitrate).  That left three gaps:
 *
 *    - a resolution change could not be expressed (the geometry lives in
 *      `VideoCodecConfig`, but the encoder was never told to re-init);
 *    - a quality target (QP) had no home;
 *    - the content hint (screen vs camera) had no home.
 *
 *  ## The contract
 *
 *  The engine computes the whole set in **one** place and pushes it.  A
 *  codec never derives a parameter from a neighbouring field, and never
 *  asks a rate controller for one.  The struct is therefore the complete
 *  description of "what the encoder should be producing right now".
 */
struct NIMRTC_API_STABLE VideoEncoderParams {
    /** Target width in pixels.  A change means a parameter-set change
     *  (B13): the encoder must restart and emit the new SPS / PPS (or
     *  VPS / AV1 sequence header) with the next frame. */
    std::uint32_t width = 0;

    /** Target height in pixels.  See `width`. */
    std::uint32_t height = 0;

    /** Target frame rate in frames per second. */
    std::uint32_t fps = 30;

    /** Target bitrate in bits per second (0 = the codec's own default). */
    std::uint32_t bitrate_bps = 0;

    /** Maximum number of frames between two IDR frames.
     *
     *  A keyframe *request* from the peer overrides this for one frame
     *  (`IVideoCodec::force_keyframe()`); this is the steady-state
     *  interval. */
    std::uint32_t keyframe_interval = 60;

    /** Quality target (quantiser).  0 = best quality, 51 = worst for
     *  H.264; the exact range is codec-family specific.  255 means
     *  "unset" — the codec uses its own rate-control default. */
    std::uint8_t qp = 255;

    /** The pixel format the encoder expects as input. */
    VideoPixelFormat pixel_format = VideoPixelFormat::kI420;

    /** The content hint — screen content and camera content want
     *  different coding tools (remote-desktop plan B7 / RD-3).
     *
     *  `kUnspecified` means "the engine has no opinion", which is the
     *  correct value for a camera stream. */
    enum class ContentHint : std::uint8_t {
        kUnspecified = 0,  ///< no opinion (camera content)
        kMotion     = 1,  ///< camera-like: spend the budget on motion
        kDetail     = 2,  ///< screen content: preserve fine detail / text
        kText       = 3,  ///< screen content dominated by text
    };

    /** The content hint for this stream. */
    ContentHint content_hint = ContentHint::kUnspecified;

    /** The colour description of the stream (remote-desktop plan C5).
     *
     *  The encoder honours it: a full-range picture must not be coded
     *  as if it were limited, and the matrix must match the one the
     *  pixels were produced with.  `known() == false` means "nobody
     *  said", which is the correct value when the source does not
     *  know. */
    VideoColorDescription color;

    /** True iff `qp` carries a meaningful value (i.e. != 255). */
    [[nodiscard]] bool has_qp() const noexcept { return qp != 255; }

    /** True iff the geometry is set (non-zero width and height). */
    [[nodiscard]] bool has_geometry() const noexcept {
        return width != 0 && height != 0;
    }
};

// ---------------------------------------------------------------------------
// Codec statistics
// ---------------------------------------------------------------------------

struct NIMRTC_API_STABLE VideoCodecStats {
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
    /** Frames encoded through the GPU zero-copy path (no CPU staging).
     *
     *  The counterpart of the engine's own `video_zero_copy_frames`
     *  counter: this one is the codec's own statement that it consumed
     *  a native handle (remote-desktop plan B15). */
    std::uint64_t frames_encoded_zero_copy = 0;
    /** Frames encoded through the CPU staging path. */
    std::uint64_t frames_encoded_staged = 0;
    /** Keyframes produced (IDR frames). */
    std::uint64_t keyframes_encoded = 0;
    /** Bytes copied CPU↔GPU inside the codec (0 on a pure hardware path). */
    std::uint64_t bytes_copied = 0;
    /** The sum of the per-frame encode durations, in microseconds.
     *
     *  The engine divides it by `frames_encoded` to get the mean encode
     *  latency; the codec owns the measurement because only it knows
     *  where the encode actually starts and ends. */
    std::uint64_t encode_latency_us_total = 0;
    /** The largest single encode duration seen, in microseconds. */
    std::uint64_t encode_latency_us_max = 0;
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
class NIMRTC_API_STABLE IVideoCodec : public IPlugin {
public:
    // ---- Direction flags (one instance may support both) -------------------

    /** Whether this instance supports encoding (raw → compressed). */
    virtual bool can_encode() const noexcept = 0;

    /** Whether this instance supports decoding (compressed → raw). */
    virtual bool can_decode() const noexcept = 0;

    // ---- Codec metadata ----------------------------------------------------

    /** Codec family this implementation handles.
     *
     *  This is the **authoritative** statement of the wire format.  The
     *  engine derives the SDP `a=rtpmap:` encoding name, the RTP payload
     *  type, and the packetizer / depacketizer selection from this
     *  value — NOT from the plugin id.
     *
     *  Contract:
     *   - A plugin that implements exactly one family MUST return that
     *     family.  `VideoCodecKind::kUnknown` is reserved for a codec
     *     that genuinely does not know (a composite / future-proofing
     *     implementation).
     *   - A composite codec (e.g. NVENC + NVDEC) reports the family of
     *     the sub-codec it wraps.
     *   - The engine falls back to parsing the plugin id only when this
     *     returns `kUnknown`; when *both* are unknown the engine fails
     *     the session with `kEngineInvalidParam` rather than
     *     negotiating a codec family it cannot describe.
     *
     *  @see VideoCodecKind */
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

    /** Push the encoder's parameter set (remote-desktop plan B12).
     *
     *  This is the **single** entry point through which the engine
     *  describes the stream the encoder is supposed to be producing.
     *  The encoder is passive: it has no access to the bandwidth
     *  estimator, to the rate controller, or to the feedback stream —
     *  it only ever learns what it needs from this call.
     *
     *  The default implementation forwards the fields that the legacy
     *  `VideoCodecConfig` already carried (geometry, fps, bitrate,
     *  keyframe interval, pixel format) to `update_config()`, so a
     *  third-party codec that only implements `update_config()` keeps
     *  working unchanged.  A codec that wants the extra knobs (QP,
     *  content hint) or wants to handle a geometry change itself
     *  overrides this.
     *
     *  ## The geometry contract (B13)
     *
     *  When `params.width` / `params.height` differ from the codec's
     *  current geometry, the parameter set changed, and the *next*
     *  encoded frame must carry the new parameter set (SPS / PPS, or
     *  VPS, or the AV1 sequence header) so the peer can decode it
     *  without a keyframe request.  The default implementation cannot
     *  promise that (it does not know the codec's internals), so a
     *  codec that can restart must override this method.
     *
     *  @param params  The complete parameter set for the stream.
     *  @return kOk on success; an error code when the codec rejects the
     *          parameters (e.g. a geometry the codec cannot handle). */
    virtual Status set_params(const VideoEncoderParams& params) noexcept {
        VideoCodecConfig cfg = config();
        if (params.has_geometry()) {
            cfg.width  = params.width;
            cfg.height = params.height;
        }
        if (params.fps != 0)            cfg.fps = params.fps;
        if (params.bitrate_bps != 0)    cfg.bitrate_bps = params.bitrate_bps;
        if (params.keyframe_interval != 0) {
            cfg.keyframe_interval = params.keyframe_interval;
        }
        cfg.pixel_format = params.pixel_format;
        return update_config(cfg);
    }

    /** The encoder's current parameter set (remote-desktop plan B12).
     *
     *  The default implementation derives it from `config()`, so a codec
     *  that does not override `set_params()` still reports something
     *  coherent.  A codec that overrides `set_params()` should override
     *  this too, so the reported parameters match what was actually
     *  applied. */
    virtual VideoEncoderParams params() const noexcept {
        const VideoCodecConfig cfg = config();
        VideoEncoderParams p;
        p.width            = cfg.width;
        p.height           = cfg.height;
        p.fps              = cfg.fps;
        p.bitrate_bps      = cfg.bitrate_bps;
        p.keyframe_interval = cfg.keyframe_interval;
        p.pixel_format     = cfg.pixel_format;
        return p;
    }

    /** Snapshot of codec statistics. */
    virtual VideoCodecStats stats() const noexcept = 0;

    /** RTP payload type this codec emits. */
    virtual std::uint8_t payload_type() const noexcept = 0;

    // ---- Codec telemetry (remote-desktop plan B15) ------------------------
    //
    // The engine reports the codec's identity in its stats snapshot: which
    // backend was selected, whether it runs on the CPU or on the GPU, and
    // whether the frames actually took the zero-copy path.  The codec is
    // the only party that knows the answer, so the engine asks.

    /** The identifier of the backend this codec runs on.
     *
     *  A short, stable string: "openh264", "nvenc", "dav1d",
     *  "stub_h264", ...  The engine reports it verbatim in
     *  `Stats::video_codec_backend`, so a host can tell which encoder
     *  produced the bitstream it is receiving.
     *
     *  Default = "unknown": a codec that does not name its backend. */
    virtual std::string_view backend_id() const noexcept { return "unknown"; }

    /** True iff this codec runs on a hardware accelerator.
     *
     *  Default = false (software).  A hardware codec overrides this to
     *  true — *when it is actually open*, because a hardware backend
     *  that failed to initialise falls back to software. */
    virtual bool is_hardware_accelerated() const noexcept { return false; }
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

class NIMRTC_API_STABLE IVideoCodecFactory {
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
class NIMRTC_API_STABLE SimpleVideoCodecFactory : public IVideoCodecFactory {
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
