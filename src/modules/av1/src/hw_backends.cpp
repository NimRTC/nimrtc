/**
 * @file src/modules/av1/src/hw_backends.cpp
 * @brief Default AV1 backend registry.
 *
 * Registers the AV1-flavoured entries of the shared NVIDIA backend table
 * (owned by the `nvcodec` module) plus the software stub that this module
 * implements.
 *
 * The NVIDIA integration is *not* re-implemented here: `nvcodec` owns the
 * NVENC / NVDEC driver code and exposes it through codec-parameterised
 * entry points (`nvenc_codec_available()` / `make_nvenc_codec()` and
 * friends), so the H.264, H.265 and AV1 codec modules share one copy of
 * the driver code instead of triplicating it.
 */

#include <nimrtc/av1/hw_backends.hpp>

#include <nimrtc/core/log.hpp>
#include <nimrtc/av1/codec_plugin.hpp>
#include <nimrtc/nvcodec/nv_backends.hpp>
#include <nimrtc/nvcodec/nv_codec_kind.hpp>
#include <nimrtc/plugins/video_codec.hpp>

namespace nimrtc::av1 {

namespace {

// ---------------------------------------------------------------------------
// Stub backend — always available, CPU only.
//
// The stub adapter is decode-first, but its encode() path also synthesises
// a sequence-header-carrying OBU stream, so the engine loop works
// end-to-end without a proprietary codec.
// ---------------------------------------------------------------------------

plugins::VideoEncoderBackend make_stub_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "stub_av1";
    be.description    = "Stub AV1 encoder (test / fallback, CPU only)";
    be.backend        = plugins::HwBackend::Software;
    be.priority       = 0;
    be.codec_kind     = plugins::VideoCodecKind::kAV1;
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
    be.id             = "stub_av1";
    be.description    = "Stub AV1 decoder (test / fallback, CPU only)";
    be.backend        = plugins::HwBackend::Software;
    be.priority       = 0;
    be.codec_kind     = plugins::VideoCodecKind::kAV1;
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

// ---------------------------------------------------------------------------
// NVENC — NVIDIA. Priority 450.
//
// The probe is codec-aware: it initialises a real encoder session for
// AV1, so a GPU without an AV1 encoder reports "not available".
// ---------------------------------------------------------------------------

plugins::VideoEncoderBackend make_nvenc_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "nvenc_av1";
    be.description    = "NVIDIA NVENC AV1 encoder";
    be.backend        = plugins::HwBackend::Nvenc;
    be.priority       = 450;
    be.codec_kind     = plugins::VideoCodecKind::kAV1;
    be.is_hw          = true;
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return nvcodec::nvenc_codec_available(nvcodec::NvCodec::kAv1);
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return nvcodec::make_nvenc_codec(nvcodec::NvCodec::kAv1, std::move(cfg));
    };
    return be;
}

// ---------------------------------------------------------------------------
// NVDEC — NVIDIA hardware decoder. Priority 430.
//
// Registered independently of NVENC so a decode-only build still exposes
// the `nvdec_av1` seam id.
// ---------------------------------------------------------------------------

plugins::VideoDecoderBackend make_nvdec_decoder_backend() {
    plugins::VideoDecoderBackend be;
    be.id             = "nvdec_av1";
    be.description    = "NVIDIA NVDEC AV1 decoder";
    be.backend        = plugins::HwBackend::Nvdec;
    be.priority       = 430;
    be.codec_kind     = plugins::VideoCodecKind::kAV1;
    be.is_hw          = true;
    // The decoder copies the mapped surface into host memory, so it
    // produces no GPU output surface — the zero-copy path does not apply.
    be.zero_copy_supported = false;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return nvcodec::nvdec_codec_available(nvcodec::NvCodec::kAv1);
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return nvcodec::make_nvdec_codec(nvcodec::NvCodec::kAv1, std::move(cfg));
    };
    return be;
}

} // namespace

// ---------------------------------------------------------------------------
// One-shot registration.
// ---------------------------------------------------------------------------

void register_default_video_backends() noexcept {
    auto& reg = plugins::HwVideoBackendRegistry::instance();

    reg.register_encoder(make_stub_encoder_backend());
    reg.register_decoder(make_stub_decoder_backend());

    // The NVENC / NVDEC entries are always registered — the availability
    // probe is what decides whether they are usable.
    reg.register_encoder(make_nvenc_encoder_backend());
    reg.register_decoder(make_nvdec_decoder_backend());

    core::log::Logger::instance().info(
        "av1: registered video backends (encoder count=" +
        std::to_string(reg.list_encoders(plugins::VideoCodecKind::kAV1).size()) +
        ", decoder count=" +
        std::to_string(reg.list_decoders(plugins::VideoCodecKind::kAV1).size()) + ")");
}

// ---------------------------------------------------------------------------
// High-level convenience — used by the plugin factory to pick an
// encoder / decoder for the configured codec (or honour a
// `video_codec_name` override).
// ---------------------------------------------------------------------------

const plugins::VideoEncoderBackend*
select_encoder_backend(const plugins::VideoCodecConfig& cfg) noexcept {
    return plugins::HwVideoBackendRegistry::instance().select_encoder(
        plugins::VideoCodecKind::kAV1, cfg.name);
}

const plugins::VideoDecoderBackend*
select_decoder_backend(const plugins::VideoCodecConfig& cfg) noexcept {
    return plugins::HwVideoBackendRegistry::instance().select_decoder(
        plugins::VideoCodecKind::kAV1, cfg.name);
}

} // namespace nimrtc::av1
