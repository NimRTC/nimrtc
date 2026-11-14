/**
 * @file src/modules/av1/src/hw_backends.cpp
 * @brief Default AV1 backend registry.
 *
 * Registers the AV1-flavoured entries of the shared NVIDIA backend table
 * (owned by the `nvcodec` module) plus the software stub that this module
 * implements.
 *
 * The NVIDIA integration is *not* re-implemented here: `nvcodec` owns the
    10| * NVENC / NVDEC driver code and exposes it through codec-parameterised
 * entry points (`nvenc_codec_available()` / `make_nvenc_codec()` and
 * friends), so the H.264, H.265 and AV1 codec modules share one copy of
 * the driver code instead of triplicating it.
 *
 * ## The non-NVIDIA rows (plan item B6)
 *
 * The AV1 table used to carry only the NVIDIA rows; the AMF, VA-API and
 * MediaCodec rows are registered here too, each with the same contract
 * every backend in the tree follows:
 *
 *   - a **priority** (NVIDIA 450, MediaCodec 420, AMF 380, VA-API 360,
 *     the software rows 50 / 0),
 *   - an **honest `available()` probe** that reports the truth about the
 *     *silicon* — and that returns false (rather than crashing) on a
 *     host that has no such hardware,
 *   - a **`zero_copy_supported` flag**.
 *
 * The rows are registered on *every* platform: the seam id and the
 * `HwBackend` kind must stay stable across the tree, and the probe is
 * what decides whether a row is usable.
 */

#include <nimrtc/av1/hw_backends.hpp>

#include <nimrtc/core/log.hpp>
#include <nimrtc/av1/codec_plugin.hpp>
#include <nimrtc/nvcodec/nv_backends.hpp>
#include <nimrtc/nvcodec/nv_codec_kind.hpp>
#include <nimrtc/nvcodec/vt_backends.hpp>
#include <nimrtc/plugins/video_codec.hpp>

namespace nimrtc::av1 {

#if defined(NIMRTC_PLUGINS_DAV1D_ON)
extern bool dav1d_av1_available() noexcept;
extern std::unique_ptr<plugins::IVideoCodec>
make_dav1d_av1_codec(plugins::VideoCodecConfig cfg);
#define HAS_DAV1D 1
#else
#define HAS_DAV1D 0
#endif

// ---------------------------------------------------------------------------
// The backend entry points exported by each backend's source file.
//
// Each source file compiles the *seam* unconditionally: the entry
// points exist with the right signatures on every platform, and the
// probe inside them reports the truth (false, with a reason) when the
// hardware or the implementation is absent.
//
// The implementations live in:
//   amf_av1_encoder.cpp   : AMD AMF       (Windows)
//   vaapi_av1_encoder.cpp : Linux VA-API
// ---------------------------------------------------------------------------

extern bool amf_av1_available() noexcept;
extern std::string amf_av1_unavailable_reason();
extern std::unique_ptr<plugins::IVideoCodec>
make_amf_av1_codec(plugins::VideoCodecConfig cfg);

extern bool vaapi_av1_available() noexcept;
extern std::string vaapi_av1_unavailable_reason();
extern std::unique_ptr<plugins::IVideoCodec>
make_vaapi_av1_codec(plugins::VideoCodecConfig cfg);

// The optional software AV1 encoder (remote-desktop plan B11).  The
// entry points exist on every platform, but the *row* is only registered
// when the vendored build is compiled in — the plan's B11 acceptance is
// "with the option OFF the build still configures and the plug-in is
// absent", not "the plug-in is present but unavailable".
extern bool svt_av1_available() noexcept;
extern std::string svt_av1_unavailable_reason();
extern std::unique_ptr<plugins::IVideoCodec>
make_svt_av1_codec(plugins::VideoCodecConfig cfg);

namespace {

// ---------------------------------------------------------------------------
// Stub backend — always available, CPU only.
//
// The stub adapter is decode-first, but its encode() path also synthesises
// a sequence-header-carrying OBU stream, so the engine loop works
// end-to-end without a proprietary codec.
// ---------------------------------------------------------------------------

/** The stub codec in its *encode-only* shape.
 *
 *  The stub does not decode AV1: it paints a synthetic pattern over the
 *  frame it was handed.  Letting the stub answer `can_decode() == true`
 *  would make the plugin factory stop at the encoder and hand the stub
 *  back as a bidirectional codec — which would leave a real decoder
 *  (dav1d) unreachable no matter how high its registry priority is.
 *
 *  So the *encoder* backend hands out this encode-only view of the stub,
 *  and the factory composes it with whichever decoder the registry
 *  selects (dav1d when the vendored build is linked in, the stub
 *  otherwise). */
class EncodeOnlyStubCodec final : public CodecPluginAdapter {
public:
    explicit EncodeOnlyStubCodec(plugins::VideoCodecConfig cfg) noexcept
        : CodecPluginAdapter(std::move(cfg)) {}

