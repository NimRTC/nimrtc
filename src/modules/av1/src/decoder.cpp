/**
 * @file src/modules/av1/src/decoder.cpp
 * @brief AV1 stub decoder — consumes an access unit, reports its shape.
 *
 * The stub walks the Annex B OBU stream and notes whether it carries a
 * *sequence header* OBU — the AV1 random-access signal, repeated by the
 * encoder on every key frame.  It returns an empty DecodedImage; a real
 * decoder produces a fully-populated VideoFrameBuffer (the hardware path
 * is the shared NVDEC backend in the `nvcodec` module).
 */

#include <nimrtc/av1/decoder.hpp>

namespace nimrtc::av1 {

namespace {

constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

} // namespace

// ---------------------------------------------------------------------------
// BitstreamParser
// ---------------------------------------------------------------------------

bool BitstreamParser::next(ParsedObu& out) noexcept {
    if (finished_ || cursor_ >= bitstream_.size()) return false;

    // Locate the next start code (3- or 4-byte).
    std::size_t start = kNotFound;
    std::size_t sc_len = 0;
    for (std::size_t i = cursor_; i + 2 < bitstream_.size(); ++i) {
        if (bitstream_[i] != 0 || bitstream_[i + 1] != 0) continue;
        if (bitstream_[i + 2] == 1) {
            start  = i;
            sc_len = 3;
            break;
        }
        if (i + 3 < bitstream_.size()
            && bitstream_[i + 2] == 0 && bitstream_[i + 3] == 1) {
            start  = i;
            sc_len = 4;
            break;
        }
    }
    if (start == kNotFound) {
        finished_ = true;
        return false;
    }

    const std::size_t obu_start = start + sc_len;
    if (obu_start >= bitstream_.size()) {
        finished_ = true;
        return false;
    }

    // The OBU ends at the next start code (or at the end of the stream).
    std::size_t obu_end = bitstream_.size();
    for (std::size_t i = obu_start + 1; i + 2 < bitstream_.size(); ++i) {
        if (bitstream_[i] != 0 || bitstream_[i + 1] != 0) continue;
        if (bitstream_[i + 2] == 1) { obu_end = i; break; }
        if (i + 3 < bitstream_.size()
            && bitstream_[i + 2] == 0 && bitstream_[i + 3] == 1) {
            obu_end = i;
            break;
        }
    }

    out.type = video_payload::av1::peek_obu_type(
        core::ByteSpan{bitstream_.data() + obu_start, obu_end - obu_start});
    out.data = core::ByteSpan{bitstream_.data() + obu_start, obu_end - obu_start};
    out.start_code_len = sc_len;

    cursor_ = obu_end;
    if (cursor_ >= bitstream_.size()) finished_ = true;
    return true;
}

std::vector<ParsedObu> BitstreamParser::parse_all(core::ByteSpan bitstream) noexcept {
    std::vector<ParsedObu> out;
    reset(bitstream);
    ParsedObu o;
    while (next(o)) {
        out.push_back(o);
    }
    return out;
}

// ---------------------------------------------------------------------------
// StubDecoder
// ---------------------------------------------------------------------------

std::optional<DecodedImage> StubDecoder::decode(core::ByteSpan encoded,
                                                video_frame::VideoFrameInfo info) noexcept {
    if (encoded.empty()) return std::nullopt;

    DecodedImage out;
    out.capture_ts_us = info.capture_ts_us;
    out.frame_seq     = info.frame_seq;
    out.rtp_timestamp = info.rtp_timestamp;

    // The sequence header OBU is the AV1 random-access signal.
    out.is_keyframe = video_payload::av1::contains_sequence_header(encoded);

    // The stub produces no pixels — the buffer remains empty
    // (valid() == false).
    return out;
}

std::unique_ptr<Decoder> create_stub_decoder() noexcept {
    return std::make_unique<StubDecoder>();
}

} // namespace nimrtc::av1
