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
 *   - amf_encoder.cpp    : AMD AMF                (NIMRTC_PLUGINS_AMF_ON, Win)
 *   - qsv_encoder.cpp    : Intel QSV / libvpl     (NIMRTC_PLUGINS_QSV_ON)
 *   - dxva_decoder.cpp   : Microsoft DXVA/MF        (NIMRTC_PLUGINS_DXVA_ON, Win)
 *   - vaapi_encoder.cpp  : Linux VA-API           (NIMRTC_PLUGINS_VAAPI_ON, Linux)
 *   - openh264_encoder.cpp: OpenH264              (NIMRTC_PLUGINS_OPENH264_ON)
 *
 * The NVIDIA backends (NVENC / NVDEC) are *shared* with the H.265 and
 * AV1 codec modules: they live in the `nvcodec` module, which owns the
 * driver integration and exposes it through codec-parameterised entry
 * points (`nvcodec::nvenc_codec_available()` and friends).  This module
 * simply registers the H.264-flavoured entries for them.
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
#include <nimrtc/nvcodec/nv_backends.hpp>
#include <nimrtc/nvcodec/nv_codec_kind.hpp>
#include <nimrtc/nvcodec/vt_backends.hpp>
#include <nimrtc/plugins/video_codec.hpp>

namespace nimrtc::h264 {

// Forward-declare the HW backend entry points exported by each source file.
// When the corresponding NIMRTC_PLUGINS_<NAME>_ON macro is defined, the
// source file compiles the real implementation; otherwise the stub
// (returning nullptr / false) is used.  Declared inside the
// `nimrtc::h264` namespace so they match the definitions in
// amf_encoder.cpp / qsv_encoder.cpp / etc.

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
// OpenH264 software — high enough to beat the stub when linked in.
//
// The codec is a *real* H.264 encoder + decoder (the vendored OpenH264
// source build), so its `backend_id()` is "openh264" — distinct from the
// synthetic stub's "software-stub".
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
    be.unavailable_reason = [](const plugins::VideoCodecConfig&) -> std::string {
        return "openh264: the vendored OpenH264 software encoder is not "
               "available in this build";
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return make_openh264_h264_codec(std::move(cfg));
    };
    return be;
}

plugins::VideoDecoderBackend make_openh264_decoder_backend() {
    plugins::VideoDecoderBackend be;
    be.id             = "openh264_h264";
    be.description    = "OpenH264 software H.264 decoder";
    be.backend        = plugins::HwBackend::OpenH264;
    be.priority       = 50;
    be.codec_kind     = plugins::VideoCodecKind::kH264;
    be.is_hw          = false;
    be.zero_copy_supported = false;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return openh264_h264_available();
    };
    be.unavailable_reason = [](const plugins::VideoCodecConfig&) -> std::string {
        return "openh264: the vendored OpenH264 software decoder is not "
               "available in this build";
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
//
// The probe is codec-aware: it initializes a real encoder session for
// the requested codec, so a GPU without an HEVC (or AV1) encoder reports
// "not available" instead of "yes, because NVENC exists".
// ---------------------------------------------------------------------------

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
        return nvcodec::nvenc_codec_available(nvcodec::NvCodec::kH264);
    };
    be.unavailable_reason = [](const plugins::VideoCodecConfig&) -> std::string {
        return "nvenc: no usable NVENC session for H.264 on this host — the "
               "NVIDIA driver is absent, the GPU has no H.264 encoder, or "
               "the session failed to open";
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return nvcodec::make_nvenc_codec(nvcodec::NvCodec::kH264, std::move(cfg));
    };
    return be;
}

// ---------------------------------------------------------------------------
// NVDEC — NVIDIA hardware decoder. Priority 430.
//
// Registered independently of NVENC: a decode-only build (or a host whose
// NVENC session is unusable) must still expose the `nvdec_h264` seam id
// when the NIMRTC_PLUGINS_NVDEC option is on.
// ---------------------------------------------------------------------------

