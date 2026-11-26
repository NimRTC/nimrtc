/**
 * @file nimrtc/plugins/video_codec_view.hpp
 * @brief VideoCodecView — a *non-owning* combined view over a split
 *        encoder/decoder pair.
 *
 * ## Why this exists
 *
 * The codec seam is split (remote-desktop plan B1): the engine holds an
 * `IVideoEncoder` and an `IVideoDecoder`, not one combined object.  The
 * *combined* `IVideoCodec` stays as a deprecated compatibility shim
 * (ADR-015 Decision 3), so a host that still speaks the old vocabulary
 * must keep working.
 *
 * `HwCompositeCodec` (the owning variant) answers that need when the
 * two halves are created *together* by a codec module's factory — which
 * is what the shipped H.264 / H.265 / AV1 / VP9 modules do.  It
 * **owns** its two halves and closes them in its destructor.
 *
 * The engine's situation is different: it creates the two halves
 * *separately* (from the two split registry slots) and drives them
 * independently.  Wrapping them in an owning composite would mean the
 * engine owns a wrapper that owns the halves, and the `video_codec()`
 * accessor would have to hand out a pointer into that wrapper — an
 * ownership web for no benefit.
 *
 * `VideoCodecView` is the **non-owning** counterpart: it holds *borrowed*
 * pointers and forwards every call.  The engine keeps the split
 * instances alive (it owns them) and keeps the view alongside them; the
 * view's lifetime is strictly shorter than theirs.
 *
 * ## The lifetime contract
 *
 * The view must not outlive the two halves it points at.  It does not
 * enforce that (it cannot — it does not own them), so the *owner* must
 * keep the view and the halves in the same scope and destroy the view
 * first.  A view whose halves are gone is a dangling pointer, exactly
 * like any other borrowed reference.
 *
 * ## The direction rules
 *
 * A view is built from *both* halves; a missing half (nullptr) means
 * "this direction is not served".  The forwarders answer `kErrUnsupported`
 * in that case rather than dereferencing a null pointer — the same
 * contract `HwCompositeCodec` follows.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include <nimrtc/core/api_version.hpp>
#include <nimrtc/plugins/video_codec.hpp>

namespace nimrtc::plugins {

// The view implements the *deprecated* combined shim on purpose: it is
// the thing that presents a split encoder/decoder pair to a consumer
// that still speaks the combined vocabulary.  The deprecation warning
// itself is untouched — it is suppressed only for the declaration and
// the definitions that have to name the shim.
NIMRTC_SUPPRESS_DEPRECATION_BEGIN

/**
 * @brief A borrowed, combined view over a split encoder/decoder pair.
 *
 * @see HwCompositeCodec for the owning variant.
 */
class NIMRTC_API_STABLE VideoCodecView final : public IVideoCodec {
public:
    /**
     * @param encoder  The encode half (may be null → no encode path).
     * @param decoder  The decode half (may be null → no decode path).
     * @param cfg      The configuration to report when neither half is
     *                 present.
     */
    VideoCodecView(IVideoEncoder* encoder,
                   IVideoDecoder* decoder,
                   VideoCodecConfig cfg = {}) noexcept;
    ~VideoCodecView() override;

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

    Status set_params(const VideoEncoderParams& params) noexcept override;

    bool supports_zero_copy_encode() const noexcept override;
    bool supports_zero_copy_decode() const noexcept override;

    Status encode_zero_copy(const GpuBuffer& gpu,
                            EncodedVideoFrame& encoded_out) noexcept override;

    Status decode_zero_copy(const EncodedVideoFrame& encoded,
                            GpuBuffer& out_surface) noexcept override;

    VideoCodecConfig config() const noexcept override;
    Status update_config(VideoCodecConfig cfg) noexcept override;
    VideoCodecStats stats() const noexcept override;
    std::uint8_t payload_type() const noexcept override;

    std::string_view backend_id() const noexcept override;
    bool is_hardware_accelerated() const noexcept override;

private:
    IVideoEncoder*             encoder_ = nullptr;  ///< borrowed
    IVideoDecoder*             decoder_ = nullptr;  ///< borrowed
    VideoCodecConfig           cfg_{};
};

NIMRTC_SUPPRESS_DEPRECATION_END

} // namespace nimrtc::plugins
