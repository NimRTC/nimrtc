/**
 * @file nimrtc/h264/bitstream.hpp
 * @brief H.264 Annex B bitstream parser.
 *
 * Per ARCHITECTURE.md §6 + §11.5: NimRTC does NOT bundle a decoder; the
 * bitstream parser here is just a structural helper for:
 *   - Walking Annex B (start-code-prefixed) bitstreams
 *   - Extracting individual NAL units
 *   - Reading parameter sets (SPS / PPS) for SDP answerer / decoder init
 *
 * The actual encoding / decoding is pluggable (see codec_plugin.hpp).
 *
 * ## Annex B format
 *
 *   00 00 00 01 [nalu_bytes] 00 00 00 01 [nalu_bytes] ...
 *
 * Three- and four-byte start codes are both supported; four-byte is
 * preferred for byte-aligned streams.  We DO NOT support MP4-style
 * length-prefixed framing here — that's handled by the RTP payload layer
 * (modules/video_payload).
 *
 * ## RBSP / EBSP
 *
 * H.264 uses "emulation prevention" bytes: 0x00 0x00 0x03 in the bitstream
 * represents the byte sequence 0x00 0x00 in the actual RBSP.  This parser
 * does NOT un-do emulation prevention — that's the decoder's job.  We just
 * locate NAL units.
 *
 * @note P1. Standalone module; not yet wired into the engine.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/video_payload/h264.hpp>

namespace nimrtc::h264 {

// ---------------------------------------------------------------------------
// H.264 start codes
// ---------------------------------------------------------------------------

constexpr std::size_t kStartCode3 = 3;   // 00 00 01
constexpr std::size_t kStartCode4 = 4;   // 00 00 00 01

/** True iff the buffer at @p data starts with an Annex B start code. */
inline bool is_start_code(core::ByteSpan data) noexcept {
    if (data.size() >= 4 && data[0] == 0 && data[1] == 0
                         && data[2] == 0 && data[3] == 1) return true;
    if (data.size() >= 3 && data[0] == 0 && data[1] == 0
                         && data[2] == 1) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Parsed NAL unit
// ---------------------------------------------------------------------------

/** A single NAL unit extracted from an Annex B bitstream.
 *  `data` references the parser's input buffer — caller must keep it alive. */
struct ParsedNalu {
    nimrtc::video_payload::h264::NaluType type =
        nimrtc::video_payload::h264::NaluType::kUnspecified;

    /** Raw NAL unit bytes INCLUDING the 1-byte NAL header, but NOT the
     *  start code prefix. */
    core::ByteSpan data;

    /** Length of the start code that preceded this NAL unit (3 or 4). */
    std::size_t   start_code_len = 0;
};

// ---------------------------------------------------------------------------
// BitstreamParser — stateful iterator over an Annex B bitstream
// ---------------------------------------------------------------------------

class BitstreamParser {
public:
    BitstreamParser() noexcept = default;

    /** Begin parsing a new bitstream. */
    void reset(core::ByteSpan bitstream) noexcept {
        bitstream_  = bitstream;
        cursor_     = 0;
        finished_   = false;
    }

    /** Pull the next NAL unit.  Returns false when the stream is exhausted. */
    bool next(ParsedNalu& out) noexcept {
        if (finished_ || cursor_ >= bitstream_.size()) return false;

        // Find the next start code (or end of stream).
        // The sentinel value returned by find_start_code_at_or_after when no
        // start code is found is kNotFound (max size_t) — not the size of
        // an empty span (0). The original code compared against
        // core::ByteSpan{}.size() (= 0) which incorrectly matched offset 0
        // (the very first start code in the stream) and made the parser
        // bail out immediately.
        constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);
        std::size_t nalu_start = find_start_code_at_or_after(cursor_);
        if (nalu_start == kNotFound || nalu_start >= bitstream_.size()) {
            finished_ = true;
            return false;
        }

        // Determine start code length.
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

        // NAL unit starts after the start code.
        std::size_t nalu_hdr = nalu_start + sc_len;
        if (nalu_hdr >= bitstream_.size()) {
            finished_ = true;
            return false;
        }

        // Find the next start code (or end of stream) — this is the NAL unit's end.
        std::size_t next_sc = find_start_code_at_or_after(nalu_hdr + 1);
        std::size_t nalu_end = (next_sc == kNotFound || next_sc >= bitstream_.size())
                               ? bitstream_.size() : next_sc;

        out.type          = static_cast<video_payload::h264::NaluType>(
                                bitstream_[nalu_hdr] & 0x1F);
        out.data          = core::ByteSpan{
                                bitstream_.data() + nalu_hdr,
                                nalu_end - nalu_hdr};
        out.start_code_len = sc_len;

        cursor_ = nalu_end;
        if (cursor_ >= bitstream_.size()) finished_ = true;
        return true;
    }

    /** True iff the iterator is exhausted. */
    [[nodiscard]] bool finished() const noexcept { return finished_; }

    /** Pull all NAL units into a vector. */
    std::vector<ParsedNalu> parse_all(core::ByteSpan bitstream) noexcept {
        std::vector<ParsedNalu> out;
        reset(bitstream);
        ParsedNalu n;
        while (next(n)) {
            out.push_back(n);
        }
        return out;
    }

private:
    /** Find the offset of the next start code at or after @p from.
     *  Returns `kNotFound` (the maximum representable size_t) if not
     *  found. We use a value distinct from any legal offset so callers
     *  can safely distinguish "not found" from "found at the very end". */
    std::size_t find_start_code_at_or_after(std::size_t from) noexcept {
        constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);
        const std::size_t n = bitstream_.size();
        if (from >= n) return kNotFound;

        // Scan for 00 00 pattern; once found, check for 01.
        for (std::size_t i = from; i + 2 < n; ++i) {
            if (bitstream_[i] == 0 && bitstream_[i + 1] == 0) {
                if (i + 2 < n && bitstream_[i + 2] == 1) return i;
                if (i + 3 < n && bitstream_[i + 2] == 0
                                && bitstream_[i + 3] == 1) return i;
            }
        }
        return kNotFound;     // not found
    }

    core::ByteSpan bitstream_{};
    std::size_t    cursor_   = 0;
    bool           finished_ = false;
};

// ---------------------------------------------------------------------------
// SPS / PPS extraction helpers
// ---------------------------------------------------------------------------

/** Extract the first SPS NAL unit from an Annex B bitstream.
 *  Returns std::nullopt if no SPS is found. */
std::optional<ParsedNalu> find_sps(core::ByteSpan bitstream) noexcept;

/** Extract the first PPS NAL unit from an Annex B bitstream.
 *  Returns std::nullopt if no PPS is found. */
std::optional<ParsedNalu> find_pps(core::ByteSpan bitstream) noexcept;

/** Find the first IDR slice (key frame) in an Annex B bitstream. */
std::optional<ParsedNalu> find_idr_slice(core::ByteSpan bitstream) noexcept;

/** Build an Annex B bitstream from a list of NAL units (start-code-prefixed).
 *  Output buffer must be at least 4 bytes per NALU + sum of NALU sizes. */
std::size_t build_annex_b(core::ByteSpan* nalu_datas,
                          std::size_t nalu_count,
                          std::uint8_t* output,
                          std::size_t output_capacity) noexcept;

} // namespace nimrtc::h264