plugins::VideoDecoderBackend make_nvdec_decoder_backend() {
    plugins::VideoDecoderBackend be;
    be.id             = "nvdec_h264";
    be.description    = "NVIDIA NVDEC H.264 decoder";
    be.backend        = plugins::HwBackend::Nvdec;
    be.priority       = 430;
    be.codec_kind     = plugins::VideoCodecKind::kH264;
    be.is_hw          = true;
    // The decoder copies the mapped NV12 surface into host memory, so it
    // produces no GPU output surface — the zero-copy path does not apply.
    be.zero_copy_supported = false;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return nvcodec::nvdec_codec_available(nvcodec::NvCodec::kH264);
    };
    be.unavailable_reason = [](const plugins::VideoCodecConfig&) -> std::string {
        return "nvdec: no usable NVDEC session for H.264 on this host — the "
               "NVIDIA driver is absent, the GPU has no H.264 decoder, or "
               "the session failed to open";
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return nvcodec::make_nvdec_codec(nvcodec::NvCodec::kH264, std::move(cfg));
    };
    return be;
}

// ---------------------------------------------------------------------------
// VideoToolbox —Apple. Priority 400.
//
// The entry is registered on *every* platform — not only on Apple — so
// the seam id ("videotoolbox_h264") and the `HwBackend::VideoToolbox`
// kind stay stable across the tree, and the honesty test can assert
// that the seam *exists* on a non-Apple host.
//
// ## Why priority 400?
//
// The priority ladder follows Sunshine's: NVENC 450 (highest), NVDEC
// 430, MediaCodec 420, **VideoToolbox 400**, AMF 380, VA-API 360, Quick
// Sync 340, DXVA 320, the software codecs 50, the stub 0.
//
// VideoToolbox sits *below* the Android MediaCodec row and *above* the
// AMD / Intel / Linux rows because:
//
//   - on a Mac it is the *only* hardware encoder, so its absolute
//     position among the hardware rows does not matter — it wins by
//     being the only one whose `available()` is true, and
//   - it is above every desktop-Vendor row (AMF / VA-API / QSV) so
//     that a shared priority table stays meaningful: a Mac never
//     competes with an AMD card for the same seat, and the ordering
//     reads as "the platform's own encoder first".
//
// It stays above the software path (50 / 0) by a wide margin, which is
// the point of B5a: without it macOS falls back to software encoding,
// which does not fit the plan's CPU budget at 1080p30.
//
// The availability probe is the real thing: it creates and tears down
// a `VTCompressionSession` for H.264.  On a non-Apple build it
// reports false (there is no VideoToolbox at all); on a Mac whose
// silicon cannot do H.264 it also reports false.  Only a Mac with a
// working H.264 encoder reports true.
//
// The decoder is registered alongside it: VideoToolbox does both
// directions, and the decode path is what makes the macOS leg of the
// remote-desktop pipeline work end to end.
// ---------------------------------------------------------------------------

plugins::VideoEncoderBackend make_videotoolbox_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "videotoolbox_h264";
    be.description    = "Apple VideoToolbox H.264 encoder";
    be.backend        = plugins::HwBackend::VideoToolbox;
    be.priority       = 400;
    be.codec_kind     = plugins::VideoCodecKind::kH264;
    be.is_hw          = true;
    // The encoder consumes and produces `CVPixelBufferRef` surfaces,
    // which travel in the tree's GpuBuffer as
    // `GpuBackend::kAppleCFPixelBuffer` — so the zero-copy path
    // applies.
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return nvcodec::vt_encoder_available(nvcodec::VtCodec::kH264);
    };
    be.unavailable_reason = [](const plugins::VideoCodecConfig&)
        -> std::string {
        return nvcodec::vt_encoder_unavailable_reason(nvcodec::VtCodec::kH264);
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return nvcodec::make_vt_encoder(nvcodec::VtCodec::kH264, std::move(cfg));
    };
    return be;
}

