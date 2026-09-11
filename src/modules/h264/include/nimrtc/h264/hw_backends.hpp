/**
 * @file nimrtc/h264/hw_backends.hpp
 * @brief H.264 backend registry — platform HW encoders first, software last.
 *
 * Mirrors Sunshine's `video.cpp` backend table:
 *
 *   priority  backend        platforms                impl plugin
 *   ───────── ────────────── ─────────────────────── ──────────────────
 *      450    nvenc_h264     Win+Linux NVIDIA         src/plugins/nvenc/
 *      420    mediacodec_h264 Android                  src/plugins/mediacodec/
 *      400    videotoolbox_h264 macOS/iOS              src/plugins/videotoolbox/
 *      380    amf_h264       Windows AMD              src/plugins/amf/
 *      360    vaapi_h264      Linux Intel/AMD          src/plugins/vaapi/
 *      340    quicksync_h264  Windows Intel            src/plugins/qsv/
 *      320    dxva_h264       Windows fallback         src/plugins/dxva/
 *       80    ffmpeg_h264     all                      src/plugins/ffmpeg/
 *       50    openh264_h264   all                      src/plugins/openh264/
 *       10    stub_h264       all                      this module
 *
 * `available()` for each entry is implemented in hw_backends.cpp and depends
 * on compile-time switches (`NIMRTC_PLUGINS_NVENC_ON` etc.) plus runtime
 * probes (driver loaded, HW present, profile supported).
 *
 * `create()` returns either:
 *   - A real IVideoCodec derived from a plugin implementation
 *   - The existing `Stub*Encoder` / `Stub*Decoder` from this module (always
 *     available) so `select_*` is never null.
 *
 * Selection semantics (in HwVideoBackendRegistry):
 *   1. cfg.video_codec_name matches → use that backend (if available)
 *   2. Else highest-priority available backend wins
 *   3. Else ANY backend for the kind (caller will surface the failure)
 */

#pragma once

#include <nimrtc/plugins/hw_video_backend.hpp>

namespace nimrtc::h264 {

/// Register all H.264 backend entries declared in hw_backends.cpp. Idempotent.
/// Called automatically by `register_default_plugins()` from the h264
/// module. Safe to invoke multiple times (re-registration is no-op).
void register_default_video_backends() noexcept;

/// Select encoder honouring `preferred_id` and selection policy. Returns
/// a backend descriptor that is non-null. If `create()` ultimately fails,
/// the caller will see a clear error message identifying the backend.
const plugins::VideoEncoderBackend*
select_encoder_backend(const plugins::VideoCodecConfig& cfg) noexcept;

/// Same for decoders.
const plugins::VideoDecoderBackend*
select_decoder_backend(const plugins::VideoCodecConfig& cfg) noexcept;

} // namespace nimrtc::h264
