/**
 * @file nimrtc/video_payload/h265.hpp
 * @brief H.265 / HEVC RTP payload packetizer / depacketizer (RFC 7798).
 *
 * ## Relationship to the H.264 payload module
 *
 * H.265 reuses the *shape* of the H.264 payload format (RFC 6184) but
 * differs in two important ways:
 *
 *   1. The NAL unit header is **two bytes** (H.264's is one):
 *
 *        0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5
 *       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *       |F|   Type    |  LayerId  | TID   |
 *       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 *      `F` is the forbidden-zero bit, `Type` is the 6-bit NAL unit type,
 *      `LayerId` is the 6-bit spatial layer id, and `TID` is
 *      `nuh_temporal_id_plus1` (3 bits).
 *
 *   2. The **aggregation** packet is `AP` (type 48), not STAP-A, and its
 *      aggregation unit carries its own 16-bit length prefix per NAL
 *      unit (plus an optional 16-bit DONL).
 *
 * ## Packetization modes (RFC 7798 §4.3)
 *
 *   - **Single NAL unit packet** — NAL types 0..47 (and 64..127, which
 *     are reserved for future RTP header extensions).  The payload is
 *     the NAL unit verbatim; the two-byte payload header *is* the NAL
 *     header.
 *   - **AP** (type 48) — aggregates several small NAL units that share
 *     a timestamp.
 *   - **FU** (type 49) — fragments one large NAL unit.
 *   - **PACI** (type 50) — payload content aggregation; not produced
 *     here (the picture/gap handling it provides is not needed for the
 *     low-latency path NimRTC implements).
 *
 * ## FU header
 *
 *        0 1 2 3 4 5 6 7
 *       +-+-+-+-+-+-+-+-+
 *       |S|E|  FuType  |
 *       +-+-+-+-+-+-+-+-+
 *
 * `S` marks the first fragment, `E` the last, and `FuType` carries the
 * type of the *original* (un-fragmented) NAL unit.
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

namespace nimrtc::video_payload::h265 {

// ---------------------------------------------------------------------------
// NAL unit types (ITU-T H.265 Table 7-1)
// ---------------------------------------------------------------------------

enum class NIMRTC_API_STABLE NaluType : std::uint8_t {
    kUnspecified       = 0,
    kSliceTRAIL_N      = 0,
    kSliceTRAIL_R      = 1,
    kSliceTSA_N        = 2,
    kSliceTSA_R        = 3,
    kSliceSTSA_N       = 4,
    kSliceSTSA_R       = 5,
    kSliceRADL_N       = 6,
    kSliceRADL_R       = 7,
    kSliceRASL_N       = 8,
    kSliceRASL_R       = 9,

    // 10..15 reserved
    kSliceBLA_W_LP     = 16,
    kSliceBLA_W_RADL   = 17,
    kSliceBLA_N_LP     = 18,
    kSliceIDR_W_RADL   = 19,
    kSliceIDR_N_LP     = 20,
    kSliceCRA_NUT      = 21,

    kVPS               = 32,
    kSPS               = 33,
    kPPS               = 34,
    kAUD               = 35,
    kEndOfSequence     = 36,
    kEndOfStream       = 37,
    kFillerData        = 38,
    kPrefixSEI         = 39,
    kSuffixSEI         = 40,

    // 41..47 reserved (RSV_NVCL)
    // 48..63 are the RTP-payload-specific types below.
    kAP                = 48,
    kFU                = 49,
    kPACI              = 50,
};

/** True iff @p t names a VCL NAL unit (a coded slice). */
NIMRTC_API_STABLE inline bool is_vcl_nalu(NaluType t) noexcept {
    const auto v = static_cast<std::uint8_t>(t);
    return v <= 9 || (v >= 16 && v <= 21);
}

/** True iff @p t names an IRAP (intra) picture — the H.265 equivalent of
 *  H.264's IDR.  These are the random-access points a receiver needs
 *  before it can start decoding. */
NIMRTC_API_STABLE inline bool is_irap_nalu(NaluType t) noexcept {
    const auto v = static_cast<std::uint8_t>(t);
    return v >= 16 && v <= 21;
}

/** True iff @p t is a video parameter set (VPS / SPS / PPS). */
NIMRTC_API_STABLE inline bool is_parameter_set_nalu(NaluType t) noexcept {
    return t == NaluType::kVPS || t == NaluType::kSPS || t == NaluType::kPPS;
}

/** True iff @p t names a key-frame (intra) slice. */
NIMRTC_API_STABLE inline bool is_keyframe_nalu(NaluType t) noexcept {
    return is_irap_nalu(t);
}

// ---------------------------------------------------------------------------
// The two-byte NAL unit header
// ---------------------------------------------------------------------------

struct NIMRTC_API_STABLE NaluHeader {
    /** The forbidden-zero bit.  Must be 0 for a legal NAL unit. */
    std::uint8_t forbidden = 0;
    /** The 6-bit NAL unit type. */
    NaluType     type      = NaluType::kUnspecified;
    /** The 6-bit spatial layer id. */
    std::uint8_t layer_id  = 0;
    /** `nuh_temporal_id_plus1` — the 3-bit temporal id plus one. */
    std::uint8_t tid       = 1;
};

