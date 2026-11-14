/**
 * @file src/modules/vp9/src/hw_backends.cpp
 * @brief Default VP9 backend registry.
 *
 * The VP9 backend table is deliberately *small*: it carries the optional
 * vendored libvpx software encoder (remote-desktop plan B11) and the
 * synthetic stub that this module implements.
 *
 * ## Why there is no hardware VP9 row
 *
 * None of the hardware encoders this tree knows about (NVENC, AMF,
 * VA-API, Quick Sync, VideoToolbox, MediaCodec) exposes a VP9
 * *encoder* entry point.  A table row for a "VP9 hardware backend"
 * that no silicon implements would be dishonest — the plan's rule is
 * that a backend that exists in the table but not in the silicon
 * reports `available() == false` *with a reason*, and inventing rows
 * for hardware that does not exist would mean inventing the reasons
 * too.  So the table carries what is real: a software row and a stub.
 *
 * ## The B11 row
 *
 * The `libvpx_vp9` row is registered **only** when the vendored libvpx
 * build is compiled in (`NIMRTC_PLUGINS_LIBVPX_ON`).  With the option
 * OFF the plug-in is *absent* from the registry — that is the plan's
 * B11 acceptance ("with the option OFF the build still configures and
 * the plug-in is absent"), not "the plug-in is present but broken".
 */

#include <nimrtc/vp9/hw_backends.hpp>

#include <string>

#include <nimrtc/core/log.hpp>
#include <nimrtc/vp9/codec_plugin.hpp>
#include <nimrtc/plugins/video_codec.hpp>

namespace nimrtc::vp9 {

// ---------------------------------------------------------------------------
// The backend entry points exported by the libvpx source file.
//
// The source file compiles the *seam* unconditionally: the entry points
// exist with the right signatures on every platform, and the probe
// inside them reports the truth (false, with a reason) when the
// vendored build is absent.
// ---------------------------------------------------------------------------

extern bool libvpx_vp9_available() noexcept;
extern std::string libvpx_vp9_unavailable_reason();
extern std::unique_ptr<plugins::IVideoCodec>
make_libvpx_vp9_codec(plugins::VideoCodecConfig cfg);

namespace {

// ---------------------------------------------------------------------------
// Stub backend — always available, CPU only.
//
// The stub adapter is decode-first, but its encode() path also
// synthesises a VP9-shaped frame, so the engine loop works end-to-end
// without a codec.
// ---------------------------------------------------------------------------

plugins::VideoEncoderBackend make_stub_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "stub_vp9";
    be.description    = "Stub VP9 encoder (test / fallback, CPU only)";
    be.backend        = plugins::HwBackend::Software;
    be.priority       = 0;
    be.codec_kind     = plugins::VideoCodecKind::kVP9;
    be.is_hw          = false;
    be.zero_copy_supported = false;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return true;
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return std::make_unique<CodecPluginAdapter>(std::move(cfg));
    };
    return be;
}

plugins::VideoDecoderBackend make_stub_decoder_backend() {
    plugins::VideoDecoderBackend be;
    be.id             = "stub_vp9";
    be.description    = "Stub VP9 decoder (test / fallback, CPU only)";
    be.backend        = plugins::HwBackend::Software;
    be.priority       = 0;
    be.codec_kind     = plugins::VideoCodecKind::kVP9;
    be.is_hw          = false;
    be.zero_copy_supported = false;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return true;
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return std::make_unique<CodecPluginAdapter>(std::move(cfg));
    };
    return be;
}

#if defined(NIMRTC_PLUGINS_LIBVPX_ON)
// ---------------------------------------------------------------------------
// libvpx — the vendored software VP9 encoder. Priority 50.
//
// The row exists only when the vendored build is compiled in, so the
// registry carries either "libvpx is here" or "libvpx is not here" —
// never a row that cannot work.
//
// libvpx is a software codec, so `is_hw` is false and
// `zero_copy_supported` is false: it consumes CPU planes and has no
// GPU surface path.
// ---------------------------------------------------------------------------

plugins::VideoEncoderBackend make_libvpx_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "libvpx_vp9";
    be.description    = "libvpx software VP9 encoder (screen-content tuning)";
    be.backend        = plugins::HwBackend::Software;
    be.priority       = 50;
    be.codec_kind     = plugins::VideoCodecKind::kVP9;
    be.is_hw          = false;
    be.zero_copy_supported = false;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return libvpx_vp9_available();
    };
    be.unavailable_reason = [](const plugins::VideoCodecConfig&) -> std::string {
        return libvpx_vp9_unavailable_reason();
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return make_libvpx_vp9_codec(std::move(cfg));
    };
    return be;
}
#endif  // NIMRTC_PLUGINS_LIBVPX_ON

} // namespace

// ---------------------------------------------------------------------------
// One-shot registration.
// ---------------------------------------------------------------------------

void register_default_video_backends() noexcept {
    auto& reg = plugins::HwVideoBackendRegistry::instance();

    reg.register_encoder(make_stub_encoder_backend());
    reg.register_decoder(make_stub_decoder_backend());

#if defined(NIMRTC_PLUGINS_LIBVPX_ON)
    // The optional software VP9 encoder (remote-desktop plan B11).
    // Registered only when the vendored build is compiled in — with the
    // option off the plug-in is *absent*, not "present but unavailable".
    reg.register_encoder(make_libvpx_encoder_backend());
#endif

    core::log::Logger::instance().info(
        "vp9: registered video backends (encoder count=" +
        std::to_string(reg.list_encoders(plugins::VideoCodecKind::kVP9).size()) +
        ", decoder count=" +
        std::to_string(reg.list_decoders(plugins::VideoCodecKind::kVP9).size()) + ")");
}

// ---------------------------------------------------------------------------
// High-level convenience — used by the plugin factory to pick an
// encoder / decoder for the configured codec (or honour a
// `video_codec_name` override).
// ---------------------------------------------------------------------------

const plugins::VideoEncoderBackend*
select_encoder_backend(const plugins::VideoCodecConfig& cfg) noexcept {
    return plugins::HwVideoBackendRegistry::instance().select_encoder(
        plugins::VideoCodecKind::kVP9, cfg.name, cfg.backend_preference);
}

const plugins::VideoDecoderBackend*
select_decoder_backend(const plugins::VideoCodecConfig& cfg) noexcept {
    return plugins::HwVideoBackendRegistry::instance().select_decoder(
        plugins::VideoCodecKind::kVP9, cfg.name, cfg.backend_preference);
}

} // namespace nimrtc::vp9