    bool can_decode() const noexcept override { return false; }
};

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
        return std::make_unique<EncodeOnlyStubCodec>(std::move(cfg));
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
// dav1d software decoder — priority 50, above the synthetic stub.
//
// The vendored dav1d is a *real* AV1 decoder, so its `backend_id()` is
// "dav1d" — distinct from the stub's "software-stub".
//
// There is no dav1d *encoder* (dav1d is decode-only), so the encoder
// direction stays with the stub / NVENC.
// ---------------------------------------------------------------------------

#if HAS_DAV1D
plugins::VideoDecoderBackend make_dav1d_decoder_backend() {
    plugins::VideoDecoderBackend be;
    be.id             = "dav1d_av1";
    be.description    = "dav1d software AV1 decoder";
    be.backend        = plugins::HwBackend::Dav1d;
    be.priority       = 50;
    be.codec_kind     = plugins::VideoCodecKind::kAV1;
    be.is_hw          = false;
    be.zero_copy_supported = false;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return dav1d_av1_available();
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return make_dav1d_av1_codec(std::move(cfg));
    };
    return be;
}
#endif

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
    be.unavailable_reason = [](const plugins::VideoCodecConfig&) -> std::string {
        return "nvenc: no usable NVENC session for AV1 on this host — the "
               "NVIDIA driver is absent, the GPU has no AV1 encoder "
               "(AV1 NVENC arrived with the Ada / RTX 40 series), or the "
               "session failed to open";
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
    be.unavailable_reason = [](const plugins::VideoCodecConfig&) -> std::string {
        return "nvdec: no usable NVDEC session for AV1 on this host — the "
               "NVIDIA driver is absent, the GPU has no AV1 decoder "
               "(AV1 NVDEC arrived with the Ada / RTX 40 series), or the "
               "session failed to open";
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return nvcodec::make_nvdec_codec(nvcodec::NvCodec::kAv1, std::move(cfg));
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
// honestly reports "not available" everywhere.
// ---------------------------------------------------------------------------

plugins::VideoEncoderBackend make_mediacodec_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "mediacodec_av1";
    be.description    = "Android MediaCodec AV1 encoder "
                         "(seam only — the Android leg is not implemented; "
                         "available() is false everywhere)";
    be.backend        = plugins::HwBackend::MediaCodec;
    be.priority       = 420;
    be.codec_kind     = plugins::VideoCodecKind::kAV1;
    be.is_hw          = true;
    be.zero_copy_supported = false;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return false;
    };
    be.unavailable_reason = [](const plugins::VideoCodecConfig&)
        -> std::string {
        return "mediacodec: the NDK MediaCodec integration is not implemented "
               "in this tree, so the Android AV1 hardware encoder is not "
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
// The probe creates and tears down a real `VTCompressionSession` for AV1.
//
// Note: as of macOS 14 (Sonoma) VideoToolbox exposes AV1 **decode** on M3+
// silicon, but *no* Apple chip has an AV1 hardware *encoder* — so this
// row's probe reports "not available" on every Mac, naming the failed
// session creation as the reason.  The row is still registered on
// every platform so the seam id ("videotoolbox_av1") and the
// `HwBackend::VideoToolbox` kind stay stable across the tree, and so a
// future Apple silicon with an AV1 encoder is picked up without a
// table change.
// ---------------------------------------------------------------------------

plugins::VideoEncoderBackend make_videotoolbox_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "videotoolbox_av1";
    be.description    = "Apple VideoToolbox AV1 encoder (macOS 14+; no "
                         "current Apple silicon has an AV1 encoder)";
    be.backend        = plugins::HwBackend::VideoToolbox;
    be.priority       = 400;
    be.codec_kind     = plugins::VideoCodecKind::kAV1;
    be.is_hw          = true;
    // The encoder consumes and produces `CVPixelBufferRef` surfaces.
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return nvcodec::vt_encoder_available(nvcodec::VtCodec::kAv1);
    };
    be.unavailable_reason = [](const plugins::VideoCodecConfig&)
        -> std::string {
        return nvcodec::vt_encoder_unavailable_reason(nvcodec::VtCodec::kAv1);
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return nvcodec::make_vt_encoder(nvcodec::VtCodec::kAv1, std::move(cfg));
    };
    return be;
}

plugins::VideoDecoderBackend make_videotoolbox_decoder_backend() {
    plugins::VideoDecoderBackend be;
    be.id             = "videotoolbox_av1";
    be.description    = "Apple VideoToolbox AV1 decoder (macOS 14+; M3+ "
                         "silicon only)";
    be.backend        = plugins::HwBackend::VideoToolbox;
    be.priority       = 400;
    be.codec_kind     = plugins::VideoCodecKind::kAV1;
    be.is_hw          = true;
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return nvcodec::vt_decoder_available(nvcodec::VtCodec::kAv1);
    };
    be.unavailable_reason = [](const plugins::VideoCodecConfig&)
        -> std::string {
        return nvcodec::vt_decoder_unavailable_reason(nvcodec::VtCodec::kAv1);
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return nvcodec::make_vt_decoder(nvcodec::VtCodec::kAv1, std::move(cfg));
    };
    return be;
}