/** Parse the two-byte NAL unit header at the start of @p payload.
 *  @return The header, or std::nullopt when @p payload is too short. */
NIMRTC_API_STABLE std::optional<NaluHeader> parse_nalu_header(
    core::ByteSpan payload) noexcept;

/** Write the two-byte payload header for @p type / @p layer_id / @p tid.
 *  @return Number of bytes written (2), or 0 when @p output is too small. */
NIMRTC_API_STABLE std::size_t build_payload_header(NaluType type,
                             std::uint8_t layer_id,
                             std::uint8_t tid,
                             core::MutableByteSpan output) noexcept;

// ---------------------------------------------------------------------------
// One NAL unit extracted from a packet
// ---------------------------------------------------------------------------

struct NIMRTC_API_STABLE Nalu {
    NaluType       type      = NaluType::kUnspecified;
    /** The NAL unit bytes *including* the two-byte NAL header.
     *  Pointers reference the input buffer — the caller must keep it alive. */
    core::ByteSpan data;
    /** True iff the marker bit was set on this NALU's RTP packet. */
    bool           marker    = false;
};

// ---------------------------------------------------------------------------
// Single NAL unit packet
// ---------------------------------------------------------------------------

/** Parse a Single NAL unit packet (NAL types 0..47).
 *  @return The NAL unit, or std::nullopt when the payload is empty or
 *          carries a reserved / wrapper type. */
NIMRTC_API_STABLE std::optional<Nalu> parse_single(core::ByteSpan payload) noexcept;

// ---------------------------------------------------------------------------
// AP — aggregation packet (NAL type 48)
// ---------------------------------------------------------------------------

/** Parse an AP packet into its constituent NAL units.
 *
 *  The AP payload is:
 *
 *      payload header (2 bytes, type = 48)
 *      [ DONL (2 bytes) ]        — present only when the peer negotiated
 *                                  `sprop-max-don-diff`
 *      [ size (2 bytes) ][ NAL unit ] × N
 *
 *  @param payload       The RTP payload.
 *  @param has_donl      Whether the DONL field is present.
 *  @param nalus         Receives the NAL units (each *with* its own
 *                       two-byte NAL header).
 *  @return true when at least one NAL unit was parsed. */
NIMRTC_API_STABLE bool parse_ap(core::ByteSpan payload,
                                bool has_donl,
                                std::vector<Nalu>& nalus) noexcept;

/** Build an AP packet from a list of NAL units.
 *  @param nalus      The NAL units (each *with* its two-byte header).
 *  @param layer_id   The spatial layer id to stamp on the AP header.
 *  @param tid        The temporal id to stamp on the AP header.
 *  @param output     Caller-owned buffer.
 *  @return Number of bytes written; 0 on overflow. */
NIMRTC_API_STABLE std::size_t build_ap(const std::vector<Nalu>& nalus,
                       std::uint8_t layer_id,
                       std::uint8_t tid,
                       core::MutableByteSpan output) noexcept;

// ---------------------------------------------------------------------------
// FU — fragmentation unit (NAL type 49)
// ---------------------------------------------------------------------------

/** The FU header (RFC 7798 §4.3.3). */
struct NIMRTC_API_STABLE FuHeader {
    std::uint8_t  fu_header = 0;   // S + E + FuType
    NaluType      fu_type   = NaluType::kUnspecified;
    bool          start     = false;
    bool          end       = false;
};

/** Parse an FU packet's first three bytes (payload header + FU header).
 *  @return true when @p payload starts with an FU (type 49). */
NIMRTC_API_STABLE bool parse_fu_header(core::ByteSpan payload,
                       FuHeader& out) noexcept;

/** Build one FU fragment.
 *  @param original_nalu_type The NAL unit type of the *original* NAL unit.
 *  @param layer_id           Spatial layer id (stamped on the payload header).
 *  @param tid                Temporal id (stamped on the payload header).
 *  @param fragment_data      The bytes of this fragment (the original NAL
 *                            unit minus its two-byte header).
 *  @param start              True iff this is the FIRST fragment.
 *  @param end                True iff this is the LAST fragment.
 *  @param output             Caller-owned buffer.
 *  @return Number of bytes written; 0 on overflow. */
NIMRTC_API_STABLE std::size_t build_fu(NaluType original_nalu_type,
                       std::uint8_t layer_id,
                       std::uint8_t tid,
                       core::ByteSpan fragment_data,
                       bool start,
                       bool end,
                       core::MutableByteSpan output) noexcept;

// ---------------------------------------------------------------------------
// Convenience — top-level dispatcher
// ---------------------------------------------------------------------------

/** Determine the NAL unit type from the first two bytes of an RTP payload.
 *  Returns kUnspecified for a payload shorter than two bytes. */
NIMRTC_API_STABLE NaluType peek_nalu_type(core::ByteSpan payload) noexcept;

/** The RTP payload-format wrapper type of the packet, i.e. the type of the
 *  *payload header* rather than of the encapsulated NAL unit.
 *
 *  For a Single NAL unit packet this equals the NAL unit type; for an
 *  AP / FU packet it is 48 / 49 respectively. */
NIMRTC_API_STABLE NaluType peek_payload_type(core::ByteSpan payload) noexcept;

} // namespace nimrtc::video_payload::h265
