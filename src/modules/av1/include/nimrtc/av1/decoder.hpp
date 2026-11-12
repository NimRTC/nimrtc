/**
 * @file nimrtc/av1/decoder.hpp
 * @brief AV1 decoder interface + stub implementation.
 *
 * ## Why a stub
 *
 * NimRTC does not bundle a software AV1 decoder (dav1d is an optional
 * vendor dependency behind `NIMRTC_VENDORED_DAV1D`; see
 * `docs/plan/remote-desktop-plan.md` RD-2 / B4).  What this module ships:
 *
 *   - The `Decoder` interface — what a host plugin (or the shared NVIDIA
 *     backend in the `nvcodec` module) must implement.
 *   - A no-op `StubDecoder` that consumes an AV1 access unit and reports
 *     its key-frame-ness without producing pixels.
 *
 * ## Access unit framing
 *
 * The decoder consumes an **Annex B** OBU stream: each OBU is preceded by
 * a 4-byte start code (`00 00 00 01`), which is the framing the NVENC AV1
 * encoder emits (`outputAnnexBFormat = 1`) and the framing the CUVID AV1
 * parser expects.
 *
 * The OBU header (AV1 spec §5.3.1) is one byte:
 *
 *      bit 7     : obu_forbidden_bit (always 0)
 *      bits 6..3 : obu_type
 *      bit 2     : obu_extension_flag
 *      bit 1     : obu_has_size_field
 *      bit 0     : obu_reserved_1bit
 *
 * A random-access point is signalled by the **sequence header** OBU
 * (type 1), which the encoder repeats on every key frame.
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
#include <nimrtc/video_payload/av1.hpp>

namespace nimrtc::av1 {

// ---------------------------------------------------------------------------
// One OBU extracted from an Annex B stream
// ---------------------------------------------------------------------------

/** A single OBU extracted from an Annex B stream.
 *
 *  `data` references the parser's input buffer — the caller must keep it
 *  alive for as long as the span is used. */
struct NIMRTC_API_STABLE ParsedObu {
    /** The OBU type. */
    nimrtc::video_payload::av1::ObuType type =
        nimrtc::video_payload::av1::ObuType::kReserved1;

    /** The OBU bytes *including* the OBU header, but *not* the start code
     *  prefix. */
    core::ByteSpan data;

    /** Length of the start code that preceded this OBU (3 or 4). */
    std::size_t start_code_len = 0;
};

// ---------------------------------------------------------------------------
// BitstreamParser — stateful iterator over an Annex B OBU stream
// ---------------------------------------------------------------------------

/** Iterator over the OBUs of an AV1 Annex B stream.
 *
 *  The parser is deliberately *structural*: it locates the OBUs and their
 *  headers, and leaves the OBU payload untouched for the decoder. */
class NIMRTC_API_STABLE BitstreamParser {
public:
    BitstreamParser() noexcept = default;

    /** Begin parsing a new bitstream. */
    void reset(core::ByteSpan bitstream) noexcept {
        bitstream_ = bitstream;
        cursor_   = 0;
        finished_ = false;
    }

    /** Pull the next OBU.  Returns false when the stream is exhausted (or
     *  malformed). */
    bool next(ParsedObu& out) noexcept;

    /** True iff the iterator is exhausted. */
    [[nodiscard]] bool finished() const noexcept { return finished_; }

    /** Pull all OBUs into a vector. */
    std::vector<ParsedObu> parse_all(core::ByteSpan bitstream) noexcept;

private:
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

/** Abstract AV1 decoder — implemented by host plugins (or by the shared
 *  NVIDIA NVDEC backend in the `nvcodec` module). */
class NIMRTC_API_STABLE Decoder {
public:
    virtual ~Decoder() = default;

    /** Initialise the decoder with a configuration.
     *  Must be called once before decode(). */
    virtual plugins::Status open() noexcept = 0;

    /** Decode one access unit.
     *  @param encoded  AV1 Annex B OBU stream.
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

/** A no-op decoder used when no real AV1 implementation is linked. */
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

} // namespace nimrtc::av1