// ---------------------------------------------------------------------------
// AMF — Windows AMD. Priority 380.
//
// AMD's AMF SDK exposes an AV1 encoder component on the VCN 3.0 / 4.0
// silicon.  The probe creates the component for real, so a machine
// without that silicon reports "not available" — which is the honest
// answer, and what the plan asks for ("a backend that exists in the
// table but not in the silicon reports `available() == false`").
// ---------------------------------------------------------------------------

plugins::VideoEncoderBackend make_amf_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "amf_av1";
    be.description    = "AMD AMF AV1 encoder (VCN 3.0 / 4.0)";
    be.backend        = plugins::HwBackend::Amf;
    be.priority       = 380;
    be.codec_kind     = plugins::VideoCodecKind::kAV1;
    be.is_hw          = true;
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return amf_av1_available();
    };
    be.unavailable_reason = [](const plugins::VideoCodecConfig&) -> std::string {
        return amf_av1_unavailable_reason();
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return make_amf_av1_codec(std::move(cfg));
    };
    return be;
}

// ---------------------------------------------------------------------------
// VA-API — Linux Intel / AMD. Priority 360.
//
// The probe opens a DRM render node and asks VA-API for an AV1 encoder
// entry point.  Note that VA-API's AV1 *encoder* support is newer and
// narrower than its HEVC support — the probe is what tells the truth
// about a given driver.
// ---------------------------------------------------------------------------

plugins::VideoEncoderBackend make_vaapi_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "vaapi_av1";
    be.description    = "VA-API AV1 encoder (Linux Intel/AMD)";
    be.backend        = plugins::HwBackend::Vaapi;
    be.priority       = 360;
    be.codec_kind     = plugins::VideoCodecKind::kAV1;
    be.is_hw          = true;
    be.zero_copy_supported = true;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return vaapi_av1_available();
    };
    be.unavailable_reason = [](const plugins::VideoCodecConfig&) -> std::string {
        return vaapi_av1_unavailable_reason();
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return make_vaapi_av1_codec(std::move(cfg));
    };
    return be;
}

} // namespace

