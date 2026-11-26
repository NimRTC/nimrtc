/**
 * @file nimrtc/plugins/hw_composite_codec.hpp
 * @brief HwCompositeCodec — pairs an encode-only and a decode-only codec.
 *
 * ## Why this exists
 *
 * A hardware video session almost always needs *both* directions:
 * the local camera is encoded and the remote picture is decoded.  But
 * the hardware backends are naturally one-directional:
 *
 *   - `nvenc_h264`  → NVENC: encode only (`can_decode() == false`)
 *   - `nvdec_h264`  → NVDEC: decode only (`can_encode() == false`)
 *   - `stub_h264`   → the software fallback: both directions
 *
 * The engine holds a *single* `plugins::IVideoCodec` per session and
 * calls `encode()` for the outbound stream and `decode()` for the
 * inbound one.  A bare NVENC instance therefore cannot serve the
 * receive path, and a bare NVDEC instance cannot serve the send path.
 *
 * `HwCompositeCodec` is the adapter that makes the two fit the
 * single-instance contract: it *owns* an encoder instance and a decoder
 * instance and forwards each call to the one that can serve it.
 *
 * ## Why it lives in the seam layer (`src/plugins/`)
 *
 * The composite is **glue**: it composes two split-seam interfaces and
 * implements nothing itself.  Its home therefore is the layer that
 * *defines* the interfaces, not a concrete codec module — several codec
 * modules (H.264, H.265, AV1, VP9) need it, and a module that needed
 * it would have to link a *sibling* module, which is exactly the
 * coupling `src/modules/<name>/` being self-contained forbids (L3).
 *
 * The engine may include it too: `src/plugins/` is the seam, and the
 * layering rule (L4) exempts the seam itself.
 *
 * ## Codec awareness
 *
 * The composite is used by the H.264, H.265 and AV1 codec modules, so
 * it reports the codec of the sub-codecs it wraps rather than a
 * hard-coded "h264".  The codec kind and name are taken from the
 * encoder when there is one, and from the decoder otherwise; both
 * sub-codecs of a composite always carry the same codec, because the
 * backend registry selects them per codec kind.
 *
 * ## Statistics
 *
 * The composite aggregates the two sub-codecs' statistics:
 *   - `frames_encoded` / `bytes_encoded` come from the encoder,
 *   - `frames_decoded` / `bytes_decoded` come from the decoder,
 *   - the error counters are summed.
 *
 * ## Configuration
 *
 * `update_config()` forwards to the encoder.  The decoder is *not*
 * reconfigured with the encoder's frame size: a decoder's width/height
 * are an upper bound on the picture it may receive (the real geometry
 * comes from the bitstream's parameter sets), so shrinking that bound
 * when the *encoder* changes resolution would break the receive path.
 * The decoder's own bound is therefore left untouched.
 *
 * ## Selection
 *
 * The codec plugin factories build a composite only when the selected
 * encoder and the selected decoder are genuinely different objects.
 * When a single backend already serves both directions (the software
 * stub), that instance is returned directly — no composite, no extra
 * indirection.
 */

#pragma once

#include <memory>
#include <string_view>

#include <nimrtc/core/api_version.hpp>
#include <nimrtc/plugins/video_codec.hpp>

namespace nimrtc::plugins {

// The composite implements the *deprecated* combined shim on purpose: it
// is the thing that presents a split encoder/decoder pair to a consumer
// that still speaks the combined vocabulary.  The deprecation warning
// itself is untouched — it is suppressed only for the declaration and
// the definitions that have to name the shim.
NIMRTC_SUPPRESS_DEPRECATION_BEGIN

/**
 * @brief A bidirectional `plugins::IVideoCodec` built from two
 *        single-direction codecs.
 *
 * The composite takes ownership of both sub-codecs and closes them in
 * its destructor.  `open()` opens both; if the decoder fails to open,
 * the encoder is closed again so a half-open session cannot leak a
 * hardware session.
 *
 * ## The sub-codec types
 *
 * The two halves are held as `IVideoEncoder` and `IVideoDecoder` -- the
 * split seam -- rather than as the combined `IVideoCodec`.  The
 * *direction* of each half is then in its type: the composite cannot
 * accidentally call `decode()` on the encoder or `encode()` on the
 * decoder, and a backend that only implements one direction is a legal
 * argument here.
 *
 * The constructor still accepts a `std::unique_ptr<IVideoCodec>` from
 * an outside caller: `IVideoCodec` derives from both halves, so the
 * conversion to `unique_ptr<IVideoEncoder>` (resp. `unique_ptr<IVideoDecoder>`)
 * is an ordinary upcast and the existing call sites keep compiling.
 */
class NIMRTC_API_STABLE HwCompositeCodec final : public IVideoCodec {
public:
    /**
     * @param cfg       Configuration handed to both sub-codecs.
     * @param encoder   Encode-only codec (may be null → no encode path).
     * @param decoder   Decode-only codec (may be null → no decode path).
     */
    HwCompositeCodec(VideoCodecConfig cfg,
                     std::unique_ptr<IVideoEncoder> encoder,
                     std::unique_ptr<IVideoDecoder> decoder) noexcept;
    ~HwCompositeCodec() override;

