/**
 * @file src/modules/h264/src/hw_backends.cpp
 * @brief Default H.264 backend registry.
 *
 * Registers backend entries in priority order. Every platform has at
 * least one entry (the stub), plus conditionally-compiled entries for
 * the platform HW encoder.
 *
 * The actual HW backends live in individual source files under
 * src/modules/h264/src/:
 *   - nvenc_encoder.cpp  : NVIDIA NVENC + NVDEC   (NIMRTC_PLUGINS_NVENC_ON)
 *   - amf_encoder.cpp    : AMD AMF                (NIMRTC_PLUGINS_AMF_ON, Win)
 *   - qsv_encoder.cpp    : Intel QSV / libvpl     (NIMRTC_PLUGINS_QSV_ON)
 *   - dxva_decoder.cpp   : Microsoft DXVA/MF        (NIMRTC_PLUGINS_DXVA_ON, Win)
 *   - vaapi_encoder.cpp  : Linux VA-API           (NIMRTC_PLUGINS_VAAPI_ON, Linux)
 *   - openh264_encoder.cpp: OpenH264              (NIMRTC_PLUGINS_OPENH264_ON)
 *
 * Each source file exposes two symbols (stub when SDK absent):
 *   bool    <backend>_h264_available() noexcept;
 *   unique_ptr<IVideoCodec> make_<backend>_h264_codec(VideoCodecConfig);
 *
 * This file wires those symbols into the HwVideoBackendRegistry entries.
 * Priority values follow Sunshine's table.
 */

#include <nimrtc/h264/hw_backends.hpp>

#include <nimrtc/core/log.hpp>
#include <nimrtc/h264/codec_plugin.hpp>
#include <nimrtc/plugins/video_codec.hpp>

// Forward-declare the HW backend entry points exported by each source file.
// When the corresponding NIMRTC_PLUGINS_<NAME>_ON macro is defined, the
// source file compiles the real implementation; otherwise the stub (returning
// nullptr / false) is used.

#if defined(NIMRTC_PLUGINS_NVENC_ON)
extern bool nvenc_h264_available() noexcept;
extern std::unique_ptr<nimrtc::plugins::IVideoCodec>
make_nvenc_h264_codec(nimrtc::plugins::VideoCodecConfig cfg);
extern bool nvdec_h264_available() noexcept;
extern std::unique_ptr<nimrtc::plugins::IVideoCodec>
make_nvdec_h264_codec(nimrtc::plugins::VideoCodecConfig cfg);
#define HAS_NVENC 1
#else
#define HAS_NVENC 0
#endif

#if defined(NIMRTC_PLUGINS_AMF_ON) && defined(_WIN32)
extern bool amf_h264_available() noexcept;
extern std::unique_ptr<plugins::IVideoCodec>
make_amf_h264_codec(plugins::VideoCodecConfig cfg);
#define HAS_AMF 1
#else
#define HAS_AMF 0
#endif

#if defined(NIMRTC_PLUGINS_QSV_ON)
extern bool qsv_h264_available() noexcept;
extern std::unique_ptr<plugins::IVideoCodec>
make_qsv_h264_codec(plugins::VideoCodecConfig cfg);
#define HAS_QSV 1
#else
#define HAS_QSV 0
#endif

#if defined(NIMRTC_PLUGINS_DXVA_ON) && defined(_WIN32)
extern bool dxva_h264_available() noexcept;
extern std::unique_ptr<plugins::IVideoCodec>
make_dxva_h264_codec(plugins::VideoCodecConfig cfg);
#define HAS_DXVA 1
#else
#define HAS_DXVA 0
#endif

#if defined(NIMRTC_PLUGINS_VAAPI_ON) && defined(__linux__)
extern bool vaapi_h264_available() noexcept;
extern std::unique_ptr<plugins::IVideoCodec>
make_vaapi_h264_codec(plugins::VideoCodecConfig cfg);
#define HAS_VAAPI 1
#else
#define HAS_VAAPI 0
#endif

#if defined(NIMRTC_PLUGINS_OPENH264_ON)
extern bool openh264_h264_available() noexcept;
extern std::unique_ptr<plugins::IVideoCodec>
make_openh264_h264_codec(plugins::VideoCodecConfig cfg);
#define HAS_OPENH264 1
#else
#define HAS_OPENH264 0
#endif

namespace nimrtc::h264 {

// ---------------------------------------------------------------------------
// Stub backend —always available, zero-copy NOT supported, CPU only.
// Used as the default fallback when no HW backend is reachable on the
// current host. Also used as the test backend in unit tests (deterministic
// synthetic bitstream, no platform deps).
// ---------------------------------------------------------------------------

namespace {

plugins::VideoEncoderBackend make_stub_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "stub_h264";
    be.description    = "Stub H.264 encoder (test / fallback, CPU only)";
    be.backend        = plugins::HwBackend::Software;
    be.priority       = 0;
    be.codec_kind     = plugins::VideoCodecKind::kH264;
    be.is_hw          = false;
    be.zero_copy_supported = false;
    be.available = [](const plugins::VideoCodecConfig&)
                    noexcept -> bool { return true; };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        // The stub adapter is decode-first, but its encode() path also
        // synthesises an IDR-shaped bitstream so the engine loop works.
        return std::make_unique<CodecPluginAdapter>(std::move(cfg));
    };
    return be;
}