// ---------------------------------------------------------------------------
// SVT-AV1 — the optional software AV1 encoder (remote-desktop plan B11).
//
// Priority 50: above the synthetic stub (0) and *at* the level of the
// other software rows (dav1d is 50 too — one is the decoder half, this
// is the encoder half).
//
// The row is registered ONLY when the vendored build is compiled in
// (`NIMRTC_PLUGINS_SVT_AV1_ON`).  With the option off the plug-in is
// *absent* from the registry, which is exactly what the plan's B11
// acceptance asks for ("with the option OFF the build still configures
// and the plug-in is absent").
// ---------------------------------------------------------------------------

#if defined(NIMRTC_PLUGINS_SVT_AV1_ON)
plugins::VideoEncoderBackend make_svt_av1_encoder_backend() {
    plugins::VideoEncoderBackend be;
    be.id             = "svt_av1";
    be.description    = "SVT-AV1 software AV1 encoder (screen-content mode "
                         "+ intra block copy)";
    be.backend        = plugins::HwBackend::Software;
    be.priority       = 50;
    be.codec_kind     = plugins::VideoCodecKind::kAV1;
    be.is_hw          = false;
    // The encoder consumes CPU planes — it has no GPU surface path.
    be.zero_copy_supported = false;
    be.available = [](const plugins::VideoCodecConfig&) noexcept -> bool {
        return svt_av1_available();
    };
    be.unavailable_reason = [](const plugins::VideoCodecConfig&) -> std::string {
        return svt_av1_unavailable_reason();
    };
    be.create = [](plugins::VideoCodecConfig cfg)
                -> std::unique_ptr<plugins::IVideoCodec> {
        return make_svt_av1_codec(std::move(cfg));
    };
    return be;
}
#endif

// ---------------------------------------------------------------------------
// One-shot registration.
// ---------------------------------------------------------------------------

void register_default_video_backends() noexcept {
    auto& reg = plugins::HwVideoBackendRegistry::instance();

    reg.register_encoder(make_stub_encoder_backend());
    reg.register_decoder(make_stub_decoder_backend());

#if HAS_DAV1D
    reg.register_decoder(make_dav1d_decoder_backend());
#endif

    // The NVENC / NVDEC entries are always registered — the availability
    // probe is what decides whether they are usable.
    reg.register_encoder(make_nvenc_encoder_backend());
    reg.register_decoder(make_nvdec_decoder_backend());

    // The non-NVIDIA AV1 rows (plan item B6).  Registered
    // unconditionally: the seam id and the `HwBackend` kind stay stable
    // across the tree, and the availability probe decides usability.
    reg.register_encoder(make_mediacodec_encoder_backend());
    reg.register_encoder(make_videotoolbox_encoder_backend());
    reg.register_decoder(make_videotoolbox_decoder_backend());
    reg.register_encoder(make_amf_encoder_backend());
    reg.register_encoder(make_vaapi_encoder_backend());

#if defined(NIMRTC_PLUGINS_SVT_AV1_ON)
    // The optional software AV1 encoder (plan item B11).  Registered
    // only when the vendored build is compiled in — with the option off
    // the plug-in is *absent*, not "present but unavailable".
    reg.register_encoder(make_svt_av1_encoder_backend());
#endif

    core::log::Logger::instance().info(
        "av1: registered video backends (encoder count=" +
        std::to_string(reg.list_encoders(plugins::VideoCodecKind::kAV1).size()) +
        ", decoder count=" +
        std::to_string(reg.list_decoders(plugins::VideoCodecKind::kAV1).size()) + ")");
}

// ---------------------------------------------------------------------------
// High-level convenience — used by the plugin factory to pick a
// encoder / decoder for the configured codec (or honour a
// `video_codec_name` override).
// ---------------------------------------------------------------------------

const plugins::VideoEncoderBackend*
select_encoder_backend(const plugins::VideoCodecConfig& cfg) noexcept {
    return plugins::HwVideoBackendRegistry::instance().select_encoder(
        plugins::VideoCodecKind::kAV1, cfg.name, cfg.backend_preference);
}

const plugins::VideoDecoderBackend*
select_decoder_backend(const plugins::VideoCodecConfig& cfg) noexcept {
    return plugins::HwVideoBackendRegistry::instance().select_decoder(
        plugins::VideoCodecKind::kAV1, cfg.name, cfg.backend_preference);
}

} // namespace nimrtc::av1
