/**
 * @file src/modules/h265/src/hw_backends.cpp
 * @brief Default H.265 backend registry.
 *
 * Registers the HEVC-flavoured entries of the shared NVIDIA backend table
 * (owned by the `nvcodec` module) plus the software stub that this module
 * implements.
 *
 * The NVIDIA integration is *not* re-implemented here: `nvcodec` owns the
    10| * NVENC / NVDEC driver code and exposes it through codec-parameterised
 * entry points (`nvenc_codec_available()` / `make_nvenc_codec()` and
 * friends), so the H.264, H.265 and AV1 codec modules share one copy of
 * the driver code instead of triplicating it.
 *
 * ## The non-NVIDIA rows
 *
 * The HEVC table used to carry only the NVIDIA rows; the other four
 * (AMF, VA-API, Quick Sync, VideoToolbox) and the Android MediaCodec
 * row are registered here too, each with the same three-part contract
 * every backend in the tree follows:
 *
 *   - a **priority** (the table's ordering: NVIDIA 450, MediaCodec
 *     420, VideoToolbox 400, AMF 380, VA-API 360, Quick Sync 340, the
 *     software stub 0),
 *   - an **honest `available()` probe** that reports the truth about
 *     the *silicon*, not about the OS version — and that returns false
 *     (rather than crashing) on a host that has no such hardware,
 *   - a **`zero_copy_supported` flag** that says whether the backend can
 *     hand the engine a GPU surface without a CPU staging copy.
 *
 * The entries are registered on *every* platform: the seam id and the
 * `HwBackend` kind must stay stable across the tree, and the probe is
 * what decides whether a row is usable.  A backend that exists in the
 * table but not in the silicon reports `available() == false` — which is
 * the honest answer, and what the honesty test asserts.
 */

#include <nimrtc/h265/hw_backends.hpp>

#include <nimrtc/core/log.hpp>
#include <nimrtc/h265/codec_plugin.hpp>
#include <nimrtc/nvcodec/nv_backends.hpp>
#include <nimrtc/nvcodec/nv_codec_kind.hpp>
#include <nimrtc/nvcodec/vt_backends.hpp>
#include <nimrtc/plugins/video_codec.hpp>

namespace nimrtc::h265 {

// ---------------------------------------------------------------------------
// The backend entry points exported by each backend's source file.
//
// Each source file compiles the *seam* unconditionally: the entry
// points exist with the right signatures on every platform, and the
// probe inside them reports the truth (false, with a reason) when the
// hardware or the implementation is absent.  That is what lets the
// table rows below be registered unconditionally — the seam id and
// the `HwBackend` kind stay stable across the tree.
//
// The implementations live in:
//   amf_h265_encoder.cpp   : AMD AMF                (Windows)
//   vaapi_h265_encoder.cpp : Linux VA-API
//   qsv_h265_encoder.cpp   : Intel Quick Sync / libvpl (Windows + Linux)
// ---------------------------------------------------------------------------

extern bool amf_h265_available() noexcept;
extern std::string amf_h265_unavailable_reason();
extern std::unique_ptr<plugins::IVideoCodec>
make_amf_h265_codec(plugins::VideoCodecConfig cfg);

extern bool vaapi_h265_available() noexcept;
extern std::string vaapi_h265_unavailable_reason();
extern std::unique_ptr<plugins::IVideoCodec>
make_vaapi_h265_codec(plugins::VideoCodecConfig cfg);

extern bool qsv_h265_available() noexcept;
extern std::string qsv_h265_unavailable_reason();
extern std::unique_ptr<plugins::IVideoCodec>
make_qsv_h265_codec(plugins::VideoCodecConfig cfg);

namespace {

// ---------------------------------------------------------------------------
// Stub backend — always available, CPU only.
//
// The stub adapter is decode-first, but its encode() path also synthesises
// an IDR-shaped H.265 bitstream, so the engine loop works end-to-end
// without a proprietary codec.
// ---------------------------------------------------------------------------

plugins::VideoEncoderBackend make_stub_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "stub_h265";
    be.description    = "Stub H.265 encoder (test / fallback, CPU only)";
    be.backend        = plugins::HwBackend::Software;
    be.priority       = 0;
    be.codec_kind     = plugins::VideoCodecKind::kH265;
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
    be.id             = "stub_h265";
    be.description    = "Stub H.265 decoder (test / fallback, CPU only)";
    be.backend        = plugins::HwBackend::Software;
    be.priority       = 0;
    be.codec_kind     = plugins::VideoCodecKind::kH265;
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
// HEVC, so a GPU without an HEVC encoder reports "not available".
// ---------------------------------------------------------------------------

plugins::VideoEncoderBackend make_nvenc_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "nvenc_h265";
    be.description    = "NVIDIA NVENC H.265 / HEVC encoder";
    be.backend        = plugins::HwBackend::Nvenc;
    be.priority       = 450;
    be.codec_kind     = plugins::VideoCodecKind::kH265;
    be.is_hw          = true;
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return nvcodec::nvenc_codec_available(nvcodec::NvCodec::kH265);
    };
    be.unavailable_reason = [](const plugins::VideoCodecConfig&) -> std::string {
        return "nvenc: no usable NVENC session for H.265 / HEVC on this host "
               "— the NVIDIA driver is absent, the GPU has no HEVC encoder, "
               "or the session failed to open";
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return nvcodec::make_nvenc_codec(nvcodec::NvCodec::kH265, std::move(cfg));
    };
    return be;
}

