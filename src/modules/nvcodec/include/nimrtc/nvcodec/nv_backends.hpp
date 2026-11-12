/**
 * @file nimrtc/nvcodec/nv_backends.hpp
 * @brief The codec-generic NVENC / NVDEC entry points.
 *
 * ## Why this header exists
 *
 * The NVIDIA encode / decode driver code lives in this module (see
 * `src/nvenc_encoder.cpp` and `src/nvdec_decoder.cpp` — they are added
 * to the `nimrtc_nvcodec` target by the `nimrtc_link_nvenc()` /
 * `nimrtc_link_nvdec()` CMake helpers).
 *
 * That code is *not* H.264-specific: NVENC and NVDEC serve H.264,
 * H.265 and AV1 through the same session code, differing only in a
 * handful of constants (the `encodeGUID`, the profile GUID and the
 * CUVID codec type).  Rather than triplicate ~900 lines of driver
 * code — which would guarantee the three copies drift apart — the
 * backends are **parameterised by codec**, and this header is the
 * public surface of that parameterisation.
 *
 * The H.264, H.265 and AV1 codec modules therefore *call into* this
 * module's NV backends.  This is a documented cross-module dependency,
 * in the same shape as `modules/jb` → `modules/jb_strategy`: this
 * module owns the NVIDIA driver integration, and the sibling codec
 * modules consume it through a narrow, stable seam.
 *
 * ## Availability semantics
 *
 * `nvenc_codec_available()` and `nvdec_codec_available()` are honest
 * probes: they return true only when the codec can *actually* be
 * encoded / decoded on this host.  A GPU may have a working NVENC
 * session and still have no HEVC encoder (or no AV1 encoder at all) —
 * the probe initializes a real encoder session for the codec in
 * question, so it reports the truth rather than guessing from the
 * device name.
 *
 * Both functions are always present (they are *not* compiled out when
 * the NVIDIA SDK is absent) — they simply return `false` / `nullptr`
 * in that case.  The callers therefore need no `#if` guards, and a
 * build without the SDK behaves exactly like a build on a host whose
 * GPU cannot do that codec.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>
#include <memory>

#include <nimrtc/nvcodec/nv_codec_kind.hpp>
#include <nimrtc/plugins/video_codec.hpp>

namespace nimrtc::nvcodec {

/** True iff this host can encode @p codec with NVENC.
 *
 *  Returns false when the NVIDIA driver is absent, when the GPU has no
 *  usable NVENC session, or when the silicon has no hardware encoder
 *  for that codec family. */
NIMRTC_API_STABLE bool nvenc_codec_available(NvCodec codec) noexcept;

/** Create an NVENC encoder for @p codec.
 *
 *  Returns nullptr when the codec is unavailable on this host or when
 *  the encoder session fails to open.  The returned instance is
 *  *not* opened — the caller owns that step (the plugin factory
 *  contract is "created, then opened"). */
NIMRTC_API_STABLE std::unique_ptr<plugins::IVideoCodec>
make_nvenc_codec(NvCodec codec, plugins::VideoCodecConfig cfg);

/** True iff this host can decode @p codec with NVDEC. */
NIMRTC_API_STABLE bool nvdec_codec_available(NvCodec codec) noexcept;

/** Create an NVDEC decoder for @p codec.
 *
 *  Returns nullptr when the codec is unavailable on this host or when
 *  the decoder session fails to open. */
NIMRTC_API_STABLE std::unique_ptr<plugins::IVideoCodec>
make_nvdec_codec(NvCodec codec, plugins::VideoCodecConfig cfg);

} // namespace nimrtc::nvcodec
