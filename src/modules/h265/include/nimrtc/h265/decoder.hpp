/**
 * @file nimrtc/h265/decoder.hpp
 * @brief H.265 / HEVC decoder interface + stub implementation.
 *
 * ## Why a stub
 *
 * NimRTC does not bundle a software HEVC decoder (x265 is GPL-2.0 and
 * excluded by the project's licensing policy; see
 * `docs/plan/remote-desktop-plan.md` RD-2).  What this module ships:
 *
 *   - The `Decoder` interface — what a host plugin (or the shared NVIDIA
 *     backend in the `nvcodec` module) must implement.
 *   - A no-op `StubDecoder` that consumes an H.265 access unit and
 *     reports its key-frame-ness without producing pixels.
 *
 * The stub exists so the full pipeline (RTP → payload → JB → decoder →
 * callback) can be exercised for H.265 without linking a proprietary
 * codec, exactly like the H.264 module's stub.
 *
 * ## Access unit framing
 *
 * The decoder consumes an **Annex B** bitstream: a sequence of NAL units,
 * each preceded by a 3- or 4-byte start code.  Each NAL unit starts with
 * the two-byte NAL unit header of RFC 7798 §1.1.4:
 *
 *      byte 0: F(1) | Type(6) | LayerId MSB(1)
 *      byte 1: LayerId low 5 bits(5) | TID(3)
 *
 * A random-access point is an **IRAP** NAL unit (types 16..21: BLA, IDR
 * and CRA pictures); the parameter sets (VPS = 32, SPS = 33, PPS = 34)
 * precede it.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/plugins/base.hpp>
#include <nimrtc/video_frame/frame.hpp>
#include <nimrtc/video_payload/h265.hpp>

namespace nimrtc::h265 {

// ---------------------------------------------------------------------------
// One NAL unit extracted from an Annex B bitstream
// ---------------------------------------------------------------------------

/** A single NAL unit extracted from an Annex B bitstream.
 *
 *  `data` references the parser's input buffer — the caller must keep it
 *  alive for as long as the span is used. */
struct NIMRTC_API_STABLE ParsedNalu {
    /** The NAL unit type (the 6-bit field of the two-byte header). */
    nimrtc::video_payload::h265::NaluType type =
        nimrtc::video_payload::h265::NaluType::kUnspecified;

    /** The spatial layer id. */
    std::uint8_t layer_id = 0;

    /** `nuh_temporal_id_plus1`. */
    std::uint8_t tid = 1;

    /** Raw NAL unit bytes *including* the two-byte NAL header, but *not*
     *  the start code prefix. */
    core::ByteSpan data;

    /** Length of the start code that preceded this NAL unit (3 or 4). */
    std::size_t start_code_len = 0;
};

// ---------------------------------------------------------------------------
// BitstreamParser — stateful iterator over an Annex B bitstream
// ---------------------------------------------------------------------------

/** Iterator over the NAL units of an H.265 Annex B bitstream.
 *
 *  The parser is deliberately *structural*: it locates the NAL units and
 *  their headers, and leaves the RBSP (including the emulation-prevention
 *  bytes) untouched for the decoder to deal with. */
class NIMRTC_API_STABLE BitstreamParser {
public:
    BitstreamParser() noexcept = default;

    /** Begin parsing a new bitstream. */
    void reset(core::ByteSpan bitstream) noexcept {
        bitstream_ = bitstream;
        cursor_   = 0;
        finished_ = false;
    }

    /** Pull the next NAL unit.  Returns false when the stream is
     *  exhausted (or malformed). */
    bool next(ParsedNalu& out) noexcept;

    /** True iff the iterator is exhausted. */
    [[nodiscard]] bool finished() const noexcept { return finished_; }

    /** Pull all NAL units into a vector. */
    std::vector<ParsedNalu> parse_all(core::ByteSpan bitstream) noexcept;

private:
    /** Find the offset of the next start code at or after @p from.
     *  Returns `kNotFound` when there is none. */
    std::size_t find_start_code_at_or_after(std::size_t from) noexcept;

    core::ByteSpan bitstream_{};
    std::size_t    cursor_   = 0;
    bool           finished_ = false;
};

// ---------------------------------------------------------------------------
// Decoder interface
// ---------------------------------------------------------------------------

/** Output of one decode call. */
struct NIMRTC_API_STABLE DecodedImage {
    video_frame::VideoFrameBuffer buffer;   ///< refcounted pixel storage
    std::uint32_t                 width    = 0;
    std::uint32_t                 height   = 0;
    std::int64_t                  capture_ts_us = 0;
    std::uint32_t                 frame_seq     = 0;
    std::uint32_t                 rtp_timestamp = 0;
    bool                          is_keyframe   = false;
};

/** Abstract H.265 decoder — implemented by host plugins (or by the
 *  shared NVIDIA NVDEC backend in the `nvcodec` module). */
class NIMRTC_API_STABLE Decoder {
public:
    virtual ~Decoder() = default;

    /** Initialise the decoder with a configuration.
     *  Must be called once before decode(). */
    virtual plugins::Status open() noexcept = 0;

    /** Decode one access unit.
     *  @param encoded  H.265 Annex B bitstream (VPS/SPS/PPS may precede the
     *                  slices).
     *  @param info     Per-frame metadata (capture_ts, frame_seq,
     *                  rtp_timestamp).
     *  @return DecodedImage on success (may have an empty buffer for the
     *          stub). */
    virtual std::optional<DecodedImage> decode(core::ByteSpan encoded,
                                               video_frame::VideoFrameInfo info) noexcept = 0;

    /** Tear down. */
    virtual void close() noexcept = 0;

    /** Whether this is a no-op decoder (consumes input, produces empty
     *  output). */
    virtual bool is_stub() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// Stub decoder
// ---------------------------------------------------------------------------

/** A no-op decoder used when no real H.265 implementation is linked. */
class NIMRTC_API_STABLE StubDecoder final : public Decoder {
public:
    StubDecoder() = default;
    ~StubDecoder() override = default;

    plugins::Status open() noexcept override { return plugins::kOk; }

    std::optional<DecodedImage> decode(core::ByteSpan encoded,
                                       video_frame::VideoFrameInfo info) noexcept override;

    void close() noexcept override {}

    bool is_stub() const noexcept override { return true; }
};

/** Factory for the stub decoder. */
NIMRTC_API_STABLE std::unique_ptr<Decoder> create_stub_decoder() noexcept;

} // namespace nimrtc::h265
