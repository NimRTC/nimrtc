/**
 * @file nimrtc/vp9/hw_backends.hpp
 * @brief VP9 backend registry — the optional software libvpx row + a stub.
 *
 * ## Why this module exists
 *
 * The remote-desktop plan's B11 asks for the *optional* software VP9
 * encoder: `libvpx` (BSD-3-Clause), vendored behind
 * `NIMRTC_VENDORED_LIBVPX`, which **defaults OFF**.
 *
 * The AV1 module is codec-specific (it owns the AV1 OBU parser and the
 * Annex-B framing the AV1 RTP payload format uses), so the VP9 codec
 * gets its own small module rather than being bolted onto the AV1 one.
 * The module mirrors the AV1 module's shape exactly:
 *
 *   - a `Decoder` interface + a stub implementation,
 *   - a `CodecPluginAdapter` that wraps the decoder behind
 *     `plugins::IVideoCodec`,
 *   - a backend table with the `available()` / priority /
 *     `zero_copy_supported` contract.
 *
 * ## The backend table
 *
 *   priority  backend            platforms                impl
 *   --------- ------------------ ------------------------ ---------------------------
 *      50    libvpx_vp9         all (vendored, optional)  src/modules/vp9/
 *        0    stub_vp9           all                       this module
 *
 * There is no hardware VP9 row: none of the hardware encoders in this
 * tree (NVENC, AMF, VA-API, QSV, VideoToolbox) exposes a VP9 encoder
 * entry point.  Reporting a "VP9 hardware row" that no silicon
 * implements would be dishonest, so the table carries only the software
 * row and the stub.
 *
 * The `libvpx_vp9` row is registered ONLY when the vendored build is
 * compiled in (`NIMRTC_PLUGINS_LIBVPX_ON`).  With the option OFF the
 * plug-in is *absent* from the registry — that is the plan's B11
 * acceptance ("with the option OFF the build still configures and the
 * plug-in is absent"), not "the plug-in is present but unavailable".
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>
#include <nimrtc/plugins/hw_video_backend.hpp>

namespace nimrtc::vp9 {

/** Register all VP9 backend entries.  Idempotent.
 *
 *  Called automatically by `register_default_plugins()` from the vp9
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

} // namespace nimrtc::vp9