// ---------------------------------------------------------------------------
// NVDEC — NVIDIA hardware decoder. Priority 430.
//
// Registered independently of NVENC so a decode-only build still exposes
// the `nvdec_h265` seam id.
// ---------------------------------------------------------------------------

plugins::VideoDecoderBackend make_nvdec_decoder_backend() {
    plugins::VideoDecoderBackend be;
    be.id             = "nvdec_h265";
    be.description    = "NVIDIA NVDEC H.265 / HEVC decoder";
    be.backend        = plugins::HwBackend::Nvdec;
    be.priority       = 430;
    be.codec_kind     = plugins::VideoCodecKind::kH265;
    be.is_hw          = true;
    // The decoder copies the mapped surface into host memory, so it
    // produces no GPU output surface — the zero-copy path does not apply.
    be.zero_copy_supported = false;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return nvcodec::nvdec_codec_available(nvcodec::NvCodec::kH265);
    };
    be.unavailable_reason = [](const plugins::VideoCodecConfig&) -> std::string {
        return "nvdec: no usable NVDEC session for H.265 / HEVC on this host "
               "— the NVIDIA driver is absent, the GPU has no HEVC decoder, "
               "or the session failed to open";
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return nvcodec::make_nvdec_codec(nvcodec::NvCodec::kH265, std::move(cfg));
    };
    return be;
}

// ---------------------------------------------------------------------------
// MediaCodec — Android. Priority 420.
//
// The seam is registered on every platform so the id and the
// `HwBackend::MediaCodec` kind stay stable.  The Android leg is **not
// implemented** (the NDK MediaCodec integration needs the NDK's
// `AMediaCodec` API, which this tree does not carry), so the probe
// honestly reports "not available" everywhere — including on an Android
// device — rather than pretending the row works.
//
// When the Android leg lands, the row's `available()` becomes a
// `MediaCodecList` query and `create()` builds the encoder; the
// priority, the kind and the id do not change.
// ---------------------------------------------------------------------------

plugins::VideoEncoderBackend make_mediacodec_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "mediacodec_h265";
    be.description    = "Android MediaCodec H.265 / HEVC encoder "
                         "(seam only — the Android leg is not implemented; "
                         "available() is false everywhere)";
    be.backend        = plugins::HwBackend::MediaCodec;
    be.priority       = 420;
    be.codec_kind     = plugins::VideoCodecKind::kH265;
    be.is_hw          = true;
    // No encoder exists yet, so no GPU output surface can be produced.
    be.zero_copy_supported = false;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        // The Android leg is not implemented (ADR: the NDK MediaCodec
        // integration is out of the current tree scope), so the honest
        // answer is "not available" on every platform.
        return false;
    };
    be.unavailable_reason = [](const plugins::VideoCodecConfig&)
        -> std::string {
        return "mediacodec: the NDK MediaCodec integration is not implemented "
               "in this tree, so the Android HEVC hardware encoder is not "
               "available on any host (the row exists so the seam id and "
               "the HwBackend::MediaCodec kind stay stable)";
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        (void)cfg;
        return nullptr;
    };
    return be;
}

// ---------------------------------------------------------------------------
// VideoToolbox — Apple. Priority 400.
//
// The probe creates and tears down a real `VTCompressionSession` for
// HEVC, so a Mac whose silicon has no HEVC encoder reports "not
// available" (and a non-Apple host reports the same).
//
// The decoder row is registered alongside it: VideoToolbox does both
// directions.
// ---------------------------------------------------------------------------

plugins::VideoEncoderBackend make_videotoolbox_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "videotoolbox_h265";
    be.description    = "Apple VideoToolbox H.265 / HEVC encoder";
    be.backend        = plugins::HwBackend::VideoToolbox;
    be.priority       = 400;
    be.codec_kind     = plugins::VideoCodecKind::kH265;
    be.is_hw          = true;
    // The encoder consumes and produces `CVPixelBufferRef` surfaces.
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return nvcodec::vt_encoder_available(nvcodec::VtCodec::kH265);
    };
    be.unavailable_reason = [](const plugins::VideoCodecConfig&)
        -> std::string {
        return nvcodec::vt_encoder_unavailable_reason(nvcodec::VtCodec::kH265);
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return nvcodec::make_vt_encoder(nvcodec::VtCodec::kH265, std::move(cfg));
    };
    return be;
}

