/**
 * @file nimrtc/av1/hw_backends.hpp
 * @brief AV1 backend registry — platform HW encoders first, software last.
 *
 * Mirrors the H.264 module's backend table (Sunshine's `video.cpp`
 * backend table), restricted to the AV1 codec family:
 *
 *   priority  backend            platforms                impl
 *   --------- ------------------ ------------------------ ---------------------------
 *      450    nvenc_av1          Win+Linux NVIDIA         src/modules/nvcodec/
 *      430    nvdec_av1          Win+Linux NVIDIA         src/modules/nvcodec/
 *      420    mediacodec_av1     Android                  src/modules/av1/
 *      400    videotoolbox_av1   macOS 14+                src/modules/nvcodec/
 *      380    amf_av1            Windows AMD (VCN 3/4)    src/modules/av1/
 *      360    vaapi_av1          Linux Intel/AMD          src/modules/av1/
 *       50    dav1d_av1          all (decode only)        src/modules/nvcodec/ (vendored)
 *        0    stub_av1           all                      this module
 *
 * The NVIDIA entries are *always* registered: their `available()` probe
 * initialises a real NVENC / NVDEC session for the AV1 codec, so a GPU
 * without an AV1 encoder reports "not available" rather than "yes,
 * because NVENC exists".
 *
 * The non-NVIDIA rows are registered on *every* platform too — the seam
 * id and the `HwBackend` kind stay stable across the tree, and the
 * availability probe is what decides whether a row is usable.  A row
 * that exists in the table but not in the silicon reports
 * `available() == false` **with a reason** (`unavailable_reason()`),
 * so an unsupported combination is explicitly reported rather than
 * silently ignored (remote-desktop plan B6).
 *
 * ## Selection semantics
 *
 * `select_encoder_backend()` / `select_decoder_backend()` honour
 * `cfg.name` as an explicit preference and otherwise pick the
 * highest-priority available backend for the AV1 codec family.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>
#include <nimrtc/plugins/hw_video_backend.hpp>

namespace nimrtc::av1 {

/** Register all AV1 backend entries.  Idempotent.
 *
 *  Called automatically by `register_default_plugins()` from the av1
 *  module.  Safe to invoke multiple times. */
NIMRTC_API_STABLE void register_default_video_backends() noexcept;

/** Select an encoder honouring `cfg.name` and the selection policy.
 *  Returns a backend descriptor that is non-null as long as the stub
 *  backend is registered. */
NIMRTC_API_STABLE const plugins::VideoEncoderBackend*
select_encoder_backend(const plugins::VideoCodecConfig& cfg) noexcept;

/** Same for decoders. */
NIMRTC_API_STABLE const plugins::VideoDecoderBackend*
select_decoder_backend(const plugins::VideoCodecConfig& cfg) noexcept;

} // namespace nimrtc::av1
