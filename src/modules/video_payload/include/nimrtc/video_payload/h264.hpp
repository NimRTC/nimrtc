/**
 * @file nimrtc/video_payload/h264.hpp
 * @brief H.264 RTP payload packetizer / depacketizer (RFC 6184).
 *
 * Per ARCHITECTURE.md §6 (Codec) + §4.2 Tier 0:
 *   - Payload 打包/解析归 rtp 模块（P1 即含 Opus / H.264，支撑 Tier 0 互通）
 *
 * RFC 6184 defines three transport modes for H.264 NAL units:
 *   1. Single NAL Unit Packet: one NAL unit fits in a single RTP packet.
 *      NAL type 1..23 (except the type field of the first byte).
 *   2. STAP-A (Single-Time Aggregation Packet, type 24): one RTP packet
 *      carries multiple small NAL units that share the same RTP timestamp.
 *      STAP-B (type 25) is defined but rarely used; we provide a parser
 *      but no packetizer for STAP-B.
 *   3. FU-A (Fragmentation Unit, type 28): one large NAL unit is split
 *      into multiple RTP packets.  FU-B (type 29) supports interleaving;
 *      we provide a parser but no packetizer for FU-B.
 *
 * Marker bit (M):
 *   - Last packet of an access unit has M=1.
 *   - For FU-A: the last fragment has M=1; the rest have M=0.
 *   - For STAP-A: typically M=1 (last packet of the access unit).
 *
 * @note P1. Standalone module; not yet wired into the engine.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/error.hpp>

namespace nimrtc::video_payload::h264 {

// ---------------------------------------------------------------------------
// H.264 NAL unit type (RFC 6184 §5.4)
// ---------------------------------------------------------------------------

enum class NaluType : std::uint8_t {
    kUnspecified       = 0,
    kSliceNonIDR       = 1,
    kSliceDPA          = 2,
    kSliceDPB          = 3,
    kSliceDPC          = 4,
    kSliceIDR          = 5,
    kSEI               = 6,
    kSPS               = 7,
    kPPS               = 8,
    kAUD               = 9,
    kEndOfSequence     = 10,
    kEndOfStream       = 11,
    kFiller            = 12,
    kSPSExt            = 13,
    kPrefixNALU        = 14,
    kSPSSubset         = 15,
    kAuxiliarySlice    = 19,
    kSliceExt          = 20,
    kSliceDepthExt     = 21,
    // 24..31 are RFC 6184-defined wrappers.
    kSTAPA             = 24,
    kSTAPB             = 25,
    kMTAP16            = 26,
    kMTAP24            = 27,
    kFUA               = 28,
    kFUB               = 29,
    // 30, 31 reserved.
};

inline bool is_keyframe_nalu(NaluType t) noexcept {
    return t == NaluType::kSliceIDR;
}

inline bool is_parameter_set_nalu(NaluType t) noexcept {
    return t == NaluType::kSPS || t == NaluType::kPPS
        || t == NaluType::kSPSExt || t == NaluType::kSPSSubset;
}

// ---------------------------------------------------------------------------
// One NAL unit extracted from a packet
// ---------------------------------------------------------------------------

struct Nalu {
    NaluType       type      = NaluType::kUnspecified;
    /** Raw NAL unit bytes WITHOUT the start-code prefix.
     *  Pointers reference the input buffer — caller must keep it alive. */
    core::ByteSpan data;
    /** True iff the marker bit was set on this NALU's RTP packet. */
    bool           marker    = false;
};

// ---------------------------------------------------------------------------
// Single NAL Unit Packet — parse / build
// ---------------------------------------------------------------------------

/** Parse a single NAL Unit Packet (NAL types 1..23).
 *  @param payload RTP payload bytes (just the payload, after the RTP header)
 *  @return NALU on success; std::nullopt if the payload is empty or doesn't
 *          look like a single NAL unit. */
