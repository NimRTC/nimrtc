/**
 * @file nimrtc/video_payload/av1.hpp
 * @brief AV1 RTP payload packetizer / depacketizer
 *        (draft-ietf-payload-av1 / aomediacodec av1-rtp-spec v1.0.0).
 *
 * ## The AV1 aggregation header
 *
 * Every AV1 RTP payload starts with a one-byte aggregation header:
 *
    10| *    0 1 2 3 4 5 6 7
 *   +-+-+-+-+-+-+-+-+
 *   |Z|Y| W |N|-|-|-|
 *   +-+-+-+-+-+-+-+-+
 *
 *   - **Z** — the first OBU element in this packet continues an OBU
 *     fragment from the previous packet.
 *   - **Y** — the last OBU element in this packet is continued in the
 *     next packet.
 *   - **W** — the number of OBU elements in the packet.  When it is 0,
    20| *     *every* element is preceded by a length field; when it is 1..3,
 *     the *last* element carries no length field (its length is derived
 *     from the packet length).
 *   - **N** — this packet is the first packet of a coded video sequence.
 *
 * The aggregation header is *not* part of the AV1 bitstream and must
 * never be handed to an AV1 decoder.
 *
 * ## OBU elements and the length field
 *
 * The payload carries one or more **OBU elements**.  An OBU element is
 * either a complete OBU or a fragment of one:
 *
 *   - A **complete OBU** is `obu_header` (+ optional `obu_size`) +
 *     payload.
 *   - A **fragment** carries only the payload bytes of an OBU; the
 *     header is present only in the *first* fragment of that OBU.
 *
 * The length field that precedes an element is **LEB128**-encoded (the
 * AV1 specification's variable-length integer encoding, AV1 §4.10.5) —
 * *not* a fixed 16-bit big-endian integer.  NimRTC emits the W = 0
 * form (every element carries an explicit length field) whenever a
 * packet holds more than one element, and the W = 1 form (a single
 * element, no length field) for the single-element packets.
 *
 * ## Which elements carry an OBU header
 *
 * Only the *first* fragment of an OBU carries its header.  For a packet
 * holding @c n elements:
 *
 *   - element 0 carries a header iff `Z == 0`;
 *   - elements 1..n-2 always carry a header;
 *   - the last element carries a header iff `Y == 0`.
 *
 * A single-element packet therefore carries a header iff `Z == 0` —
 * which is exactly the case for the first fragment of a fragmented OBU
 * (`Z = 0`, `Y = 1`).
 *
 * NimRTC's packetizer produces the common case: each OBU of the frame
    40| * is carried whole where it fits, several small OBUs are aggregated
 * into one packet where that helps, and an OBU that is too large is split
 * across packets with the Z / Y flags set appropriately.  The sequence
 * header OBU is repeated on every key frame, so a receiver that joins
 * mid-stream can still decode.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>
#include <cstdint>
#include <cstddef>
#include <optional>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/error.hpp>

namespace nimrtc::video_payload::av1 {

// ---------------------------------------------------------------------------
// OBU types (AV1 spec §5.3.1)
// ---------------------------------------------------------------------------

enum class NIMRTC_API_STABLE ObuType : std::uint8_t {
    kReserved1        = 0,
    kSequenceHeader   = 1,
    kTemporalDelimiter = 2,
    kFrameHeader      = 3,
    kTileGroup        = 4,
    kMetadata         = 5,
    kFrame            = 6,
    kRedundantFrameHeader = 7,
    kTileList         = 8,
    kPadding          = 15,
};

// ---------------------------------------------------------------------------
// The OBU header
// ---------------------------------------------------------------------------

struct NIMRTC_API_STABLE ObuHeader {
    ObuType    type      = ObuType::kReserved1;
    /** True when the extension header is present. */
    bool       has_extension = false;
    /** True when the `obu_size` field is present. */
    bool       has_size      = false;
    /** The temporal/spatial id (3 bits). */
    std::uint8_t temporal_id = 0;
    std::uint8_t spatial_id  = 0;
    /** The size of the OBU header (including the optional extension byte
     *  and the `obu_size` field) in bytes. */
    std::size_t header_size  = 0;
};

