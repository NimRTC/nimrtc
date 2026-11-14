/**
 * @file nimrtc/nvcodec/vt_backends.hpp
 * @brief The Apple VideoToolbox encode / decode entry points.
 *
 * ## Why this header exists
 *
 * The NVIDIA driver integration lives in this module (see
 * `src/nvenc_encoder.cpp` and `src/nvdec_decoder.cpp`) because it is
 * *not* H.264-specific: the same session code serves H.264, H.265 and
 * AV1, differing only in a handful of constants.  The same is true of
 * Apple's VideoToolbox: one `VTCompressionSession` code path serves
 * H.264 and HEVC, and one `VTDecompressionSession` code path serves
 * both, differing only in the `CMVideoCodecType` and in the parameter
 * sets the format description is built from.
 *
 * Rather than triplicate the session code across the codec modules,
 * the VideoToolbox integration is **parameterised by codec**, and this
 * header is the public surface of that parameterisation — the exact
 * shape of `nv_backends.hpp`.
 *
 * ## Availability semantics
 *
 * `vt_encoder_available()` is an *honest* probe: it creates a real
 * `VTCompressionSession` for the requested codec and tears it down
 * again.  It does **not** answer "the OS is new enough" — a Mac whose
 * silicon has no HEVC encoder (or whose OS is too old for the
 * required `VTCompressionOutputSettings`) must report "not available".
 *
 * Both functions are always present (they are *not* compiled out when
 * the Apple SDK is absent) — they simply return `false` / `nullptr` in
 * that case.  The callers therefore need no `#if` guards, and a build
 * on Windows behaves exactly like a build on a Mac whose hardware
 * cannot do that codec.
 *
 * ## The zero-copy contract
 *
 * VideoToolbox consumes and produces `CVPixelBufferRef`, which the
 * tree's `GpuBuffer` / `GpuHandle` carry as
 * `plugins::GpuBackend::kAppleCFPixelBuffer`.  The encoder therefore
 * supports the zero-copy encode path: a `CVPixelBuffer` handed to
 * `encode_zero_copy()` is submitted to the compression session
 * as-is, without a CPU staging copy.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>
#include <cstdint>
#include <memory>
#include <string>

#include <nimrtc/plugins/video_codec.hpp>

namespace nimrtc::nvcodec {

/** The codec families Apple's VideoToolbox serves.
 *
 *  VideoToolbox does H.264 and HEVC on every macOS release the project
 *  supports, and AV1 *decode* on macOS 14 (Sonoma) and newer — but only
 *  on M3-and-newer silicon, and no Apple chip has an AV1 hardware
 *  *encoder* at all.  The AV1 entry is present so the *table row* exists
 *  on every platform, and the probe reports the OS-version floor (or the
 *  failed session creation) as the reason when the host cannot serve
 *  it. */
enum class NIMRTC_API_STABLE VtCodec : std::uint8_t {
    kH264 = 0,
    kH265 = 1,
    kAv1  = 2,
};

/** True iff this host can encode @p codec with VideoToolbox.
 *
 *  The probe creates and immediately tears down a real compression
 *  session, so it reports the truth about the *hardware*, not about
 *  the OS version.  Returns false on every non-Apple build. */
NIMRTC_API_STABLE bool vt_encoder_available(VtCodec codec) noexcept;

/** Create a VideoToolbox encoder for @p codec.
 *
 *  Returns nullptr when the codec is unavailable on this host or when
 *  the session fails to open.  The returned instance is *not* opened —
 *  the caller owns that step (the plugin factory contract is
 *  "created, then opened"). */
NIMRTC_API_STABLE std::unique_ptr<plugins::IVideoCodec>
make_vt_encoder(VtCodec codec, plugins::VideoCodecConfig cfg);

/** True iff this host can decode @p codec with VideoToolbox. */
NIMRTC_API_STABLE bool vt_decoder_available(VtCodec codec) noexcept;

/** Create a VideoToolbox decoder for @p codec.
 *
 *  Returns nullptr when the codec is unavailable on this host or when
 *  the session fails to open. */
NIMRTC_API_STABLE std::unique_ptr<plugins::IVideoCodec>
make_vt_decoder(VtCodec codec, plugins::VideoCodecConfig cfg);

/** Why the VideoToolbox *encoder* for @p codec is unavailable.
 *
 *  The remote-desktop plan's B5 / B6 acceptance is "unsupported
 *  combinations are **explicitly reported**, not silently ignored", so
 *  every VideoToolbox table row carries this reason alongside its
 *  `available()` probe.
 *
 *  The string is never empty:
 *
 *   - on a non-Apple build it names the missing framework,
 *   - on an Apple build whose OS is older than the codec's floor it
 *     names the required release and the running one,
 *   - on an Apple build whose OS is new enough but whose silicon has
 *     no such encoder it names the failed session creation.
 *
 *  The function is safe to call on any platform and always returns a
 *  non-empty string. */
NIMRTC_API_STABLE std::string vt_encoder_unavailable_reason(VtCodec codec);

/** Why the VideoToolbox *decoder* for @p codec is unavailable.
 *
 *  The mirror of `vt_encoder_unavailable_reason()` — same guarantees,
 *  same non-empty contract. */
NIMRTC_API_STABLE std::string vt_decoder_unavailable_reason(VtCodec codec);

} // namespace nimrtc::nvcodec
