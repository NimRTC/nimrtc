/**
 * @file src/modules/h264/src/decoder.cpp
 * @brief H.264 stub decoder — consumes NAL units, produces empty VideoFrame.
 *
 * This decoder:
 *   - Validates that the input contains at least one IDR slice (for
 *     keyframe assertions in tests).
 *   - Reports per-call stats.
 *   - Returns an empty DecodedImage (no pixels).  Real decoders produce
 *     a fully-populated VideoFrameBuffer.
 *
 * The stub exists so the full pipeline (RTP → payload → JB → decoder →
 * callback) can be exercised without linking a proprietary H.264 decoder.
 */

#include <nimrtc/h264/decoder.hpp>
#include <nimrtc/h264/bitstream.hpp>

#include <nimrtc/core/log.hpp>

namespace nimrtc::h264 {

std::optional<DecodedImage> StubDecoder::decode(core::ByteSpan encoded,
                                               video_frame::VideoFrameInfo info) noexcept {
    if (encoded.empty()) return std::nullopt;

    DecodedImage out;
    out.capture_ts_us = info.capture_ts_us;
    out.frame_seq     = info.frame_seq;
    out.rtp_timestamp = info.rtp_timestamp;

    // Walk the bitstream to find width / height / keyframe flag.
    BitstreamParser parser;
    ParsedNalu n;
    parser.reset(encoded);

    bool found_idr = false;
    while (parser.next(n)) {
        if (n.type == video_payload::h264::NaluType::kSliceIDR) {
            found_idr = true;
        } else if (n.type == video_payload::h264::NaluType::kSPS) {
            auto summary = parse_sps(n.data);
            if (summary.valid) {
                out.width  = summary.width;
                out.height = summary.height;
            }
        }
    }
    out.is_keyframe = found_idr;

    // Stub produces no pixels — buffer remains empty (valid() == false).
    return out;
}

std::unique_ptr<Decoder> create_stub_decoder() noexcept {
    return std::make_unique<StubDecoder>();
}

} // namespace nimrtc::h264