plugins::VideoDecoderBackend make_videotoolbox_decoder_backend() {
    plugins::VideoDecoderBackend be;
    be.id             = "videotoolbox_h264";
    be.description    = "Apple VideoToolbox H.264 decoder";
    be.backend        = plugins::HwBackend::VideoToolbox;
    be.priority       = 400;
    be.codec_kind     = plugins::VideoCodecKind::kH264;
    be.is_hw          = true;
    // The decoder produces a `CVPixelBufferRef`, so it can fill a GPU
    // output surface without a CPU round-trip.
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return nvcodec::vt_decoder_available(nvcodec::VtCodec::kH264);
    };
    be.unavailable_reason = [](const plugins::VideoCodecConfig&)
        -> std::string {
        return nvcodec::vt_decoder_unavailable_reason(nvcodec::VtCodec::kH264);
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return nvcodec::make_vt_decoder(nvcodec::VtCodec::kH264, std::move(cfg));
    };
    return be;
}

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
    be.unavailable_reason = [](const plugins::VideoCodecConfig&) -> std::string {
        return "vaapi: no usable VA-API H.264 encoder on this host — the "
               "VA-API runtime is absent, no DRM render node could be "
               "opened, or the driver exposes no H.264 encoder entry point";
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
    // NOT IMPLEMENTED — the entry exists so the seam id and the
    // `HwBackend::MediaCodec` kind stay stable, but `available()` returns
    // false and `create()` returns nullptr.
    be.description    = "Android MediaCodec H.264 encoder (NOT IMPLEMENTED — create() returns nullptr)";
    be.backend        = plugins::HwBackend::MediaCodec;
    be.priority       = 420;
    be.codec_kind     = plugins::VideoCodecKind::kH264;
    be.is_hw          = true;
    // No encoder exists, so no GPU output surface can be produced.
    be.zero_copy_supported = false;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        // Android MediaCodec: check if MediaCodecList returns an H.264 encoder.
        // TODO: implement when targeting Android.
        return false;
    };
    be.unavailable_reason = [](const plugins::VideoCodecConfig&) -> std::string {
        return "mediacodec: the NDK MediaCodec integration is not implemented "
               "in this tree, so the Android H.264 hardware encoder is not "
               "available on any host";
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
    be.unavailable_reason = [](const plugins::VideoCodecConfig&) -> std::string {
        return "amf: no usable AMD AMF H.264 encoder on this host — the AMF "
               "runtime is absent, no compatible AMD GPU was found, or the "
               "encoder component failed to initialise";
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
    be.unavailable_reason = [](const plugins::VideoCodecConfig&) -> std::string {
        return "quicksync: no usable Intel Quick Sync H.264 encoder on this "
               "host — oneVPL / libvpl could not initialise, or no Intel iGPU "
               "with an H.264 encoder was found";
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
    be.unavailable_reason = [](const plugins::VideoCodecConfig&) -> std::string {
        return "dxva: no usable Microsoft DXVA / MediaFoundation H.264 "
               "decoder on this host — the decoder MFT could not be created "
               "or activated";
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
    reg.register_decoder(make_openh264_decoder_backend());
#endif

    // The NVENC / NVDEC entries are always registered — the availability
    // probe is what decides whether they are usable, and the probe is
    // honest about both "no NVIDIA driver" and "this GPU has no encoder
    // for that codec".
    reg.register_encoder(make_nvenc_encoder_backend());
    reg.register_decoder(make_nvdec_decoder_backend());

    // The VideoToolbox entries are registered on *every* platform: the
    // seam id and the `HwBackend::VideoToolbox` kind must stay stable
    // across the tree, and the availability probe is what decides
    // whether they are usable.  On a non-Apple host the probe reports
    // false — which is the honest answer.
    reg.register_encoder(make_videotoolbox_encoder_backend());
    reg.register_decoder(make_videotoolbox_decoder_backend());
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
        plugins::VideoCodecKind::kH264, cfg.name, cfg.backend_preference);
}

const plugins::VideoDecoderBackend*
select_decoder_backend(const plugins::VideoCodecConfig& cfg) noexcept {
    return plugins::HwVideoBackendRegistry::instance().select_decoder(
        plugins::VideoCodecKind::kH264, cfg.name, cfg.backend_preference);
}

} // namespace nimrtc::h264
