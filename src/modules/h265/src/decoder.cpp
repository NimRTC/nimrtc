/**
 * @file src/modules/h265/src/decoder.cpp
 * @brief H.265 stub decoder — consumes an access unit, reports its shape.
 *
 * The stub walks the Annex B bitstream, notes whether it carries an IRAP
 * (random-access) NAL unit, and returns an empty DecodedImage.  A real
 * decoder produces a fully-populated VideoFrameBuffer; the hardware path
 * is the shared NVDEC backend in the `nvcodec` module.
 */

#include <nimrtc/h265/decoder.hpp>

#include <nimrtc/core/log.hpp>

namespace nimrtc::h265 {

namespace {

constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

} // namespace

// ---------------------------------------------------------------------------
// BitstreamParser
// ---------------------------------------------------------------------------

std::size_t BitstreamParser::find_start_code_at_or_after(std::size_t from) noexcept {
    const std::size_t n = bitstream_.size();
    if (from >= n) return kNotFound;

    // Scan for a 00 00 pair; then check whether it is followed by 01
    // (three-byte start code) or 00 01 (four-byte start code).
    for (std::size_t i = from; i + 2 < n; ++i) {
        if (bitstream_[i] != 0 || bitstream_[i + 1] != 0) continue;
        if (bitstream_[i + 2] == 1) return i;
        if (i + 3 < n && bitstream_[i + 2] == 0 && bitstream_[i + 3] == 1) return i;
    }
    return kNotFound;
}

bool BitstreamParser::next(ParsedNalu& out) noexcept {
    if (finished_ || cursor_ >= bitstream_.size()) return false;

    const std::size_t nalu_start = find_start_code_at_or_after(cursor_);
    if (nalu_start == kNotFound || nalu_start >= bitstream_.size()) {
        finished_ = true;
        return false;
    }

    // Determine the start code length (3 or 4 bytes).
    std::size_t sc_len = 0;
    if (bitstream_.size() - nalu_start >= 4
        && bitstream_[nalu_start + 0] == 0
        && bitstream_[nalu_start + 1] == 0
        && bitstream_[nalu_start + 2] == 0
        && bitstream_[nalu_start + 3] == 1) {
        sc_len = 4;
    } else if (bitstream_.size() - nalu_start >= 3
               && bitstream_[nalu_start + 0] == 0
               && bitstream_[nalu_start + 1] == 0
               && bitstream_[nalu_start + 2] == 1) {
        sc_len = 3;
    } else {
        finished_ = true;
        return false;
    }

    // The NAL unit starts after the start code.
    const std::size_t nalu_hdr = nalu_start + sc_len;
    if (nalu_hdr + 2 > bitstream_.size()) {
        finished_ = true;
        return false;
    }

    // The NAL unit ends at the next start code (or at the end of the
    // stream).
    const std::size_t next_sc = find_start_code_at_or_after(nalu_hdr + 2);
    const std::size_t nalu_end =
        (next_sc == kNotFound) ? bitstream_.size() : next_sc;

    auto hdr = video_payload::h265::parse_nalu_header(
        core::ByteSpan{bitstream_.data() + nalu_hdr, nalu_end - nalu_hdr});

    out.type = hdr ? hdr->type
                    : video_payload::h265::NaluType::kUnspecified;
    out.layer_id = hdr ? hdr->layer_id : 0;
    out.tid      = hdr ? hdr->tid : 1;
    out.data     = core::ByteSpan{bitstream_.data() + nalu_hdr, nalu_end - nalu_hdr};
    out.start_code_len = sc_len;

    cursor_ = nalu_end;
    if (cursor_ >= bitstream_.size()) finished_ = true;
    return true;
}

std::vector<ParsedNalu> BitstreamParser::parse_all(core::ByteSpan bitstream) noexcept {
    std::vector<ParsedNalu> out;
    reset(bitstream);
    ParsedNalu n;
    while (next(n)) {
        out.push_back(n);
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

    // Walk the bitstream and look for an IRAP NAL unit — the H.265
    // equivalent of the H.264 IDR.
    bool found_irap = false;
    BitstreamParser parser;
    ParsedNalu n;
    parser.reset(encoded);
    while (parser.next(n)) {
        if (video_payload::h265::is_irap_nalu(n.type)) {
            found_irap = true;
        }
    }
    out.is_keyframe = found_irap;

    // The stub produces no pixels — the buffer remains empty
    // (valid() == false).
    return out;
}

std::unique_ptr<Decoder> create_stub_decoder() noexcept {
    return std::make_unique<StubDecoder>();
}

} // namespace nimrtc::h265
