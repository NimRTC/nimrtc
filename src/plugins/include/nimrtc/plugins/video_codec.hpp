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
// Backend selection policy (remote-desktop plan B2 / RD-1)
// ---------------------------------------------------------------------------

/**
 * @brief Which class of encoder the host insists on.
 *
 * Hardware-first is the *default* (RD-1): the engine picks a hardware
 * encoder whenever one exists and supports the requested configuration,
 * and the software path is the fallback.  The override exists for the
 * two cases the default cannot serve:
 *
 *   - a host that **must** have hardware (a fanless embedded box whose
 *     CPU budget does not allow a software encode) and therefore needs
 *     to be told *at startup* when the demand cannot be met, rather
 *     than discovering it as a CPU saturation weeks later;
 *   - a **test** that needs a reproducible encoder, and therefore must
 *     not depend on which GPU happens to be in the machine.
 *
 * The policy travels with `VideoCodecConfig` because the *selection*
 * happens inside the codec module (which owns the backend table), and
 * the engine is the one that knows the host's intent.
 */
enum class NIMRTC_API_STABLE CodecBackend : std::uint8_t {
    /** Hardware when available, software otherwise.  The default. */
    kAuto     = 0,
    /** Hardware only.  Selection *fails* when no hardware encoder for
     *  the requested codec is available — it never silently falls back
     *  to software. */
    kHardware = 1,
    /** Software only.  Selection never picks a hardware encoder. */
    kSoftware = 2,
};

/** Human-readable backend-policy name (for logs and config parsing). */
inline NIMRTC_API_STABLE std::string_view codec_backend_name(
        CodecBackend p) noexcept {
    switch (p) {
        case CodecBackend::kHardware: return "hardware";
        case CodecBackend::kSoftware: return "software";
        default:                     return "auto";
    }
}

/** Parse a backend policy from its configuration spelling
 *  ("auto" / "hardware" / "software").  Returns false for an unknown
 *  spelling — the caller reports the bad value rather than guessing. */
inline NIMRTC_API_STABLE bool codec_backend_from_string(
        std::string_view s, CodecBackend& out) noexcept {
    if (s == "auto")     { out = CodecBackend::kAuto;     return true; }
    if (s == "hardware") { out = CodecBackend::kHardware; return true; }
    if (s == "software") { out = CodecBackend::kSoftware; return true; }
    return false;
}

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
// Content hint (remote-desktop plan B7 / RD-3)
// ---------------------------------------------------------------------------

/**
 * @brief What kind of picture the encoder is looking at.
 *
 * A screen capture is *not* a camera pointed at a monitor (RD-3): it
 * has flat regions, sharp edges, text and very little motion, and a
 * camera-style encoder setting (high QP on detail, motion estimation
 * everywhere, a fixed quantiser) spends most of its bitrate on noise
 * the viewer does not care about.
 *
 * The hint is what lets the encoder switch to screen-content coding:
 * intra block copy, a palette / text mode, a higher quantiser on flat
 * regions, and a lower motion search depth.  It is also what the SDP
 * carries (`a=content-hint:` media-level attribute, plus the
 * `x-google-content-hint` fmtp parameter for H.264), so the *sender*
 * can tune the encoder and the *receiver* can know what to expect.
 *
 * The values mirror the WebRTC `RTCContentHint` vocabulary so a
 * WebRTC-compatible peer understands the SDP attribute unchanged.
 */
enum class NIMRTC_API_STABLE ContentHint : std::uint8_t {
    /** No hint — the encoder uses its default (camera-style) tuning. */
    kUnspecified = 0,
    /** The picture has motion; favour temporal prediction. */
    kMotion      = 1,
    /** The picture has fine detail (a camera, a document scan). */
    kDetail      = 2,
    /** The picture is text (a terminal, an editor, a spreadsheet). */
    kText        = 3,
};

/** Human-readable content-hint name (for logs and SDP). */
inline NIMRTC_API_STABLE std::string_view content_hint_name(ContentHint h) noexcept {
    switch (h) {
        case ContentHint::kMotion: return "motion";
        case ContentHint::kDetail: return "detail";
        case ContentHint::kText:   return "text";
        default:                   return "";
    }
}

/** The `a=content-hint:` SDP attribute value for a hint.
 *  Empty for `kUnspecified` — the attribute is then omitted entirely. */
inline NIMRTC_API_STABLE std::string_view content_hint_sdp_value(
        ContentHint h) noexcept {
    return content_hint_name(h);
}

// ---------------------------------------------------------------------------
// Colour description (remote-desktop plan C5)
// ---------------------------------------------------------------------------
//
// The canonical definitions live in the `video_frame` module (the single
// source of truth — the frame is what carries the description), and are
// re-exported here so plugin code can keep using `plugins::ColorRange`
// without pulling the frame module in directly.

using NIMRTC_API_STABLE ColorRange  = video_frame::ColorRange;
using NIMRTC_API_STABLE ColorMatrix = video_frame::ColorMatrix;
using NIMRTC_API_STABLE ColorDescription = video_frame::ColorDescription;

/** Human-readable colour-range name (for logs and SDP). */
inline NIMRTC_API_STABLE std::string_view color_range_name(
        ColorRange r) noexcept {
    return video_frame::color_range_name(r);
}

