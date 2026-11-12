/**
 * @file src/modules/nvcodec/src/nv_codec_kind.cpp
 * @brief The NVENC / NVDEC codec descriptor table.
 *
 * See include/nimrtc/nvcodec/nv_codec_kind.hpp for the rationale.
 */

#include <nimrtc/nvcodec/nv_codec_kind.hpp>

namespace nimrtc::nvcodec {

namespace {

// The table.  `kUnsupported` is the sentinel for "this codec has no
// NVIDIA backend"; it is never handed to the driver.
constexpr NvCodecDesc kTable[] = {
    // codec            kind                       rtpmap   plugin id  display name             payload type
    { NvCodec::kH264, plugins::VideoCodecKind::kH264, "H264", "h264", "H.264 / AVC", 102 },
    { NvCodec::kH265, plugins::VideoCodecKind::kH265, "H265", "h265", "H.265 / HEVC",  45 },
    { NvCodec::kAv1,  plugins::VideoCodecKind::kAV1,  "AV1",  "av1",  "AV1",            35 },
};

constexpr NvCodecDesc kUnsupportedDesc{};

} // namespace

NvCodecDesc codec_desc(plugins::VideoCodecKind kind) noexcept {
    for (const auto& d : kTable) {
        if (d.kind == kind) return d;
    }
    return kUnsupportedDesc;
}

NvCodecDesc codec_desc(NvCodec codec) noexcept {
    for (const auto& d : kTable) {
        if (d.codec == codec) return d;
    }
    return kUnsupportedDesc;
}

NvCodec codec_of(plugins::VideoCodecKind kind) noexcept {
    for (const auto& d : kTable) {
        if (d.kind == kind) return d.codec;
    }
    return NvCodec::kUnsupported;
}

} // namespace nimrtc::nvcodec
