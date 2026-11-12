/**
 * @file nimrtc/rtp/rtcp_tmmbr.hpp
 * @brief TMMBR / TMMBN — the temporary maximum media bit rate messages
 *        (RFC 5104 §4.2).
 *
 * v1.2.0 (session capability plan X3 / X4).
 *
 * ## Why this file exists
 *
 * RFC 5104 adds two feedback messages to the RTPFB packet type:
 *
 *  - **TMMBR** (FMT = 3) — "I can only receive this much"; a receiver
 *    tells a sender the maximum bit rate it supports;
 *  - **TMMBN** (FMT = 4) — "these are the bounding tuples"; a sender
 *    answers with the set of constraints that actually apply.
 *
 * Both carry one or more *FCI entries*, and each entry is the same
 * eight-byte tuple:
 *
 * ```text
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                              SSRC                             |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | MxTBR Exp |  MxTBR Mantissa                 |Measured Overhead|
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * ```
 *
 * The bit rate is `mantissa * 2^exp` (RFC 5104 §4.2.1.1) — the same
 * exponential encoding REMB uses, with a 17-bit mantissa and a 6-bit
 * exponent.
 *
 * ## Why the engine sends TMMBR
 *
 * A TMMBR that reports a bit rate the sender does not actually obey
 * is worse than no TMMBR at all: the peer would throttle to a limit
 * we are not honouring.  The engine therefore reports the bit rate
 * its bandwidth estimate is *currently* using, so the two agree.
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
// The FCI entry
// -----------------------------------------------------------------------------

/** One TMMBR / TMMBN FCI entry (RFC 5104 §4.2.1.1 / §4.2.2.1). */
struct NIMRTC_API_STABLE TmmbrEntry {
    /** The SSRC of the media sender the tuple applies to (TMMBR) or
     *  the owner of the tuple (TMMBN). */
    std::uint32_t ssrc = 0;

    /** The maximum total media bit rate, in bits per second.
     *
     *  On the wire this is `mantissa * 2^exp`; the struct carries the
     *  decoded value and the encoder does the conversion. */
    std::uint64_t bitrate_bps = 0;

    /** The measured average packet overhead, in bytes (0..511).
     *
     *  RFC 5104 §4.2.1.2 defines it as a running average of the
     *  observed per-packet overhead.  NimRTC does not measure it, so
     *  it reports 0 — the field is informational. */
    std::uint16_t measured_overhead_bytes = 0;
};

// -----------------------------------------------------------------------------
// The messages
// -----------------------------------------------------------------------------

/** A decoded TMMBR / TMMBN message.
 *
 *  The FCI entries are the payload; the sender's SSRC is in the
 *  common feedback header (RFC 4585 §6.1), and it is what a peer
 *  needs in order to know *who* asked for the limit. */
struct NIMRTC_API_STABLE TmmbrMessage {
    /// The SSRC of the packet sender — the reporting entity for a
    /// TMMBR, the notifying media sender for a TMMBN.
    std::uint32_t sender_ssrc = 0;

    /// The FCI entries, in wire order.
    std::vector<TmmbrEntry> entries;
};

// -----------------------------------------------------------------------------
// Parsing
// -----------------------------------------------------------------------------

/** Parse a TMMBR message (RTPFB, FMT = 3).
 *
 *  @param raw  The whole RTCP sub-packet, header included.
 *
 *  @returns The FCI entries, or a `ProtocolError` when the packet is
 *           not a TMMBR or is truncated.
 */
[[nodiscard]] NIMRTC_API_STABLE core::Result<std::vector<TmmbrEntry>>
parse_tmmbr(core::ByteSpan raw) noexcept;

/** Parse a TMMBN message (RTPFB, FMT = 4).
 *
 *  @param raw  The whole RTCP sub-packet, header included.
 *
 *  @returns The FCI entries, or a `ProtocolError` when the packet is
 *           not a TMMBN or is truncated.  A TMMBN with no FCI entry
 *           is legal (RFC 5104 §4.2.2.2) and yields an empty vector.
 */
[[nodiscard]] NIMRTC_API_STABLE core::Result<std::vector<TmmbrEntry>>
parse_tmmbn(core::ByteSpan raw) noexcept;

/** Parse a TMMBR message (RTPFB, FMT = 3) *with* its sender.
 *
 *  @param raw  The whole RTCP sub-packet, header included.
 *
 *  @returns The decoded message, or a `ProtocolError` when the packet
 *           is not a TMMBR or is truncated.  The error cases are
 *           exactly those of `parse_tmmbr()`.
 */
[[nodiscard]] NIMRTC_API_STABLE core::Result<TmmbrMessage>
parse_tmmbr_message(core::ByteSpan raw) noexcept;

/** Parse a TMMBN message (RTPFB, FMT = 4) *with* its sender.
 *
 *  @param raw  The whole RTCP sub-packet, header included.
 *
 *  @returns The decoded message, or a `ProtocolError` when the packet
 *           is not a TMMBN or is truncated.  The error cases are
 *           exactly those of `parse_tmmbn()`.
 */
[[nodiscard]] NIMRTC_API_STABLE core::Result<TmmbrMessage>
parse_tmmbn_message(core::ByteSpan raw) noexcept;

// -----------------------------------------------------------------------------
// Building
// -----------------------------------------------------------------------------

/** Build a TMMBR message (RTPFB, FMT = 3).
 *
 *  @param sender_ssrc  The SSRC of the feedback sender.
 *  @param entries      The FCI entries.  RFC 5104 §4.2.1.1 requires at
 *                      least one; an empty list yields an empty buffer.
 */
[[nodiscard]] NIMRTC_API_STABLE core::ByteBuffer
build_tmmbr(std::uint32_t sender_ssrc, const std::vector<TmmbrEntry>& entries);

/** Build a TMMBN message (RTPFB, FMT = 4).
 *
 *  @param sender_ssrc  The SSRC of the feedback sender.
 *  @param entries      The FCI entries.  May be empty: an empty TMMBN
 *                      is how a sender says "no tuple applies any
 *                      more" (RFC 5104 §4.2.2.2).
 */
[[nodiscard]] NIMRTC_API_STABLE core::ByteBuffer
build_tmmbn(std::uint32_t sender_ssrc, const std::vector<TmmbrEntry>& entries);

}  // namespace nimrtc::rtp
