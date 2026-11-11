/**
 * @file nimrtc/rtp/rtcp_xr.hpp
 * @brief RTCP Extended Report (XR) — RFC 3611 block parsing and building.
 *
 * v1.2.0 (session capability plan X1 / X2).
 *
 * ## Why this file exists
 *
 * NimRTC already *recognises* PT 207 (`RtcpBlockKind::kExtendedReport`)
 * in the compound demultiplexer, but recognition was the whole of the
 * implementation: the XR sub-packet came out of `demux_rtcp()` as an
 * opaque byte span that nothing consumed.
 *
 * RFC 3611 defines an XR packet as an 8-byte header followed by a
 * stack of *extended report blocks*.  Each block starts with a
 * 4-byte header — a block type (`BT`), a type-specific byte and a
 * length field counting 32-bit words **minus one** — and the length
 * is what lets a receiver walk past a block type it does not know.
 * That last property is the reason XR is extensible, and it is why
 * the parser here is deliberately forgiving: an unknown `BT` is
 * skipped by its length, never an error.
 *
 * ## The block types
 *
 * RFC 3611 §6.2 registers seven block types.  NimRTC parses the
 * three that carry round-trip measurement and network statistics:
 *
 * | BT | Block | Section |
 * |----|-------|---------|
 * | 4  | Receiver Reference Time Report | RFC 3611 §4.4 |
 * | 5  | DLRR Report | RFC 3611 §4.5 |
 * | 6  | Statistics Summary Report | RFC 3611 §4.6 |
 *
 * The remaining registered types — Loss RLE (1), Duplicate RLE (2),
 * Packet Receipt Times (3), VoIP Metrics (7) — are *recognised* (they
 * are skipped cleanly by the block walk) but not parsed.  They carry
 * no field any NimRTC consumer reads today, and parsing them would
 * be dead code.
 *
 * @note A block type number that a host may legitimately expect to
 *       see named "Receiver Estimated Quality" does **not** exist in
 *       RFC 3611: that was the name of an early draft's block, and
 *       the standard's numbering is BT 1 = Loss RLE, BT 5 = DLRR,
 *       BT 6 = Statistics Summary.  A report that used the draft's
 *       numbering would not interoperate with a conforming peer.
 *
 * ## On the wire
 *
 * ```text
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |V=2|P|reserved |   PT=XR=207   |             length            |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                              SSRC                             |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * :                       report blocks                          :
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * ```
 *
 * The XR header is two 32-bit words (RFC 3611 §2) — note that this is
 * *one word longer* than the 4-byte header of every other RTCP
 * packet, because the XR packet carries an SSRC.
 */
#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/error.hpp>

namespace nimrtc::rtp {

// -----------------------------------------------------------------------------
// The XR payload type
// -----------------------------------------------------------------------------

/** The RTCP payload type of an Extended Report (RFC 3611 §2). */
inline constexpr NIMRTC_API_STABLE std::uint8_t kXrPayloadType = 207;

// -----------------------------------------------------------------------------
// The block types (RFC 3611 §6.2)
// -----------------------------------------------------------------------------

/** The extended-report block types NimRTC knows about.
 *
 *  The values are the IANA "RTCP XR Block Type" registry (RFC 3611
 *  §6.2).  A block whose type is not listed here is *skipped*, not
 *  an error — that is what the block's length field is for. */
enum class NIMRTC_API_STABLE XrBlockType : std::uint8_t {
    kUnknown                = 0,
    kLossRle                = 1,   ///< RFC 3611 §4.1 — recognised, not parsed.
    kDuplicateRle           = 2,   ///< RFC 3611 §4.2 — recognised, not parsed.
    kPacketReceiptTimes     = 3,   ///< RFC 3611 §4.3 — recognised, not parsed.
    kReceiverReferenceTime  = 4,   ///< RFC 3611 §4.4 — parsed.
    kDlrr                   = 5,   ///< RFC 3611 §4.5 — parsed.
    kStatisticsSummary      = 6,   ///< RFC 3611 §4.6 — parsed.
    kVoipMetrics            = 7,   ///< RFC 3611 §4.7 — recognised, not parsed.
};

// -----------------------------------------------------------------------------
// The parsed blocks
// -----------------------------------------------------------------------------

/** A Receiver Reference Time Report Block (RFC 3611 §4.4, BT = 4).
 *
 *  The block carries the wall-clock instant at which it was sent, so
 *  that a peer receiving it can measure a round-trip time against the
 *  DLRR blocks it sends back. */
struct NIMRTC_API_STABLE XrReceiverReferenceTime {
    /// The NTP timestamp (RFC 5905 §6) of the instant the block was
    /// sent.  A sender with no wall-clock notion may set it to zero
    /// (RFC 3611 §4.4).
    std::uint64_t ntp_timestamp = 0;
};

/** One sub-block of a DLRR Report Block (RFC 3611 §4.5, BT = 5).
 *
 *  A DLRR block carries one sub-block per Receiver Reference Time
 *  Report Block the sender has received. */
struct NIMRTC_API_STABLE XrDlrrSubReport {
    /// The SSRC of the receiver whose Receiver Reference Time Report
    /// this sub-block answers.
    std::uint32_t ssrc = 0;

