/**
 * @file src/modules/h265/src/vaapi_h265_encoder.cpp
 * @brief VA-API H.265 / HEVC encoder — the Linux hardware leg.
 *
 * ## What this is
 *
 * The HEVC backend table (plan item B5) needs a row for the Linux
 * Intel / AMD hardware encoder.  VA-API serves HEVC through exactly the
 * same mechanism as H.264 — the same `VAEntrypointEncSlice`, the same
 * surface pool, the same `vaBeginPicture` / `vaEndPicture` /
 * `vaMapBuffer` sequence — differing only in:
 *
 *   - the **profile** (`VAProfileHEVCMain` instead of
 *     `VAProfileH264Main`),
 *   - the **sequence parameter buffer**
 *     (`VAEncSequenceParameterBufferH265` instead of
 *     `VAEncSequenceParameterBufferH264`), which carries the VPS *and*
 *     the SPS (HEVC has no separate PPS — the picture's slice header
 *     carries what H.264's PPS carries),
 *   - the **slice parameter buffer**
 *     (`VAEncSliceParameterBufferH265`), and
 *   - the **NAL unit types** on the wire (32 = VPS, 33 = SPS,
 *     19/20 = IDR), where H.264 uses 7 / 8 / 5.
 *
 * That shared mechanism is exactly why the H.264 module's
 * `vaapi_encoder.cpp` is the model for this file.
 *
 * ## The honest status of this file
 *
 * **The VA-API HEVC body is NOT implemented yet.**  The H.264 VA-API
 * encoder in `src/modules/h264/src/vaapi_encoder.cpp` was written
 * against a *superseded* plugin seam (`plugins::VideoCodecResult`,
 * `plugins::VideoCodecMode` — types that no longer exist in
 * `nimrtc/plugins/video_codec.hpp`), so it does not compile against the
 * current `plugins::IVideoCodec` interface.  Copying it verbatim would
 * propagate that breakage into a second file, and shipping an
 * unverifiable 1500-line driver integration that cannot be compiled or
 * tested on any host in this repository would be dishonest.
 *
 * So this file is an **honest seam**: the entry points exist, they have
 * the right signatures, and `available()` returns `false` with a reason.
 * The table row therefore reports "not available" on every host —
 * including a Linux host whose GPU *does* have an HEVC encoder — which
 * is the truthful statement ("the backend is not implemented here"),
 * not a claim that the hardware is missing.
 *
 * When the real body lands, it replaces the `open()` body below, the
 * probe switches to "open a DRM render node, `vaInitialize`, and query
 * `VAProfileHEVCMain` against `VAEntrypointEncSlice`", and nothing
 * outside this file changes: the priority, the `HwBackend::Vaapi` kind,
 * the seam id and the `zero_copy_supported` flag are already correct.
 *
 * ## The seam contract
 *
 * `available()` must never crash and must never lie: it returns false
 * on a host with no VA-API at all, and false on a host whose VA-API
 * cannot do HEVC.
 */

#include <nimrtc/h265/hw_backends.hpp>

#include <string>

#include <nimrtc/core/log.hpp>
#include <nimrtc/plugins/video_codec.hpp>

namespace nimrtc::h265 {
namespace vaapi_backend {

// ---------------------------------------------------------------------------
// The probe.
//
// The real probe (once the body is implemented) is:
//
//   1. open /dev/dri/renderD128 (or card0),
//   2. vaInitialize(),
//   3. vaQueryConfigProfiles(display, VAProfileHEVCMain, ...),
//   4. vaCreateConfig(display, VAEntrypointEncSlice, VAProfileHEVCMain,
//                     ...) — the step that proves the *encoder* exists
//      (a driver can decode HEVC without encoding it),
//   5. vaDestroyConfig / vaTerminate.
//
// Until then the honest answer is "not implemented".
// ---------------------------------------------------------------------------

bool vaapi_h265_available() noexcept {
#if defined(NIMRTC_PLUGINS_VAAPI_ON) && defined(__linux__)
    NIMRTC_LOG_INFO("vaapi: the H.265 / HEVC encoder body is not implemented "
                    "yet — reporting 'not available'");
    return false;
#else
    return false;
#endif
}

/** Why the VA-API HEVC encoder is unavailable.
 *
 *  The plan's B5 acceptance is "unsupported combinations are
 *  **explicitly reported**, not silently ignored", so the table row
 *  carries this reason next to its `available()` probe. */
std::string vaapi_h265_unavailable_reason() {
#if defined(NIMRTC_PLUGINS_VAAPI_ON) && defined(__linux__)
    return "vaapi: the VA-API H.265 / HEVC encoder body is not implemented in "
           "this tree yet, so the VA-API HEVC row reports 'not available' even "
           "on a Linux host whose GPU can encode HEVC";
#else
    return "vaapi: VA-API is a Linux interface and is not enabled in this "
           "build (NIMRTC_PLUGINS_VAAPI is off), so the VA-API HEVC hardware "
           "encoder is unavailable on this host";
#endif
}

/** Create the VA-API H.265 encoder.
 *
 *  Returns nullptr while the body is not implemented — which is what
 *  makes the registry fall through to the next backend (and eventually
 *  to the software stub). */
std::unique_ptr<plugins::IVideoCodec>
make_vaapi_h265_codec(plugins::VideoCodecConfig cfg) {
    (void)cfg;
    return nullptr;
}

} // namespace vaapi_backend

bool vaapi_h265_available() noexcept {
    return vaapi_backend::vaapi_h265_available();
}

std::string vaapi_h265_unavailable_reason() {
    return vaapi_backend::vaapi_h265_unavailable_reason();
}

std::unique_ptr<plugins::IVideoCodec>
make_vaapi_h265_codec(plugins::VideoCodecConfig cfg) {
    return vaapi_backend::make_vaapi_h265_codec(std::move(cfg));
}

} // namespace nimrtc::h265