/** Human-readable colour-matrix name (for logs and SDP). */
inline NIMRTC_API_STABLE std::string_view color_matrix_name(
        ColorMatrix m) noexcept {
    return video_frame::color_matrix_name(m);
}

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

    // ---- Remote-desktop additions (plan B2 / B7 / C5) --------------------
    //
    // These ride along with the open-time configuration so the codec
    // module can honour them while it builds the backend: the backend
    // *selection* needs the policy (B2), and the encoder's tuning needs
    // the content hint (B7) and the colour description (C5).

    /** Which class of encoder the host insists on (B2). */
    CodecBackend backend_policy = CodecBackend::kAuto;

    /** What kind of picture the encoder is looking at (B7). */
    ContentHint content_hint = ContentHint::kUnspecified;

    /** The luma/chroma range of the picture (C5). */
    ColorRange color_range = ColorRange::kUnspecified;

    /** The YUV↔RGB matrix of the picture (C5). */
    ColorMatrix color_matrix = ColorMatrix::kUnspecified;
};

// ---------------------------------------------------------------------------
// VideoEncoderParams — the single parameter set (remote-desktop plan B12)
// ---------------------------------------------------------------------------

/**
 * @brief The encoder's live parameter set — computed in one place.
 *
 * The encoder is **passive**: it has no access to the bandwidth
 * estimator, to the rate controller, or to the RTCP feedback stream.
 * Everything it needs arrives through this one struct, pushed by the
 * engine through `IVideoCodec::set_params()`.  That is what makes the
 * encoder testable in isolation and what keeps the rate-control loop
 * in one place (the engine) instead of smeared across every codec
 * plug-in.
 *
 * This is *additive* on top of `VideoCodecConfig`: the config is the
 * open-time negotiation (geometry, codec family, payload type), and
 * the params are the things that change while the session runs
 * (bitrate, QP, content hint, colour description, region of interest).
 */
struct NIMRTC_API_STABLE VideoEncoderParams {
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

    /** Constant quantiser (0 = let the codec's rate control decide).
     *
     *  A screen encoder wants a *ceiling* on the quantiser rather than
     *  a fixed value: flat regions should stay flat (a high QP is fine
     *  there) while text edges must stay sharp (a low QP).  The codec
     *  maps this onto its own quantiser-range control when it has one.
     */
    std::uint32_t qp = 0;

    /** What kind of picture this is (B7). */
    ContentHint content_hint = ContentHint::kUnspecified;

    /** The luma/chroma range of the picture (C5). */
    ColorRange color_range = ColorRange::kUnspecified;

    /** The YUV↔RGB matrix of the picture (C5). */
    ColorMatrix color_matrix = ColorMatrix::kUnspecified;

    /** A region of interest, in picture pixels.
     *
     *  The encoder spends its bitrate where the viewer is looking.  A
     *  remote-desktop session usually follows the pointer, so the ROI
     *  is the rectangle around the last user interaction — and the
     *  encoder raises the quality there while letting the rest of the
     *  screen coast on a flat, cheap encoding.
     *
     *  An empty region (width or height 0) means "no ROI": the whole
     *  picture is treated uniformly. */
    struct Region {
        std::uint32_t x = 0;
        std::uint32_t y = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;

        [[nodiscard]] bool empty() const noexcept {
            return width == 0 || height == 0;
        }
    } roi{};
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

    // ---- Telemetry (remote-desktop plan B15) -------------------------------
    //
    // The numbers that make a live session *explainable*: which backend
    // is actually running, whether the frames took the zero-copy path
    // or were staged through the CPU, what the encoder is actually
    // producing, and how long it takes.

    /** The backend identifier that produced this codec ("nvenc", "h264",
     *  "qsv", ...).  Empty when the codec does not know. */
    std::string backend_id;

    /** True iff this codec instance runs on a hardware encoder. */
    bool hardware = false;

    /** True iff the *last* frame took the zero-copy path (the GPU handle
     *  went straight into the encoder).  False means the frame was
     *  staged through the CPU. */
    bool zero_copy = false;

    /** The bitrate the encoder was *asked* for, in bits per second. */
    std::uint32_t configured_bitrate_bps = 0;

    /** The bitrate the encoder actually produced, in bits per second,
     *  averaged over the session.  0 until enough frames have been
     *  encoded to make the average meaningful. */
    std::uint32_t actual_bitrate_bps = 0;

    /** The average encode latency, in microseconds. */
    std::uint32_t avg_encode_latency_us = 0;

    /** The number of keyframes the encoder has produced. */
    std::uint64_t keyframes_encoded = 0;

    /** The number of frames that took the zero-copy path. */
    std::uint64_t zero_copy_frames = 0;

    /** The number of frames that had to be staged through the CPU. */
    std::uint64_t staged_frames = 0;

    /** How many frames the encoder had to skip because its input queue
     *  was full (the encoder is behind the capture). */
    std::uint64_t queue_drops = 0;
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

    /**
     * @brief Push the encoder's live parameter set (plan B12).
     *
     * The single entry point through which the engine tells the encoder
     * what to do: geometry, rate, quantiser, content hint, colour
     * description and region of interest.  The encoder is *passive* —
     * it never asks the bandwidth estimator anything, and the engine
     * never pokes at codec internals.
     *
     * The default implementation folds the parameters back into
     * `update_config()` so that a codec which only understands the
     * open-time config still honours the bitrate / GOP / geometry
     * changes.  A codec with real support for the extra knobs
     * (content hint, QP, ROI) overrides this.
     *
     * @return kOk when the parameters were accepted.
     */
    virtual Status set_params(const VideoEncoderParams& params) noexcept {
        // The default path maps the parameter set onto the config the
        // codec already understands.  The fields that have no config
        // counterpart (qp, content_hint, colour description, ROI) are
        // silently dropped — a codec that cares overrides this.
        VideoCodecConfig cfg = config();
        if (params.width  != 0) cfg.width  = params.width;
        if (params.height != 0) cfg.height = params.height;
        if (params.fps    != 0) cfg.fps    = params.fps;
        cfg.bitrate_bps       = params.bitrate_bps;
        cfg.keyframe_interval = params.keyframe_interval;
        return update_config(cfg);
    }

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