    /// The *middle* 32 bits of the NTP timestamp from that Receiver
    /// Reference Time Report (RFC 3611 §4.5).  Zero when no such
    /// report has been received.
    std::uint32_t last_rr = 0;

    /// The delay since that report, in 1/65536 s units.  Zero when no
    /// such report has been received.
    std::uint32_t dlrr = 0;
};

/** A DLRR Report Block (RFC 3611 §4.5, BT = 5). */
struct NIMRTC_API_STABLE XrDlrr {
    /// The sub-blocks, one per Receiver Reference Time Report received.
    std::vector<XrDlrrSubReport> sub_reports;
};

/** A Statistics Summary Report Block (RFC 3611 §4.6, BT = 6).
 *
 *  The block reports loss, duplicates, jitter and TTL / Hop Limit
 *  over a sequence-number interval.  The flag bits say which of the
 *  groups the block actually carries; the fields of an unreported
 *  group are present but zero, and the receiver MUST ignore them
 *  (RFC 3611 §4.6). */
struct NIMRTC_API_STABLE XrStatisticsSummary {
    /// The SSRC of the source the statistics describe.
    std::uint32_t ssrc = 0;

    /// The first sequence number of the reported interval.
    std::uint16_t begin_seq = 0;

    /// The last sequence number of the reported interval.
    std::uint16_t end_seq = 0;

    /// True when `lost_packets` carries a report.
    bool has_loss = false;

    /// The number of lost packets in the interval.
    std::uint32_t lost_packets = 0;

    /// True when `dup_packets` carries a report.
    bool has_duplicates = false;

    /// The number of duplicate packets in the interval.
    std::uint32_t dup_packets = 0;

    /// True when the four jitter fields carry a report.
    bool has_jitter = false;

    std::uint32_t min_jitter = 0;
    std::uint32_t max_jitter = 0;
    std::uint32_t mean_jitter = 0;
    std::uint32_t dev_jitter = 0;

    /// The TTL / Hop Limit report kind: 0 = none, 1 = IPv4 TTL,
    /// 2 = IPv6 Hop Limit (RFC 3611 §4.6).
    std::uint8_t ttl_or_hop_limit_kind = 0;

    std::uint8_t min_ttl_or_hl = 0;
    std::uint8_t max_ttl_or_hl = 0;
    std::uint8_t mean_ttl_or_hl = 0;
    std::uint8_t dev_ttl_or_hl = 0;
};

/** One decoded extended report block.
 *
 *  The payload variants are alternatives, not a union: exactly one of
 *  the three optional members is engaged, chosen by `type`. */
struct NIMRTC_API_STABLE XrBlock {
    /// The block type.  `kUnknown` for a block whose type NimRTC does
    /// not parse — such a block is reported so that a host can see it
    /// was there, but it carries no decoded fields.
    XrBlockType type = XrBlockType::kUnknown;

    /// The type-specific byte of the block header.  Its meaning
    /// depends on the block type (RFC 3611 §3).
    std::uint8_t type_specific = 0;

    /// The Receiver Reference Time block (engaged when `type` is
    /// `kReceiverReferenceTime`).
    XrReceiverReferenceTime receiver_reference_time;

    /// The DLRR block (engaged when `type` is `kDlrr`).
    XrDlrr dlrr;

    /// The Statistics Summary block (engaged when `type` is
    /// `kStatisticsSummary`).
    XrStatisticsSummary statistics_summary;
};

/** A decoded XR packet (RFC 3611 §2).
 *
 *  The packet carries the SSRC of its originator — one word more than
 *  the header of every other RTCP packet — and the stack of report
 *  blocks that follow it. */
struct NIMRTC_API_STABLE XrPacket {
    /// The SSRC of the packet's originator.
    std::uint32_t ssrc = 0;

