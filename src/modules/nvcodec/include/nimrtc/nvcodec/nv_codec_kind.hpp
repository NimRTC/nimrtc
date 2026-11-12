/**
 * @file nimrtc/nvcodec/nv_codec_kind.hpp
 * @brief The NVIDIA codec descriptor — one place that maps a NimRTC
 *        codec kind onto the NVIDIA SDK's per-codec constants.
 *
 * ## Why this file exists
 *
 * NVENC and NVDEC are *codec-parameterised* drivers: the same session
 * code encodes H.264, HEVC and AV1, and the same parser / decoder code
 * decodes all three.  What differs per codec is a small, well-bounded
 * set of constants:
 *
 *   - the NVENC `encodeGUID`   (NV_ENC_CODEC_H264_GUID / _HEVC_ / _AV1_)
 *   - the NVENC `profileGUID`  (the profile this backend asks for)
 *   - the CUVID `CodecType`    (cudaVideoCodec_H264 / _HEVC / _AV1)
 *   - the codec kind the plugin layer sees
 *
 * Collecting them in one descriptor means the encoder and the decoder
 * cannot disagree about which codec a session is running, and it means
 * "add a codec" is a single table entry rather than a hunt through the
 * driver code.
 *
 * ## Why the descriptor lives in its own module
 *
 * The NVIDIA driver integration is shared by the H.264, H.265 and AV1
 * codec modules — all three need the *same* NVENC / NVDEC session code,
 * differing only in the constants above.  Rather than triplicate
 * ~900 lines of driver code (which would guarantee the three copies
 * drift apart), the driver integration lives here, in a small
 * lower-level module that the three codec modules link against.
 *
 * This is the same cross-module dependency shape as `modules/jb` →
 * `modules/jb_strategy`: a shared lower-level module consumed by
 * several higher-level ones, with a narrow, stable public surface.
 */

#pragma once

#include <cstdint>
#include <string_view>

#include <nimrtc/core/api_version.hpp>
#include <nimrtc/plugins/video_codec.hpp>

namespace nimrtc::nvcodec {

/** The NVIDIA codecs this backend family implements.
 *
 * `kUnsupported` is a sentinel that is never passed to the driver. */
enum class NIMRTC_API_STABLE NvCodec : std::uint8_t {
    kUnsupported = 0,
    kH264        = 1,
    kH265        = 2,
    kAv1         = 3,
};

/** Everything the NVENC / NVDEC backends need to know about a codec. */
struct NIMRTC_API_STABLE NvCodecDesc {
    /** The codec itself. */
    NvCodec codec = NvCodec::kUnsupported;

    /** The plugin-layer codec kind. */
    plugins::VideoCodecKind kind = plugins::VideoCodecKind::kUnknown;

    /** The SDP `a=rtpmap` encoding name ("H264", "H265", "AV1"). */
    std::string_view rtpmap_encoding;

    /** The plugin id / `EngineConfig::video_codec_name` ("h264", "h265", "av1"). */
    std::string_view plugin_id;

    /** A short human-readable name for logs. */
    std::string_view display_name;

    /** The default RTP payload type for this codec in WebRTC.
     *  H.264 uses 102; H.265 and AV1 are negotiated dynamically and
     *  conventionally start from 45 / 35. */
    std::uint8_t default_payload_type = 102;
};

/** The descriptor for @p kind, or a "unsupported" descriptor when the
 *  codec has no NVIDIA backend. */
NIMRTC_API_STABLE NvCodecDesc codec_desc(plugins::VideoCodecKind kind) noexcept;

/** The descriptor for a `NvCodec` value. */
NIMRTC_API_STABLE NvCodecDesc codec_desc(NvCodec codec) noexcept;

/** The `NvCodec` for a plugin-layer codec kind, or `kUnsupported`. */
NIMRTC_API_STABLE NvCodec codec_of(plugins::VideoCodecKind kind) noexcept;

} // namespace nimrtc::nvcodec
