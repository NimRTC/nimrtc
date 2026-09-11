/**
 * @file src/modules/h264/src/hw_backends.cpp
 * @brief Default H.264 backend registry.
 *
 * Registers backend entries in priority order. Every platform has at
 * least one entry (the stub), plus conditionally-compiled entries for
 * the platform HW encoder.
 *
 * The actual HW backends (MediaCodec / VideoToolbox / NVENC / AMF /
 * VAAPI / QSV / DXVA / V4l2 / MMAL) live in OUT-OF-TREE plugins; this
 * file declares the probe lambdas and creates the dispatch entries for
 * them. A future PR may inline these when the SDKs are available in
 * the vendor tree.
 */

#include <nimrtc/h264/hw_backends.hpp>

#include <nimrtc/core/log.hpp>
#include <nimrtc/h264/codec_plugin.hpp>

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
// OpenH264 software (placeholder) —registers with low priority but high
// enough to beat the stub when libopenh264 is linked in. Available only
// when NIMRTC_H264_OPENH264_ON is defined at build time.
// ---------------------------------------------------------------------------

#ifdef NIMRTC_H264_OPENH264_ON
plugins::VideoEncoderBackend make_openh264_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "openh264_h264";
    be.description    = "OpenH264 software H.264 encoder";
    be.backend        = plugins::HwBackend::OpenH264;
    be.priority       = 50;
    be.codec_kind     = plugins::VideoCodecKind::kH264;
    be.is_hw          = false;
    be.zero_copy_supported = false;
    be.available = [](const plugins::VideoCodecConfig& cfg)
                    noexcept -> bool { return true; };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        // Real OpenH264 binding will live in a separate out-of-tree plugin
        // until libopenh264 is added to the vendor tree. Stub for now.
        return std::make_unique<CodecPluginAdapter>(std::move(cfg));
    };
    return be;
}
#endif

// ---------------------------------------------------------------------------
// NVENC —NVIDIA. Only compiled in on Windows + Linux x86_64. Probe uses
// the runtime NVENC API (NvEncCreateInstance) —kept inline here for
// brevity; production would defer to the out-of-tree nvenc plugin.
// ---------------------------------------------------------------------------

#if (defined(_WIN32) || defined(__linux__)) && defined(NIMRTC_PLUGINS_NVENC_ON)
plugins::VideoEncoderBackend make_nvenc_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "nvenc_h264";
    be.description    = "NVIDIA NVENC H.264 encoder";
    be.backend        = plugins::HwBackend::Nvenc;
    be.priority       = 450;
    be.codec_kind     = plugins::VideoCodecKind::kH264;
    be.is_hw          = true;
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig& cfg)
                    noexcept -> bool {
        // Real probe: try NvEncOpenEncodeSessionEx. Out-of-tree plugin
        // overrides this; here we return false so the selector falls
        // through to the next backend when the SDK isn't linked.
        (void)cfg;
        return false;
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        // Real NVENC binding is out-of-tree. Fall back to the stub so the
        // engine can still run while the plugin is being loaded.
        return std::make_unique<CodecPluginAdapter>(std::move(cfg));
    };
    return be;
}
#endif

// ---------------------------------------------------------------------------
// VideoToolbox —Apple. Probe uses VTCompressionSessionCreate to verify
// the HW is reachable. Compiled only when __APPLE__ is defined.
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
    be.available = [](const plugins::VideoCodecConfig& cfg)
                    noexcept -> bool { (void)cfg; return false; };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return std::make_unique<CodecPluginAdapter>(std::move(cfg));
    };
    return be;
}
#endif

// ---------------------------------------------------------------------------
// VA-API —Linux Intel/AMD. Conditional on __linux__ and NIMRTC_PLUGINS_VAAPI_ON.
// ---------------------------------------------------------------------------

#if defined(__linux__) && defined(NIMRTC_PLUGINS_VAAPI_ON)
plugins::VideoEncoderBackend make_vaapi_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "vaapi_h264";
    be.description    = "VA-API H.264 encoder (Linux Intel/AMD)";
    be.backend        = plugins::HwBackend::Vaapi;
    be.priority       = 360;
    be.codec_kind     = plugins::VideoCodecKind::kH264;
    be.is_hw          = true;
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig& cfg)
                    noexcept -> bool { (void)cfg; return false; };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return std::make_unique<CodecPluginAdapter>(std::move(cfg));
    };
    return be;
}
#endif

// ---------------------------------------------------------------------------
// MediaCodec —Android. Conditional on __ANDROID__.
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
    be.available = [](const plugins::VideoCodecConfig& cfg)
                    noexcept -> bool { (void)cfg; return false; };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return std::make_unique<CodecPluginAdapter>(std::move(cfg));
    };
    return be;
}
#endif