plugins::VideoDecoderBackend make_stub_decoder_backend() {
    plugins::VideoDecoderBackend be;
    be.id             = "stub_h264";
    be.description    = "Stub H.264 decoder (test / fallback, CPU only)";
    be.backend        = plugins::HwBackend::Software;
    be.priority       = 0;
    be.codec_kind     = plugins::VideoCodecKind::kH264;
    be.is_hw          = false;
    be.zero_copy_supported = false;
    be.available = [](const plugins::VideoCodecConfig&)
                    noexcept -> bool { return true; };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        // Decoder direction only —the existing adapter is decode-first.
        return std::make_unique<CodecPluginAdapter>(std::move(cfg));
    };
    return be;
}

// ---------------------------------------------------------------------------
// OpenH264 software —high enough to beat the stub when linked in.
// ---------------------------------------------------------------------------

#if HAS_OPENH264
plugins::VideoEncoderBackend make_openh264_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "openh264_h264";
    be.description    = "OpenH264 software H.264 encoder";
    be.backend        = plugins::HwBackend::OpenH264;
    be.priority       = 50;
    be.codec_kind     = plugins::VideoCodecKind::kH264;
    be.is_hw          = false;
    be.zero_copy_supported = false;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return openh264_h264_available();
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return make_openh264_h264_codec(std::move(cfg));
    };
    return be;
}
#endif

// ---------------------------------------------------------------------------
// NVENC —NVIDIA. Priority 450. available() probes the SDK directly.
// ---------------------------------------------------------------------------

#if HAS_NVENC
plugins::VideoEncoderBackend make_nvenc_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "nvenc_h264";
    be.description    = "NVIDIA NVENC H.264 encoder";
    be.backend        = plugins::HwBackend::Nvenc;
    be.priority       = 450;
    be.codec_kind     = plugins::VideoCodecKind::kH264;
    be.is_hw          = true;
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return nvenc_h264_available();
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return make_nvenc_h264_codec(std::move(cfg));
    };
    return be;
}

plugins::VideoDecoderBackend make_nvdec_decoder_backend() {
    plugins::VideoDecoderBackend be;
    be.id             = "nvdec_h264";
    be.description    = "NVIDIA NVDEC H.264 decoder";
    be.backend        = plugins::HwBackend::Nvdec;
    be.priority       = 430;
    be.codec_kind     = plugins::VideoCodecKind::kH264;
    be.is_hw          = true;
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return nvdec_h264_available();
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return make_nvdec_h264_codec(std::move(cfg));
    };
    return be;
}
#endif

// ---------------------------------------------------------------------------
// VideoToolbox —Apple. Priority 400. macOS only.
// ---------------------------------------------------------------------------

#ifdef __APPLE__
plugins::VideoEncoderBackend make_videotoolbox_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "videotoolbox_h264";
    be.description    = "Apple VideoToolbox H.264 encoder";
    be.backend        = plugins::HwBackend::VideoToolbox;
    be.priority       = 400;
    be.codec_kind     = plugins::VideoCodecKind::kH264;
    be.is_hw          = true;
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        // VTCompressionSessionCreate probe — TODO: implement when targeting macOS.
        return false;
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return nullptr;   // TODO: wire VideoToolbox encoder when macOS target
    };
    return be;
}
#endif

// ---------------------------------------------------------------------------
// VA-API —Linux Intel/AMD. Priority 360. available() probes vaInitialize.
// ---------------------------------------------------------------------------

#if HAS_VAAPI
plugins::VideoEncoderBackend make_vaapi_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "vaapi_h264";
    be.description    = "VA-API H.264 encoder (Linux Intel/AMD)";
    be.backend        = plugins::HwBackend::Vaapi;
    be.priority       = 360;
    be.codec_kind     = plugins::VideoCodecKind::kH264;
    be.is_hw          = true;
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return vaapi_h264_available();
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return make_vaapi_h264_codec(std::move(cfg));
    };
    return be;
}
#endif

// ---------------------------------------------------------------------------
// MediaCodec —Android. Priority 420. Android only.
// ---------------------------------------------------------------------------

