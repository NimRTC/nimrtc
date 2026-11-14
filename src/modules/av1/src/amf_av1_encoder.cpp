/**
 * @file src/modules/av1/src/amf_av1_encoder.cpp
 * @brief AMD AMF AV1 encoder — the Windows AMD hardware leg.
 *
 * ## What this is
 *
 * The AV1 backend table (plan item B6) needs a row for the AMD hardware
 * encoder on Windows.  AMF's component model is codec-parameterised:
 * the same `AMFContext` / `AMFVideoEncoderUAVEncode` machinery serves
 * H.264, HEVC and AV1, and the codec is selected by the **component
 * GUID** the context is asked to create:
 *
 *   - H.264 : `AMFVideoEncoderUVD_EncodeH264_GUID`
 *   - HEVC  : `AMFVideoEncoderUVD_EncodeHEVC_GUID`
 *   - AV1   : `AMFVideoEncoderUVD_EncodeAV1_GUID` (VCN 3.0 / 4.0)
 *
 * The AV1 component is *newer* than the other two: it exists only on
 * the VCN 3.0 / 4.0 silicon (RDNA 3 / RDNA 4), and the AMF runtime
 * reports it even on silicon that cannot run it.  So the probe has to
 * create the component *and* initialise it — that is the only step that
 * reports the truth about the silicon.
 *
 * ## The honest status of this file
 *
 * **The AMF AV1 body is NOT implemented here.**  The H.264 AMF file in
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
 * including a host whose AMD GPU *does* have an AV1 encoder — which is
 * the truthful statement ("the backend is not implemented here"), not a
 * claim that the hardware is missing.
 */

#include <nimrtc/av1/hw_backends.hpp>

#include <string>

#include <nimrtc/core/log.hpp>
#include <nimrtc/plugins/video_codec.hpp>

namespace nimrtc::av1 {
namespace amf_backend {

// ---------------------------------------------------------------------------
// The probe.
//
// The real probe (once the body is implemented) is:
//
//   1. AMFInit / AMFCreateContext,
//   2. context->QueryVersion(),
//   3. context->CreateComponent(AMFVideoEncoderUVD_EncodeAV1_GUID,
//      &encoder),
//   4. the component's initialisation with an AV1 configuration — the
//      step that proves the *silicon* can run AV1.
//
// Until then the honest answer is "not implemented".
// ---------------------------------------------------------------------------

bool amf_av1_available() noexcept {
#if defined(NIMRTC_PLUGINS_AMF_ON) && defined(_WIN32)
    NIMRTC_LOG_INFO("amf: the AV1 encoder body is not implemented yet — "
                    "reporting 'not available'");
    return false;
#else
    return false;
#endif
}

/** Why the AMF AV1 encoder is unavailable.
 *
 *  The plan's B6 acceptance is "a backend that exists in the table but
 *  not in the silicon reports `available() == false` with a reason" —
 *  so the table row carries this reason next to its probe. */
std::string amf_av1_unavailable_reason() {
#if defined(NIMRTC_PLUGINS_AMF_ON) && defined(_WIN32)
    return "amf: the AMF AV1 encoder body is not implemented in this tree "
           "yet, so the AMD VCN AV1 row reports 'not available' even on a "
           "VCN 3.0 / 4.0 host whose GPU can encode AV1";
#else
    return "amf: the AMF SDK is not enabled in this build (NIMRTC_PLUGINS_AMF "
           "is off), so the AMD VCN AV1 hardware encoder is unavailable";
#endif
}

/** Create the AMF AV1 encoder.
 *
 *  Returns nullptr while the body is not implemented — which is what
 *  makes the registry fall through to the next backend (and eventually
 *  to the software stub). */
std::unique_ptr<plugins::IVideoCodec>
make_amf_av1_codec(plugins::VideoCodecConfig cfg) {
    (void)cfg;
    return nullptr;
}

} // namespace amf_backend

bool amf_av1_available() noexcept {
    return amf_backend::amf_av1_available();
}

std::string amf_av1_unavailable_reason() {
    return amf_backend::amf_av1_unavailable_reason();
}

std::unique_ptr<plugins::IVideoCodec>
make_amf_av1_codec(plugins::VideoCodecConfig cfg) {
    return amf_backend::make_amf_av1_codec(std::move(cfg));
}

} // namespace nimrtc::av1