/** Parse the OBU header at the start of @p payload.
 *  @return The header, or std::nullopt when @p payload is empty. */
NIMRTC_API_STABLE std::optional<ObuHeader> parse_obu_header(
    core::ByteSpan payload) noexcept;

/** The type of the OBU at the start of @p payload. */
NIMRTC_API_STABLE ObuType peek_obu_type(core::ByteSpan payload) noexcept;

/** Size of the `obu_size` field (the LEB128 encoding) for @p size bytes. */
NIMRTC_API_STABLE std::size_t leb128_size(std::size_t size) noexcept;

/** Write the LEB128 encoding of @p size into @p output.
 *  @return Number of bytes written; 0 on overflow. */
NIMRTC_API_STABLE std::size_t write_leb128(std::size_t size,
                           core::MutableByteSpan output) noexcept;

/** Read a LEB128 value from @p payload starting at @p pos.
 *  @param pos     In/out cursor — advanced past the value on success.
 *  @param value   Receives the decoded value.
 *  @return true on success; false when the value is truncated or the
 *          encoding is longer than 8 bytes (which cannot happen in a
 *          legal AV1 bitstream). */
NIMRTC_API_STABLE bool read_leb128(core::ByteSpan payload,
                                   std::size_t& pos,
                                   std::size_t& value) noexcept;

// ---------------------------------------------------------------------------
// The AV1 aggregation header
// ---------------------------------------------------------------------------

struct NIMRTC_API_STABLE AggregationHeader {
    /** The first OBU element continues a fragment from the previous packet. */
    bool          starts_with_fragment = false;
    /** The last OBU element is continued in the next packet. */
    bool          ends_with_fragment   = false;
    /** The number of OBU elements in the packet (0 = "each element has a
     *  length field"). */
    std::uint8_t  element_count        = 0;
    /** This packet starts a new coded video sequence. */
    bool          new_sequence         = false;
};

/** Parse the aggregation header at the start of @p payload. */
NIMRTC_API_STABLE std::optional<AggregationHeader> parse_aggregation_header(
    core::ByteSpan payload) noexcept;

/** Write the aggregation header into @p output.
 *  @return Number of bytes written (1), or 0 on overflow. */
NIMRTC_API_STABLE std::size_t write_aggregation_header(
    const AggregationHeader& h, core::MutableByteSpan output) noexcept;

// ---------------------------------------------------------------------------
// OBU elements
// ---------------------------------------------------------------------------

/** One OBU element: either a whole OBU or a fragment of one.
 *
 *  `data` references the input buffer for parsed elements; for built
 *  elements it references the caller's own buffers. */
struct NIMRTC_API_STABLE ObuElement {
    /** The OBU type this element belongs to.  Only meaningful when
     *  @ref carries_header is true — a fragment continuation carries no
     *  header and therefore no type of its own. */
    ObuType      type = ObuType::kReserved1;
    /** The element bytes.
     *
     *  - For a *complete OBU* this is `obu_header` + payload.
     *  - For a *fragment* this is payload only (the header lives in the
     *    first fragment of the OBU). */
    core::ByteSpan data;
    /** True iff this element starts an OBU, i.e. it carries the OBU
     *  header (the first fragment of a fragmented OBU does too). */
    bool         carries_header = false;
    /** True iff this element is the last fragment of a fragmented OBU. */
    bool         is_last_fragment  = false;
};

/** Parse the OBU elements carried by one RTP payload.
 *
 *  @param payload  The RTP payload (starting at the aggregation header).
 *  @param elements Receives the parsed elements.
 *  @return true when the payload parsed cleanly. */
NIMRTC_API_STABLE bool parse_elements(core::ByteSpan payload,
                                      std::vector<ObuElement>& elements) noexcept;

// ---------------------------------------------------------------------------
// Packetization
// ---------------------------------------------------------------------------

