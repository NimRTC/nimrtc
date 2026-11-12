/**
 * @file nimrtc/nvcodec/hw_composite_codec.hpp
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

namespace nimrtc::nvcodec {

/**
 * @brief A bidirectional `plugins::IVideoCodec` built from two
 *        single-direction codecs.
 *
 * The composite takes ownership of both sub-codecs and closes them in
 * its destructor.  `open()` opens both; if the decoder fails to open,
 * the encoder is closed again so a half-open session cannot leak a
 * hardware session.
 */
class NIMRTC_API_STABLE HwCompositeCodec final : public plugins::IVideoCodec {
public:
    /**
     * @param cfg       Configuration handed to both sub-codecs.
     * @param encoder   Encode-only codec (may be null → no encode path).
     * @param decoder   Decode-only codec (may be null → no decode path).
     */
    HwCompositeCodec(plugins::VideoCodecConfig cfg,
                     std::unique_ptr<plugins::IVideoCodec> encoder,
                     std::unique_ptr<plugins::IVideoCodec> decoder) noexcept;

    ~HwCompositeCodec() override;

    // ---- plugins::IPlugin --------------------------------------------------

    const char* name() const noexcept override;

    plugins::Status open() noexcept override;
    void close() noexcept override;

    // ---- plugins::IVideoCodec ----------------------------------------------

    bool can_encode() const noexcept override;
    bool can_decode() const noexcept override;

    plugins::VideoCodecKind kind() const noexcept override;
    std::string_view codec_name() const noexcept override;

    plugins::Status encode(const plugins::VideoFrame& raw,
                            std::uint8_t* output,
                            std::size_t output_capacity,
                            plugins::EncodedVideoFrame& encoded_out) noexcept override;

    plugins::Status decode(const plugins::EncodedVideoFrame& encoded,
                            plugins::VideoFrame& raw_out,
                            std::uint8_t* const* output_buffers) noexcept override;

    plugins::Status force_keyframe() noexcept override;

    bool supports_zero_copy_encode() const noexcept override;
    bool supports_zero_copy_decode() const noexcept override;

    plugins::VideoCodecConfig config() const noexcept override;
    plugins::Status update_config(plugins::VideoCodecConfig cfg) noexcept override;
    plugins::VideoCodecStats stats() const noexcept override;
    std::uint8_t payload_type() const noexcept override;

private:
    plugins::VideoCodecConfig            cfg_;
    std::unique_ptr<plugins::IVideoCodec> encoder_;
    std::unique_ptr<plugins::IVideoCodec> decoder_;
    bool                                 opened_ = false;
};

} // namespace nimrtc::nvcodec