plugins::VideoDecoderBackend make_videotoolbox_decoder_backend() {
    plugins::VideoDecoderBackend be;
    be.id             = "videotoolbox_h265";
    be.description    = "Apple VideoToolbox H.265 / HEVC decoder";
    be.backend        = plugins::HwBackend::VideoToolbox;
    be.priority       = 400;
    be.codec_kind     = plugins::VideoCodecKind::kH265;
    be.is_hw          = true;
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return nvcodec::vt_decoder_available(nvcodec::VtCodec::kH265);
    };
    be.unavailable_reason = [](const plugins::VideoCodecConfig&)
        -> std::string {
        return nvcodec::vt_decoder_unavailable_reason(nvcodec::VtCodec::kH265);
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return nvcodec::make_vt_decoder(nvcodec::VtCodec::kH265, std::move(cfg));
    };
    return be;
}

// ---------------------------------------------------------------------------
// AMF — Windows AMD. Priority 380.
//
// The probe initialises a real AMF component for the HEVC encoder GUID,
// so a machine without the AMD driver reports "not available".
// ---------------------------------------------------------------------------

plugins::VideoEncoderBackend make_amf_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "amf_h265";
    be.description    = "AMD AMF H.265 / HEVC encoder (VCN)";
    be.backend        = plugins::HwBackend::Amf;
    be.priority       = 380;
    be.codec_kind     = plugins::VideoCodecKind::kH265;
    be.is_hw          = true;
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return amf_h265_available();
    };
    be.unavailable_reason = [](const plugins::VideoCodecConfig&) -> std::string {
        return amf_h265_unavailable_reason();
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return make_amf_h265_codec(std::move(cfg));
    };
    return be;
}

// ---------------------------------------------------------------------------
// VA-API — Linux Intel / AMD. Priority 360.
//
// The probe opens a DRM render node and asks VA-API for an HEVC
// encoder entry point, so a machine without the Intel / AMD media
// driver (or without a VA-API capable GPU) reports "not available".
// ---------------------------------------------------------------------------

plugins::VideoEncoderBackend make_vaapi_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "vaapi_h265";
    be.description    = "VA-API H.265 / HEVC encoder (Linux Intel/AMD)";
    be.backend        = plugins::HwBackend::Vaapi;
    be.priority       = 360;
    be.codec_kind     = plugins::VideoCodecKind::kH265;
    be.is_hw          = true;
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return vaapi_h265_available();
    };
    be.unavailable_reason = [](const plugins::VideoCodecConfig&) -> std::string {
        return vaapi_h265_unavailable_reason();
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return make_vaapi_h265_codec(std::move(cfg));
    };
    return be;
}

// ---------------------------------------------------------------------------
// Intel Quick Sync — Windows + Linux. Priority 340.
//
// The probe opens a oneVPL session and queries the HEVC encoder entry
// point, so a machine without an Intel iGPU reports "not available".
// ---------------------------------------------------------------------------

plugins::VideoEncoderBackend make_qsv_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "quicksync_h265";
    be.description    = "Intel Quick Sync H.265 / HEVC encoder";
    be.backend        = plugins::HwBackend::Qsv;
    be.priority       = 340;
    be.codec_kind     = plugins::VideoCodecKind::kH265;
    be.is_hw          = true;
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return qsv_h265_available();
    };
    be.unavailable_reason = [](const plugins::VideoCodecConfig&) -> std::string {
        return qsv_h265_unavailable_reason();
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return make_qsv_h265_codec(std::move(cfg));
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

    // The non-NVIDIA HEVC rows (plan item B5).  All of them are
    // registered unconditionally: the seam id and the `HwBackend` kind
    // must stay stable across the tree, and the availability probe is
    // what decides whether a row is usable on this host.
    reg.register_encoder(make_mediacodec_encoder_backend());
    reg.register_encoder(make_videotoolbox_encoder_backend());
    reg.register_decoder(make_videotoolbox_decoder_backend());
    reg.register_encoder(make_amf_encoder_backend());
    reg.register_encoder(make_vaapi_encoder_backend());
    reg.register_encoder(make_qsv_encoder_backend());

    core::log::Logger::instance().info(
        "h265: registered video backends (encoder count=" +
        std::to_string(reg.list_encoders(plugins::VideoCodecKind::kH265).size()) +
        ", decoder count=" +
        std::to_string(reg.list_decoders(plugins::VideoCodecKind::kH265).size()) + ")");
}

// ---------------------------------------------------------------------------
// High-level convenience — used by the plugin factory to pick a
// encoder / decoder for the configured codec (or honour a
// `video_codec_name` override).
// ---------------------------------------------------------------------------

const plugins::VideoEncoderBackend*
select_encoder_backend(const plugins::VideoCodecConfig& cfg) noexcept {
    return plugins::HwVideoBackendRegistry::instance().select_encoder(
        plugins::VideoCodecKind::kH265, cfg.name, cfg.backend_preference);
}

const plugins::VideoDecoderBackend*
select_decoder_backend(const plugins::VideoCodecConfig& cfg) noexcept {
    return plugins::HwVideoBackendRegistry::instance().select_decoder(
        plugins::VideoCodecKind::kH265, cfg.name, cfg.backend_preference);
}

} // namespace nimrtc::h265