#ifdef __ANDROID__
plugins::VideoEncoderBackend make_mediacodec_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "mediacodec_h264";
    be.description    = "Android MediaCodec H.264 encoder";
    be.backend        = plugins::HwBackend::MediaCodec;
    be.priority       = 420;
    be.codec_kind     = plugins::VideoCodecKind::kH264;
    be.is_hw          = true;
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        // Android MediaCodec: check if MediaCodecList returns an H.264 encoder.
        // TODO: implement when targeting Android.
        return false;
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return nullptr;   // TODO: wire Android MediaCodec when Android target
    };
    return be;
}
#endif

// ---------------------------------------------------------------------------
// AMF —Windows AMD. Priority 380. available() probes AMFInit.
// ---------------------------------------------------------------------------

#if HAS_AMF
plugins::VideoEncoderBackend make_amf_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "amf_h264";
    be.description    = "AMD AMF H.264 encoder (VCN)";
    be.backend        = plugins::HwBackend::Amf;
    be.priority       = 380;
    be.codec_kind     = plugins::VideoCodecKind::kH264;
    be.is_hw          = true;
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return amf_h264_available();
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return make_amf_h264_codec(std::move(cfg));
    };
    return be;
}
#endif

// ---------------------------------------------------------------------------
// Intel Quick Sync —Windows + Linux. Priority 340. available() probes MFXInit.
// ---------------------------------------------------------------------------

#if HAS_QSV
plugins::VideoEncoderBackend make_qsv_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "quicksync_h264";
    be.description    = "Intel Quick Sync H.264 encoder";
    be.backend        = plugins::HwBackend::Qsv;
    be.priority       = 340;
    be.codec_kind     = plugins::VideoCodecKind::kH264;
    be.is_hw          = true;
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return qsv_h264_available();
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return make_qsv_h264_codec(std::move(cfg));
    };
    return be;
}
#endif

// ---------------------------------------------------------------------------
// DXVA / MediaFoundation decoder —Windows fallback. Priority 320.
// available() probes CLSID_CMSH264DecoderMFT COM creation.
// ---------------------------------------------------------------------------

#if HAS_DXVA
plugins::VideoDecoderBackend make_dxva_decoder_backend() {
    plugins::VideoDecoderBackend be;
    be.id             = "dxva_h264";
    be.description    = "Microsoft DXVA / MediaFoundation H.264 decoder";
    be.backend        = plugins::HwBackend::Dxva;
    be.priority       = 320;
    be.codec_kind     = plugins::VideoCodecKind::kH264;
    be.is_hw          = true;
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return dxva_h264_available();
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return make_dxva_h264_codec(std::move(cfg));
    };
    return be;
}
#endif

} // namespace

// ---------------------------------------------------------------------------
// One-shot registration —called from `register_default_plugins()` in
// codec_plugin.cpp at static-init time. Idempotent: callers may invoke
// multiple times; the registry appends entries (use a flag if duplicates
// become a problem).
// ---------------------------------------------------------------------------

void register_default_video_backends() noexcept {
    auto& reg = plugins::HwVideoBackendRegistry::instance();

    reg.register_encoder(make_stub_encoder_backend());
    reg.register_decoder(make_stub_decoder_backend());

#if HAS_OPENH264
    reg.register_encoder(make_openh264_encoder_backend());
#endif

#if HAS_NVENC
    reg.register_encoder(make_nvenc_encoder_backend());
    reg.register_decoder(make_nvdec_decoder_backend());
#endif

#ifdef __APPLE__
    reg.register_encoder(make_videotoolbox_encoder_backend());
#endif

#if HAS_VAAPI
    reg.register_encoder(make_vaapi_encoder_backend());
#endif

#ifdef __ANDROID__
    reg.register_encoder(make_mediacodec_encoder_backend());
#endif

#if HAS_AMF
    reg.register_encoder(make_amf_encoder_backend());
#endif

#if HAS_QSV
    reg.register_encoder(make_qsv_encoder_backend());
#endif

#if HAS_DXVA
    reg.register_decoder(make_dxva_decoder_backend());
#endif

    core::log::Logger::instance().info(
        "h264: registered video backends (encoder count=" +
        std::to_string(reg.list_encoders(plugins::VideoCodecKind::kH264).size()) +
        ", decoder count=" +
        std::to_string(reg.list_decoders(plugins::VideoCodecKind::kH264).size()) + ")");
}

// ---------------------------------------------------------------------------
// High-level convenience —used by the engine to pick an encoder/decoder
// for the configured codec (or honour a `video_codec_name` override).
// ---------------------------------------------------------------------------

const plugins::VideoEncoderBackend*
select_encoder_backend(const plugins::VideoCodecConfig& cfg) noexcept {
    return plugins::HwVideoBackendRegistry::instance().select_encoder(
        plugins::VideoCodecKind::kH264, cfg.name);
}

const plugins::VideoDecoderBackend*
select_decoder_backend(const plugins::VideoCodecConfig& cfg) noexcept {
    return plugins::HwVideoBackendRegistry::instance().select_decoder(
        plugins::VideoCodecKind::kH264, cfg.name);
}

} // namespace nimrtc::h264
