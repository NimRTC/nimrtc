/**
 * @file nimrtc/vp9/codec_plugin.hpp
 * @brief Codec plugin adapter: wraps nimrtc::vp9::Decoder behind
 *        plugins::IVideoCodec.
 *
 * Implements the video codec plugin interface defined in
 * <nimrtc/plugins/video_codec.hpp> by delegating to a concrete
 * vp9::Decoder instance.
 *
 * ## Backend selection
 *
 * The factory follows the same hardware-first policy as the H.264 /
 * H.265 / AV1 modules: the backend table is consulted first, and the
 * software stub is the fallback.  There is no VP9 *hardware* row in
 * this tree (no hardware encoder in the tree exposes a VP9 entry
 * point), so the selection resolves to the vendored libvpx row when it
 * is compiled in, and to the stub otherwise.
 *
 * Registered as id `"vp9"` (matches the `EngineConfig::video_codec_name`
 * override that selects this codec).
 *
 * @note The stub codec's `encode()` emits a *synthetic* VP9-shaped
 *       bitstream, not a real VP9 picture.  A host that needs real
 *       pixels builds with `NIMRTC_VENDORED_LIBVPX=ON`.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string_view>

#include <nimrtc/core/plugin_id.hpp>

#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/vp9/decoder.hpp>

namespace nimrtc::vp9 {

// ---------------------------------------------------------------------------
// CodecPluginAdapter
// ---------------------------------------------------------------------------

/** Wraps a concrete vp9::Decoder behind plugins::IVideoCodec. */
class NIMRTC_API_STABLE CodecPluginAdapter : public plugins::IVideoCodec {
public:
    explicit CodecPluginAdapter(plugins::VideoCodecConfig cfg) noexcept;
    ~CodecPluginAdapter() override;

    CodecPluginAdapter(const CodecPluginAdapter&)            = delete;
    CodecPluginAdapter& operator=(const CodecPluginAdapter&) = delete;

    // ---- plugins::IPlugin --------------------------------------------------

    const char* name() const noexcept override;
    plugins::Status open() noexcept override;
    void          close() noexcept override;

    // ---- plugins::IVideoCodec ----------------------------------------------

    bool can_encode() const noexcept override { return true; }
    bool can_decode() const noexcept override { return true; }

    plugins::VideoCodecKind kind() const noexcept override {
        return plugins::VideoCodecKind::kVP9;
    }
    std::string_view codec_name() const noexcept override { return "vp9"; }

    plugins::Status encode(const plugins::VideoFrame& raw,
                           std::uint8_t* output,
                           std::size_t output_capacity,
                           plugins::EncodedVideoFrame& encoded_out) noexcept override;

    plugins::Status decode(const plugins::EncodedVideoFrame& encoded,
                           plugins::VideoFrame& raw_out,
                           std::uint8_t* const* output_buffers) noexcept override;

    plugins::Status force_keyframe() noexcept override { return plugins::kOk; }

    /** Fill an I420 framebuffer with a recognisable synthetic pattern
     *  derived from @p frame_seq.  Used by the stub decoder so round-
     *  tripped frames are visually identifiable on the wire / on screen. */
    static void paint_i420(std::uint8_t* y,
                           std::uint8_t* u,
                           std::uint8_t* v,
                           std::uint32_t width, std::uint32_t height,
                           std::uint32_t frame_seq,
                           std::uint32_t stride_y,
                           std::uint32_t stride_u,
                           std::uint32_t stride_v) noexcept;

    plugins::VideoCodecConfig config() const noexcept override { return cfg_; }
    plugins::Status update_config(plugins::VideoCodecConfig cfg) noexcept override;
    plugins::VideoCodecStats stats() const noexcept override;
    std::uint8_t payload_type() const noexcept override;

    // ---- Telemetry ---------------------------------------------------------

    /** The backend identity a host sees in `Stats::video_codec_backend`.
     *
     *  "software-stub" is the honest label for this adapter: it
     *  recognises the *shape* of a VP9 frame but reconstructs nothing
     *  — it paints a synthetic pattern.  The vendored libvpx backend
     *  reports "libvpx" instead, so the two are distinguishable.
     *
     *  @see plugins::IVideoCodec::backend_id() */
    std::string_view backend_id() const noexcept override { return "software-stub"; }

private:
    plugins::VideoCodecConfig cfg_{};
    std::unique_ptr<Decoder>  decoder_;
    plugins::VideoCodecStats  stats_{};
};

// ---------------------------------------------------------------------------
// Vp9PluginFactory
// ---------------------------------------------------------------------------

/** Factory producing CodecPluginAdapter instances.
 *  Registered as id "vp9".  A host that ships its own VP9 codec
 *  registers a different factory and selects it via
 *  `EngineConfig::video_codec_name`. */
class NIMRTC_API_STABLE Vp9PluginFactory : public plugins::IVideoCodecFactory {
public:
    std::string_view id() const noexcept override { return NIMRTC_PLUGIN_ID("vp9"); }
    std::string_view display_name() const noexcept {
        return "VP9 (RFC 9559) — libvpx software encoder when vendored, stub fallback";
    }
    plugins::IVideoCodec* create(plugins::VideoCodecConfig cfg) const override;
};

// ---------------------------------------------------------------------------
// Registration entry point
// ---------------------------------------------------------------------------

namespace detail {
void do_register_default_plugins() noexcept;
}

/** Register the built-in "vp9" codec plugin with core::PluginRegistry.
 *  Idempotent (uses a Meyers' singleton latch internally). */
void register_default_plugins() noexcept;

} // namespace nimrtc::vp9
