/**
 * @file nimrtc/av1/codec_plugin.hpp
 * @brief Codec plugin adapter: wraps nimrtc::av1::Decoder behind
 *        plugins::IVideoCodec.
 *
 * Implements the video codec plugin interface defined in
 * <nimrtc/plugins/video_codec.hpp> by delegating to a concrete
 * av1::Decoder instance.
 *
 * ## Backend selection
 *
 * The factory follows the same hardware-first policy as the H.264 and
 * H.265 modules: the NVIDIA NVENC / NVDEC backends (owned by the shared
 * `nvcodec` module) are consulted first, and the software stub is the
 * fallback.  Because the hardware backends are one-directional (NVENC
 * encodes, NVDEC decodes) and the engine holds *one* codec instance per
 * session, the factory pairs the two into a `nvcodec::HwCompositeCodec`
 * when they differ.
 *
 * Registered as id `"av1"` (matches the `EngineConfig::video_codec_name`
 * override that selects this codec).
 *
 * @note The stub codec's `encode()` emits a *synthetic* AV1 Annex B OBU
 *       stream (temporal delimiter + sequence header + frame), not a real
 *       AV1 picture.  A host that needs real pixels replaces the backend
 *       with a hardware or software AV1 encoder registered under a
 *       different id.
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
#include <nimrtc/av1/decoder.hpp>

namespace nimrtc::av1 {

// ---------------------------------------------------------------------------
// CodecPluginAdapter
// ---------------------------------------------------------------------------

/** Wraps a concrete av1::Decoder behind plugins::IVideoCodec. */
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
        return plugins::VideoCodecKind::kAV1;
    }
    std::string_view codec_name() const noexcept override { return "av1"; }

    plugins::Status encode(const plugins::VideoFrame& raw,
                           std::uint8_t* output, std::size_t output_capacity,
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
     *  recognises the *shape* of an AV1 OBU stream but reconstructs
     *  nothing — it paints a synthetic pattern.  A host that needs real
     *  pixels must be able to tell the two apart, so this id is
     *  deliberately distinct from the real software backend ("dav1d") and
     *  from the hardware ones.
     *
     *  @see plugins::IVideoCodec::backend_id() */
    std::string_view backend_id() const noexcept override { return "software-stub"; }

private:
    plugins::VideoCodecConfig cfg_{};
    std::unique_ptr<Decoder>  decoder_;
    plugins::VideoCodecStats  stats_{};
};

// ---------------------------------------------------------------------------
// Av1PluginFactory
// ---------------------------------------------------------------------------

/** Factory producing CodecPluginAdapter instances.
 *  Registered as id "av1".  A host that ships its own AV1 codec
 *  registers a different factory and selects it via
 *  `EngineConfig::video_codec_name`. */
class NIMRTC_API_STABLE Av1PluginFactory : public plugins::IVideoCodecFactory {
public:
    std::string_view id() const noexcept override { return NIMRTC_PLUGIN_ID("av1"); }
    std::string_view display_name() const noexcept override {
        return "AV1 (draft-ietf-payload-av1) — NVIDIA NVENC/NVDEC first, stub fallback";
    }
    plugins::IVideoCodec* create(plugins::VideoCodecConfig cfg) const override;
};

// ---------------------------------------------------------------------------
// Registration entry point
// ---------------------------------------------------------------------------

namespace detail {
void do_register_default_plugins() noexcept;
}

/** Register the built-in "av1" codec plugin with core::PluginRegistry.
 *  Idempotent (uses a Meyers' singleton latch internally). */
void register_default_plugins() noexcept;

} // namespace nimrtc::av1
