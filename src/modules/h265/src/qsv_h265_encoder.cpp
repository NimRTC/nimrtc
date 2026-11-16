/**
 * @file src/modules/h265/src/qsv_h265_encoder.cpp
 * @brief Intel Quick Sync (oneVPL / libvpl) H.265 / HEVC encoder.
 *
 * ## What this is
 *
 * The HEVC backend table (plan item B5) needs a row for the Intel
 * hardware encoder on Windows and Linux.  The MFX / oneVPL session code
 * is *codec-parameterised*: the same `MFXVideoENCODE_Init` /
 * `MFXVideoENCODE_FrameEncode` path serves AVC and HEVC, and the only
 * differences are:
 *
 *   - the **codec id** (`MFX_CODEC_HEVC` instead of `MFX_CODEC_AVC`),
 *   - the **profile** (`MFX_PROFILE_HEVC_MAIN` instead of
 *     `MFX_PROFILE_AVC_BASELINE`), and
 *   - the **bitstream framing**: oneVPL emits *length-prefixed* NAL
 *     units, so the output has to be converted to Annex-B start codes
 *     before the RTP packetizer can split it — the same conversion the
 *     H.264 QSV path already performs.
 *
 * The H.264 QSV path (`src/modules/h264/src/qsv_encoder.cpp`) is the
 * model for the probe's shape.
 *
 * ## The honest status of this file
 *
 * **The oneVPL HEVC body is NOT implemented here.**  The H.264 QSV file
 * in the tree was written against the *superseded* plugin seam
 * (`plugins::VideoCodecResult`, `plugins::VideoCodecMode` — types that
 * no longer exist in `nimrtc/plugins/video_codec.hpp`), so it does not
 * compile against the current `plugins::IVideoCodec` interface, and
 * duplicating it would propagate that breakage into a second file.
 *
 * So this file is an **honest seam**: the entry points exist, they have
 * the right signatures, and `available()` returns `false` with a reason.
 * The table row therefore reports "not available" on every host —
 * including a host whose Intel iGPU *does* have an HEVC encoder —
 * which is the truthful statement ("the backend is not implemented
 * here"), not a claim that the hardware is missing.
 *
 * ## The seam contract
 *
 * `available()` must never crash and must never lie: it returns false
 * on a host with no oneVPL at all, and false on a host whose oneVPL
 * cannot do HEVC.
 */

#include <nimrtc/h265/hw_backends.hpp>

#include <string>

#include <nimrtc/core/log.hpp>
#include <nimrtc/plugins/video_codec.hpp>

namespace nimrtc::h265 {
namespace qsv_backend {

// ---------------------------------------------------------------------------
// The probe.
//
// The real probe (once the body is implemented) is:
//
//   1. MFXInit(MFX_IMPL_TYPE_HARDWARE, ...) — then MFX_IMPL_TYPE_ANY,
//   2. MFXVideoENCODE_Query with CodecId = MFX_CODEC_HEVC — the step
//      that proves the *encoder* exists (oneVPL can decode HEVC
//      without encoding it),
//   3. MFXClose.
//
// Until then the honest answer is "not implemented".
// ---------------------------------------------------------------------------

bool qsv_h265_available() noexcept {
#if defined(NIMRTC_PLUGINS_QSV_ON)
    NIMRTC_LOG_INFO("qsv: the H.265 / HEVC encoder body is not implemented "
                    "yet — reporting 'not available'");
    return false;
#else
    return false;
#endif
}

/** Why the Quick Sync HEVC encoder is unavailable.
 *
 *  The plan's B5 acceptance is "unsupported combinations are
 *  **explicitly reported**, not silently ignored", so the table row
 *  carries this reason next to its `available()` probe. */
std::string qsv_h265_unavailable_reason() {
#if defined(NIMRTC_PLUGINS_QSV_ON)
    return "quicksync: the oneVPL / libvpl H.265 / HEVC encoder body is not "
           "implemented in this tree yet, so the Intel Quick Sync HEVC row "
           "reports 'not available' even on a host whose iGPU can encode "
           "HEVC";
#else
    return "quicksync: the oneVPL / libvpl SDK is not enabled in this build "
           "(NIMRTC_PLUGINS_QSV is off), so the Intel Quick Sync HEVC "
           "hardware encoder is unavailable";
#endif
}

/** Create the oneVPL H.265 encoder.
 *
 *  Returns nullptr while the body is not implemented — which is what
 *  makes the registry fall through to the next backend (and eventually
 *  to the software stub). */
std::unique_ptr<plugins::IVideoCodec>
make_qsv_h265_codec(plugins::VideoCodecConfig cfg) {
    (void)cfg;
    return nullptr;
}

} // namespace qsv_backend

bool qsv_h265_available() noexcept {
    return qsv_backend::qsv_h265_available();
}

std::string qsv_h265_unavailable_reason() {
    return qsv_backend::qsv_h265_unavailable_reason();
}

std::unique_ptr<plugins::IVideoCodec>
make_qsv_h265_codec(plugins::VideoCodecConfig cfg) {
    return qsv_backend::make_qsv_h265_codec(std::move(cfg));
}

} // namespace nimrtc::h265
