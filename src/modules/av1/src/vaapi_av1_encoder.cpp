/**
 * @file src/modules/av1/src/vaapi_av1_encoder.cpp
 * @brief VA-API AV1 encoder — the Linux hardware leg.
 *
 * ## What this is
 *
 * The AV1 backend table (plan item B6) needs a row for the Linux Intel /
 * AMD hardware encoder.  VA-API serves AV1 through the same mechanism
 * as H.264 and HEVC — the same `VAEntrypointEncSlice`, the same surface
 * pool, the same `vaBeginPicture` / `vaEndPicture` / `vaMapBuffer`
 * sequence — differing only in:
 *
 *   - the **profile** (`VAProfileAV1`),
 *   - the **sequence parameter buffer**
 *     (`VAEncSequenceParameterBufferAV1`), which carries the AV1
 *     sequence header OBU,
 *   - the **picture parameter buffer**
 *     (`VAEncPictureParameterBufferAV1`), and
 *   - the **bitstream framing**: the coded picture is a sequence of
 *     OBUs, which the RTP packetizer already knows how to split (the
 *     NVENC AV1 path emits Annex-B OBU framing too).
 *
 * ## The honest status of this file
 *
 * **The VA-API AV1 body is NOT implemented here.**  The H.264 VA-API
 * encoder in `src/modules/h264/src/vaapi_encoder.cpp` was written
 * against a *superseded* plugin seam (`plugins::VideoCodecResult`,
 * `plugins::VideoCodecMode` — types that no longer exist in
 * `nimrtc/plugins/video_codec.hpp`), so it does not compile against the
 * current `plugins::IVideoCodec` interface, and duplicating it would
 * propagate that breakage into a second file.
 *
 * So this file is an **honest seam**: the entry points exist, they have
 * the right signatures, and `available()` returns `false` with a reason.
 * The table row therefore reports "not available" on every host —
 * including a Linux host whose GPU *does* have an AV1 encoder — which
 * is the truthful statement ("the backend is not implemented here"),
 * not a claim that the hardware is missing.
 *
 * Note that VA-API's AV1 *encoder* support is newer and narrower than
 * its HEVC support, so the probe has to query the profile rather than
 * assume it.
 */

#include <nimrtc/av1/hw_backends.hpp>

#include <string>

#include <nimrtc/core/log.hpp>
#include <nimrtc/plugins/video_codec.hpp>

namespace nimrtc::av1 {
namespace vaapi_backend {

// ---------------------------------------------------------------------------
// The probe.
//
// The real probe (once the body is implemented) is:
//
//   1. open /dev/dri/renderD128 (or card0),
//   2. vaInitialize(),
//   3. vaQueryConfigProfiles(display, VAProfileAV1, ...),
//   4. vaCreateConfig(display, VAEntrypointEncSlice, VAProfileAV1,
//                     ...) — the step that proves the *encoder* exists,
//   5. vaDestroyConfig / vaTerminate.
//
// Until then the honest answer is "not implemented".
// ---------------------------------------------------------------------------

bool vaapi_av1_available() noexcept {
#if defined(NIMRTC_PLUGINS_VAAPI_ON) && defined(__linux__)
    NIMRTC_LOG_INFO("vaapi: the AV1 encoder body is not implemented yet — "
                    "reporting 'not available'");
    return false;
#else
    return false;
#endif
}

/** Why the VA-API AV1 encoder is unavailable.
 *
 *  VA-API's AV1 *encoder* support is newer and narrower than its HEVC
 *  support, so the reason has to distinguish "no VA-API at all" from
 *  "VA-API present but this driver exposes no AV1 encoder". */
std::string vaapi_av1_unavailable_reason() {
#if defined(NIMRTC_PLUGINS_VAAPI_ON) && defined(__linux__)
    return "vaapi: the VA-API AV1 encoder body is not implemented in this "
           "tree yet, so the Linux VA-API AV1 row reports 'not available' even "
           "on a host whose driver exposes an AV1 encoder";
#else
    return "vaapi: the VA-API backend is not enabled in this build "
           "(NIMRTC_PLUGINS_VAAPI is off), so the Linux VA-API AV1 hardware "
           "encoder is unavailable";
#endif
}

/** Create the VA-API AV1 encoder.
 *
 *  Returns nullptr while the body is not implemented — which is what
 *  makes the registry fall through to the next backend (and eventually
 *  to the software stub). */
std::unique_ptr<plugins::IVideoCodec>
make_vaapi_av1_codec(plugins::VideoCodecConfig cfg) {
    (void)cfg;
    return nullptr;
}

} // namespace vaapi_backend

bool vaapi_av1_available() noexcept {
    return vaapi_backend::vaapi_av1_available();
}

std::string vaapi_av1_unavailable_reason() {
    return vaapi_backend::vaapi_av1_unavailable_reason();
}

std::unique_ptr<plugins::IVideoCodec>
make_vaapi_av1_codec(plugins::VideoCodecConfig cfg) {
    return vaapi_backend::make_vaapi_av1_codec(std::move(cfg));
}

} // namespace nimrtc::av1