    /// The report blocks, in wire order.
    std::vector<XrBlock> blocks;
};

// -----------------------------------------------------------------------------
// Parsing
// -----------------------------------------------------------------------------

/** Parse the extended report blocks of an XR packet.
 *
 *  @param raw  The whole RTCP sub-packet (the XR packet, header
 *              included), as returned by `demux_rtcp()`.
 *
 *  @returns The decoded blocks, or a `ProtocolError` when `raw` is not
 *           an XR packet at all (wrong payload type, wrong version, or
 *           truncated below the 8-byte XR header).
 *
 *  A block whose type this build does not parse is returned as a
 *  block with `type == kUnknown`; it is **not** an error.  A block
 *  whose *length* is inconsistent with the packet is a protocol
 *  error, because there is then no safe way to find the next block.
 *
 *  @note The XR packet's own length field delimits the blocks: a
 *       block may not extend past the end of the packet (RFC 3611 §2).
 */
[[nodiscard]] NIMRTC_API_STABLE core::Result<std::vector<XrBlock>>
parse_xr(core::ByteSpan raw) noexcept;

/** Parse an XR packet into its originator SSRC and its blocks.
 *
 *  @param raw  The whole RTCP sub-packet (the XR packet, header
 *              included), as returned by `demux_rtcp()`.
 *
 *  @returns The decoded packet, or a `ProtocolError` when `raw` is not
 *           a well-formed XR packet.  The error cases are exactly
 *           those of `parse_xr()`.
 *
 *  The originator SSRC is what a receiver echoes in the DLRR block it
 *  sends back (RFC 3611 §4.5), so a host that wants to answer a
 *  peer's Receiver Reference Time report needs it.
 */
[[nodiscard]] NIMRTC_API_STABLE core::Result<XrPacket>
parse_xr_packet(core::ByteSpan raw) noexcept;

/** Parse a single extended report block.
 *
 *  @param raw  The block, header included.
 *
 *  @returns The decoded block, or a `ProtocolError` when the block is
 *           truncated or its declared length does not match its
 *           contents.
 *
 *  A block type this build does not parse decodes successfully into a
 *  block with `type == kUnknown` — the length is still validated, so
 *  a malformed unknown block is still reported as an error.
 */
[[nodiscard]] NIMRTC_API_STABLE core::Result<XrBlock>
parse_xr_block(core::ByteSpan raw) noexcept;

// -----------------------------------------------------------------------------
// Building
// -----------------------------------------------------------------------------

/** Build an XR packet (RFC 3611 §2) out of report blocks.
 *
 *  @param ssrc      The SSRC of the packet's originator.
 *  @param blocks    The blocks to stack, in order.
 *
 *  @return The XR packet bytes, or an empty buffer when the packet
 *           would exceed the RTCP length field's 16-bit word count
 *           (RFC 3550 §6.1) — which cannot happen for the block
 *           counts a host can realistically produce, but is checked
 *           rather than assumed.
 */
[[nodiscard]] NIMRTC_API_STABLE core::ByteBuffer
build_xr(std::uint32_t ssrc, const std::vector<XrBlock>& blocks);

/** Build a Receiver Reference Time Report Block (RFC 3611 §4.4).
 *
 *  @param ntp_timestamp  The NTP timestamp of the sending instant.
 */
[[nodiscard]] NIMRTC_API_STABLE core::ByteBuffer
build_xr_receiver_reference_time(std::uint64_t ntp_timestamp);

/** Build a DLRR Report Block (RFC 3611 §4.5).
 *
 *  @param sub_reports  The sub-blocks, one per Receiver Reference
 *                      Time Report received.
 */
[[nodiscard]] NIMRTC_API_STABLE core::ByteBuffer
build_xr_dlrr(const std::vector<XrDlrrSubReport>& sub_reports);

/** Build a Statistics Summary Report Block (RFC 3611 §4.6).
 *
 *  @param summary  The statistics to report.  The flag bits are
 *                  derived from the `has_*` fields of the summary.
 */
[[nodiscard]] NIMRTC_API_STABLE core::ByteBuffer
build_xr_statistics_summary(const XrStatisticsSummary& summary);

}  // namespace nimrtc::rtp