/** One OBU of a frame, as handed to the packetizer. */
struct NIMRTC_API_STABLE Obu {
    ObuType      type = ObuType::kReserved1;
    /** The OBU bytes *including* the OBU header. */
    core::ByteSpan data;
};

/** Split an Annex-B OBU stream into its OBUs.
 *
 *  The Annex-B framing (`00 00 00 01` before each OBU) is what the
 *  NVENC AV1 encoder emits (`outputAnnexBFormat = 1`) and what the
 *  CUVID AV1 parser expects, so it is the framing NimRTC carries
 *  internally for AV1.
 *
 *  @param bs    The Annex-B stream.
 *  @param obus  Receives the OBUs — each *with* its OBU header, but
 *               *without* the start-code prefix.
 *  @return true when at least one OBU was parsed. */
NIMRTC_API_STABLE bool parse_annex_b_obus(core::ByteSpan bs,
                                           std::vector<Obu>& obus) noexcept;

/** True when the Annex-B stream @p bs carries an AV1 *sequence header*
 *  OBU.
 *
 *  The sequence header is repeated on every key frame, so its presence
 *  is the signal that the stream can be decoded from this point on. */
NIMRTC_API_STABLE bool contains_sequence_header(core::ByteSpan bs) noexcept;

/** A packet produced by the packetizer. */
struct NIMRTC_API_STABLE Packet {
    /** The RTP payload bytes (aggregation header + OBU elements). */
    std::vector<std::uint8_t> buffer;
    /** A view of @p buffer — the actual RTP payload. */
    core::ByteSpan payload;
    /** True iff this packet is the last of the frame. */
    bool          marker = false;
};

/** Packetize one AV1 frame into RTP payloads.
 *
 *  @param obus     The frame's OBUs (each *with* its OBU header).
 *  @param mtu      The maximum RTP payload size in bytes.
 *  @param new_sequence  True when the frame starts a new coded video
 *                      sequence (i.e. it is a key frame).
 *  @param packets  Receives the produced packets.  The `buffer` member of
 *                  each packet holds the bytes; `payload` points into it.
 *  @return false when the frame cannot be packetized (e.g. an OBU that
 *          is too small to fragment). */
NIMRTC_API_STABLE bool packetize(const std::vector<Obu>& obus,
                                 std::size_t mtu,
                                 bool new_sequence,
                                 std::vector<Packet>& packets) noexcept;

// ---------------------------------------------------------------------------
// Reassembly
// ---------------------------------------------------------------------------

/** Reassembles the OBUs of an AV1 frame from a sequence of RTP payloads.
 *
 *  The depacketizer's counterpart to `packetize()`: it consumes the
 *  aggregation headers, undoes the fragmentation (Z / Y) and emits the
 *  OBUs in **Annex-B** framing — the form the AV1 decoder wants.
 *
 *  The class is stateful across packets (a fragmented OBU spans
 *  several of them), so the caller keeps one instance per stream.
 */
class NIMRTC_API_STABLE ObuReassembler {
public:
    /** Feed one RTP payload.
     *
     *  @param payload  The RTP payload (starting at the aggregation header).
     *  @param annex_b  Receives the reassembled OBUs in Annex-B framing.
     *                 The buffer is *appended* to.
     *  @return true when the payload parsed cleanly. */
    bool push(core::ByteSpan payload, std::vector<std::uint8_t>& annex_b) noexcept;

    /** Drop any partial OBU and forget the sequence-header state. */
    void reset() noexcept;

    /** True once a *sequence header* OBU has been seen.
     *
     *  The sequence header is repeated on every key frame; a receiver
     *  that has seen one can decode the stream from that point on. */
    bool has_sequence_header() const noexcept { return saw_sequence_header_; }

private:
    /** The OBU currently being assembled (header + payload so far). */
    std::vector<std::uint8_t> pending_;
    /** True when the assembly holds an OBU that is not complete yet. */
    bool          pending_open_ = false;
    bool          saw_sequence_header_ = false;
};

} // namespace nimrtc::video_payload::av1