    // ---- plugins::IPlugin --------------------------------------------------

    const char* name() const noexcept override;

    Status open() noexcept override;
    void close() noexcept override;

    // ---- plugins::IVideoCodec ----------------------------------------------

    bool can_encode() const noexcept override;
    bool can_decode() const noexcept override;

    VideoCodecKind kind() const noexcept override;
    std::string_view codec_name() const noexcept override;

    Status encode(const VideoFrame& raw,
                  std::uint8_t* output,
                  std::size_t output_capacity,
                  EncodedVideoFrame& encoded_out) noexcept override;

    Status decode(const EncodedVideoFrame& encoded,
                  VideoFrame& raw_out,
                  std::uint8_t* const* output_buffers) noexcept override;

    Status force_keyframe() noexcept override;

    /** Push a live parameter set into the encoder (remote-desktop plan
     *  B12).  The decoder half is *not* reconfigured: its geometry is
     *  an upper bound set by the bitstream, not by the local capture. */
    Status set_params(const VideoEncoderParams& params) noexcept override {
        if (!encoder_) return kErrUnsupported;
        return encoder_->set_params(params);
    }

    bool supports_zero_copy_encode() const noexcept override;
    bool supports_zero_copy_decode() const noexcept override;

    /**
     * @brief Encode a GPU surface — forwarded to the *encoder*.
     *
     * The composite forwards the zero-copy encode exactly the way it
     * forwards the ordinary `encode()`: to the sub-codec that can serve
     * it, which is the encoder.
     *
     * Without this override the composite would inherit the interface's
     * default — `kErrUnsupported` — while its `supports_zero_copy_encode()`
     * answered *true* (it forwards that to the encoder too).  That is
     * the worst of both worlds: the engine would believe the seam was
     * live, wire the capture device's GPU callback, hand the surface to
     * the codec, and be told "unsupported" for every single frame.  The
     * frames would be dropped (the capture device does not keep a CPU
     * mirror once the callback is attached) and the session would show
     * a picture that never moves.
     */
    Status encode_zero_copy(const GpuBuffer& gpu,
                            EncodedVideoFrame& encoded_out) noexcept override;

    /**
     * @brief Decode into a GPU surface — forwarded to the *decoder*.
     *
     * The mirror image of `encode_zero_copy()` above, for the same
     * reason: a composite whose decoder can consume a surface has to
     * say so *and* be able to do it.
     */
    Status decode_zero_copy(const EncodedVideoFrame& encoded,
                            GpuBuffer& out_surface) noexcept override;

    VideoCodecConfig config() const noexcept override;
    Status update_config(VideoCodecConfig cfg) noexcept override;
    VideoCodecStats stats() const noexcept override;
    std::uint8_t payload_type() const noexcept override;

    /** The backend identity of the sub-codec that carries the *outbound*
     *  stream — the encoder, when there is one.
     *
     *  The engine reports this verbatim in `Stats::video_codec_backend`,
     *  and those stats describe the encode path (frames encoded,
     *  keyframes, encode latency), so the encoder is the sub-codec that
     *  has to be named.  A receive-only composite falls back to the
     *  decoder's id.
     *
     *  Without this override the composite would report the interface's
     *  default "unknown", and a host could no longer tell a real
     *  software codec ("openh264") from the synthetic stub
     *  ("software-stub"). */
    std::string_view backend_id() const noexcept override;

    /**
     * @brief True iff the *encoder* runs on a hardware accelerator.
     *
     * The composite forwards the question to the encoder, because the
     * engine's telemetry describes the *outbound* stream: a composite
     * whose encoder is NVENC and whose decoder is OpenH264 is a
     * hardware-accelerated *encode* path, and the budget the remote
     * desktop plan's §B.6 writes is a send-side budget.
     *
     * Without this override the composite would inherit the interface's
     * default `false`, and a machine that really was encoding on the
     * GPU would be reported as software — which is exactly the claim
     * the capture/encode benchmark exists to check.
     */
    bool is_hardware_accelerated() const noexcept override;

private:
    VideoCodecConfig                  cfg_;
    std::unique_ptr<IVideoEncoder>    encoder_;
    std::unique_ptr<IVideoDecoder>    decoder_;
    bool                              opened_ = false;
};

NIMRTC_SUPPRESS_DEPRECATION_END

} // namespace nimrtc::plugins
