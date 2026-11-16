/**
 * @file src/modules/h265/src/amf_h265_encoder.cpp
 * @brief AMD AMF H.265 / HEVC encoder — the Windows AMD hardware leg.
 *
 * ## What this is
 *
 * The HEVC backend table (plan item B5) needs a row for the AMD
 * hardware encoder on Windows.  AMF's component model is
 * codec-parameterised: the same `AMFContext` /
 * `AMFVideoEncoderUAVEncode` machinery serves H.264, HEVC and AV1, and
 * the codec is selected by the **component GUID** the context is asked
 * to create:
 *
 *   - H.264 : `AMFVideoEncoderUVD_EncodeH264_GUID`
 *   - HEVC  : `AMFVideoEncoderUVD_EncodeHEVC_GUID`
 *   - AV1   : `AMFVideoEncoderUVD_EncodeAV1_GUID` (VCN 3.0 / 4.0)
 *
 * Everything else — the surface (the AMF surface is a shared texture
 * the encoder reads from), the fill / motion-estimation parameters, the
 * `AMFVideoEncoderUAVEncode` call, the bitstream query — is identical.
 *
 * ## The honest status of this file
 *
 * **The AMF HEVC body is NOT implemented here.**  The H.264 AMF file in
 * the tree (`src/modules/h264/src/amf_encoder.cpp`) was written against
 * the *superseded* plugin seam (`plugins::VideoCodecResult`,
 * `plugins::VideoCodecDirection` — types that no longer exist in
 * `nimrtc/plugins/video_codec.hpp`), so it does not compile against the
 * current `plugins::IVideoCodec` interface, and duplicating it would
 * propagate that breakage into a second file.
 *
 * So this file is an **honest seam**: the entry points exist, they have
 * the right signatures, and `available()` returns `false` with a reason.
 * The table row therefore reports "not available" on every host —
 * including a host whose AMD GPU *does* have an HEVC encoder — which is
 * the truthful statement ("the backend is not implemented here"), not a
 * claim that the hardware is missing.
 *
 * ## The seam contract
 *
 * `available()` must never crash and must never lie: it returns false
 * on a host with no AMF runtime at all, and false on a host whose
 * silicon cannot do HEVC.
 */

#include <nimrtc/h265/hw_backends.hpp>

#include <string>

#include <nimrtc/core/log.hpp>
#include <nimrtc/plugins/video_codec.hpp>

namespace nimrtc::h265 {
namespace amf_backend {

// ---------------------------------------------------------------------------
// The probe.
//
// The real probe (once the body is implemented) is:
//
//   1. AMFInit / AMFCreateContext,
//   2. context->QueryVersion(),
//   3. context->CreateComponent(AMFVideoEncoderUVD_EncodeHEVC_GUID,
//      &encoder) — the step that proves the *encoder* exists (the
//      runtime can report the AV1 / HEVC components even on silicon
//      that cannot run them),
//   4. the component's initialisation.
//
// Until then the honest answer is "not implemented".
// ---------------------------------------------------------------------------

bool amf_h265_available() noexcept {
#if defined(NIMRTC_PLUGINS_AMF_ON) && defined(_WIN32)
    NIMRTC_LOG_INFO("amf: the H.265 / HEVC encoder body is not implemented "
                    "yet — reporting 'not available'");
    return false;
#else
    return false;
#endif
}

/** Why the AMF HEVC encoder is unavailable.
 *
 *  The plan's B5 acceptance is "unsupported combinations are
 *  **explicitly reported**, not silently ignored", so the table row
 *  carries this reason next to its `available()` probe. */
std::string amf_h265_unavailable_reason() {
#if defined(NIMRTC_PLUGINS_AMF_ON) && defined(_WIN32)
    return "amf: the AMF H.265 / HEVC encoder body is not implemented in this "
           "tree yet, so the AMD VCN HEVC row reports 'not available' even on "
           "a host whose GPU can encode HEVC";
#else
    return "amf: the AMF SDK is not enabled in this build (NIMRTC_PLUGINS_AMF "
           "is off), so the AMD VCN HEVC hardware encoder is unavailable";
#endif
}

/** Create the AMF H.265 encoder.
 *
 *  Returns nullptr while the body is not implemented — which is what
 *  makes the registry fall through to the next backend (and eventually
 *  to the software stub). */
std::unique_ptr<plugins::IVideoCodec>
make_amf_h265_codec(plugins::VideoCodecConfig cfg) {
    (void)cfg;
    return nullptr;
}

} // namespace amf_backend

bool amf_h265_available() noexcept {
    return amf_backend::amf_h265_available();
}

std::string amf_h265_unavailable_reason() {
    return amf_backend::amf_h265_unavailable_reason();
}

std::unique_ptr<plugins::IVideoCodec>
make_amf_h265_codec(plugins::VideoCodecConfig cfg) {
    return amf_backend::make_amf_h265_codec(std::move(cfg));
}

} // namespace nimrtc::h265