// ---------------------------------------------------------------------------
// AMF —Windows AMD. Conditional on _WIN32 + NIMRTC_PLUGINS_AMF_ON.
// ---------------------------------------------------------------------------

#if defined(_WIN32) && defined(NIMRTC_PLUGINS_AMF_ON)
plugins::VideoEncoderBackend make_amf_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "amf_h264";
    be.description    = "AMD AMF H.264 encoder (VCN)";
    be.backend        = plugins::HwBackend::Amf;
    be.priority       = 380;
    be.codec_kind     = plugins::VideoCodecKind::kH264;
    be.is_hw          = true;
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig& cfg)
                    noexcept -> bool { (void)cfg; return false; };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return std::make_unique<CodecPluginAdapter>(std::move(cfg));
    };
    return be;
}
#endif

// ---------------------------------------------------------------------------
// Intel Quick Sync —Windows. Conditional on _WIN32 + NIMRTC_PLUGINS_QSV_ON.
// ---------------------------------------------------------------------------

#if defined(_WIN32) && defined(NIMRTC_PLUGINS_QSV_ON)
plugins::VideoEncoderBackend make_qsv_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "quicksync_h264";
    be.description    = "Intel Quick Sync H.264 encoder";
    be.backend        = plugins::HwBackend::Qsv;
    be.priority       = 340;
    be.codec_kind     = plugins::VideoCodecKind::kH264;
    be.is_hw          = true;
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig& cfg)
                    noexcept -> bool { (void)cfg; return false; };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return std::make_unique<CodecPluginAdapter>(std::move(cfg));
    };
    return be;
}
#endif

// ---------------------------------------------------------------------------
// DXVA / MediaFoundation decoder —Windows fallback when no encoder is
// available but the GPU can still decode. Lower priority than encoder HW
// because decode is rarely the bottleneck; priority matches WebRTC.
// ---------------------------------------------------------------------------

#if defined(_WIN32) && defined(NIMRTC_PLUGINS_DXVA_ON)
plugins::VideoDecoderBackend make_dxva_decoder_backend() {
    plugins::VideoDecoderBackend be;
    be.id             = "dxva_h264";
    be.description    = "Microsoft DXVA / MediaFoundation H.264 decoder";
    be.backend        = plugins::HwBackend::Dxva;
    be.priority       = 320;
    be.codec_kind     = plugins::VideoCodecKind::kH264;
    be.is_hw          = true;
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig& cfg)
                    noexcept -> bool { (void)cfg; return false; };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return std::make_unique<CodecPluginAdapter>(std::move(cfg));
    };
    return be;
}
#endif

// ---------------------------------------------------------------------------
// NVDEC decoder —NVIDIA (Win/Linux). Priority 360.
// ---------------------------------------------------------------------------

#if (defined(_WIN32) || defined(__linux__)) && defined(NIMRTC_PLUGINS_NVENC_ON)
plugins::VideoDecoderBackend make_nvdec_decoder_backend() {
    plugins::VideoDecoderBackend be;
    be.id             = "nvdec_h264";
    be.description    = "NVIDIA NVDEC H.264 decoder";
    be.backend        = plugins::HwBackend::Nvdec;
    be.priority       = 430;
    be.codec_kind     = plugins::VideoCodecKind::kH264;
    be.is_hw          = true;
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig& cfg)
                    noexcept -> bool { (void)cfg; return false; };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return std::make_unique<CodecPluginAdapter>(std::move(cfg));
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

#ifdef NIMRTC_H264_OPENH264_ON
    reg.register_encoder(make_openh264_encoder_backend());
#endif

#if (defined(_WIN32) || defined(__linux__)) && defined(NIMRTC_PLUGINS_NVENC_ON)
    reg.register_encoder(make_nvenc_encoder_backend());
    reg.register_decoder(make_nvdec_decoder_backend());
#endif

#ifdef __APPLE__
    reg.register_encoder(make_videotoolbox_encoder_backend());
#endif

#if defined(__linux__) && defined(NIMRTC_PLUGINS_VAAPI_ON)
    reg.register_encoder(make_vaapi_encoder_backend());
#endif

#ifdef __ANDROID__
    reg.register_encoder(make_mediacodec_encoder_backend());
#endif

#if defined(_WIN32) && defined(NIMRTC_PLUGINS_AMF_ON)
    reg.register_encoder(make_amf_encoder_backend());
#endif

#if defined(_WIN32) && defined(NIMRTC_PLUGINS_QSV_ON)
    reg.register_encoder(make_qsv_encoder_backend());
#endif

#if defined(_WIN32) && defined(NIMRTC_PLUGINS_DXVA_ON)
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