std::optional<Nalu> parse_single(core::ByteSpan payload) noexcept;

/** Build a single NAL Unit Packet (NAL types 1..23).
 *  @param nalu_data NAL unit bytes WITHOUT the start-code prefix.
 *  @param output    Caller-owned buffer.
 *  @return number of bytes written; 0 on overflow. */
std::size_t build_single(NaluType type,
                         core::ByteSpan nalu_data,
                         core::MutableByteSpan output) noexcept;

// ---------------------------------------------------------------------------
// STAP-A — multiple small NAL units in one RTP packet
// ---------------------------------------------------------------------------

/** Parse an STAP-A packet into its constituent NAL units.
 *  Caller's buffer must outlive the returned Nalus (we reference into it). */
struct StapAResult {
    std::vector<Nalu> nalus;
    bool              parsed_ok = false;
};
StapAResult parse_stap_a(core::ByteSpan payload) noexcept;

/** Build an STAP-A packet from a list of NAL units.
 *  Each nalu in @p nalus must have valid `data` (raw NAL unit body, no
 *  start code) and a `type` field.  The function writes the STAP-A header
 *  + each NAL unit with its 16-bit length prefix. */
std::size_t build_stap_a(core::ByteSpan* nalu_datas,
                         std::size_t nalu_count,
                         core::MutableByteSpan output) noexcept;

// ---------------------------------------------------------------------------
// FU-A — fragmentation of large NAL units
// ---------------------------------------------------------------------------

/** FU-A header (RFC 6184 §5.8). */
struct FuAHeader {
    std::uint8_t  fu_indicator = 0;   // F + NRI + Type=28
    std::uint8_t  fu_header    = 0;   // S + E + R + Type
    core::ByteSpan fragment;          // payload of THIS fragment
};

/** Parse a FU-A packet's first 2 bytes (indicator + header).
 *  @return true if `payload` starts with FU-A (type 28 in indicator byte). */
bool parse_fu_a_header(core::ByteSpan payload,
                       FuAHeader& out) noexcept;

/** Build one FU-A fragment.
 *  @param original_nalu_type The NAL unit type of the *original* (un-fragmented) NALU.
 *  @param fragment_data       The bytes of this fragment (NAL unit body, sans
 *                            the original 1-byte NAL header).
 *  @param start               True iff this is the FIRST fragment.
 *  @param end                 True iff this is the LAST fragment.
 *  @param output              Caller-owned buffer.
 *  @return Number of bytes written; 0 on overflow. */
std::size_t build_fu_a(NaluType original_nalu_type,
                       core::ByteSpan fragment_data,
                       bool start,
                       bool end,
                       core::MutableByteSpan output) noexcept;

// ---------------------------------------------------------------------------
// Convenience — top-level dispatcher
// ---------------------------------------------------------------------------

/** Parse any RTP payload type (Single / STAP-A / FU-A / FU-B).
 *  FU-B is parsed as a header + fragment but not reassembled (rare in practice). */
struct ParseResult {
    enum class Kind : std::uint8_t {
        kSingle = 0,
        kStapA,
        kFuAFragment,
        kFuBFragment,
        kUnsupported,
    };
    Kind            kind        = Kind::kUnsupported;
    /** For Single: the NAL unit.  For STAP-A: the parsed NALUs (only first
     *  one is set if you use the convenience overload below; full result is
     *  via parse_stap_a). For FU-A: only the header + this fragment. */
    Nalu            primary;
    FuAHeader       fu_a_header{};
    bool            fu_a_start  = false;
    bool            fu_a_end    = false;
    bool            parsed_ok   = false;
};

ParseResult parse(core::ByteSpan payload) noexcept;

/** Determine NAL unit type from the first byte of an RTP payload.
 *  Returns kUnspecified for empty payload. */
NaluType peek_nalu_type(core::ByteSpan payload) noexcept;

} // namespace nimrtc::video_payload::h264
