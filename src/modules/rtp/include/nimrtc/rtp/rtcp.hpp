/**
 * @file nimrtc/rtp/rtcp.hpp
 * @brief RTCP compound-packet demultiplexing and the REMB feedback message.
 *
 * v1.2.0 M1 (F1 / F4 / F7 groundwork).
 *
 * ## Why this file exists
 *
 * An RTCP datagram on the wire is a *compound* packet: a concatenation
 * of sub-packets that share one datagram but carry different kinds of
 * information (RFC 3550 §6.1):
 *
 *   - SR (PT 200)  — the peer's send-side statistics and its NTP anchor;
 *   - RR (PT 201)  — the peer's view of *our* stream: loss, jitter, RTT;
 *   - RTPFB (PT 205) — generic RTP feedback: NACK (FMT 1) and
 *     transport-wide congestion control (FMT 15);
 *   - PSFB (PT 206) — payload-specific feedback: PLI / FIR / SLI and
 *     REMB (FMT 15).
 *
 * Until now the engine walked the compound by hand, in
 * `engine_media.cpp::handle_rtcp()`, and only two of the kinds were
 * understood: SR (for the cross-end timeline) and whatever the video
 * sender plugin chose to consume.  Every other kind was dropped on the
 * floor.
 *
 * This file provides the two pieces that the engine needs and that
 * belong to the *wire format* rather than to the engine's policy:
 *
 *   1. `demux_rtcp()` — split a compound datagram into its sub-packets
 *      and classify each one by payload type.  This is the routing
 *      table of F1.
 *   2. `parse_remb()` — decode the Receiver Estimated Maximum Bitrate
 *      message (draft-alvestrand-rmcat-remb-03).
 *
 * Both are pure functions over a byte span: they own no state and they
 * never allocate beyond their result.
 *
 * ## REMB on the wire
 *
 * REMB is a PSFB message with FMT = 15 (PT 206).  Its payload is:
 *
 * ```
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |V=2|P| FMT=15  |   PT=206     |             length              |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                  SSRC of packet sender                        |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                    SSRC of media source (0)                   |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |  Unique identifier 'R' 'E' 'M' 'B'                            |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |  Num SSRC     | BR Exp       |        BR Mantissa            |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |   SSRC feedback                                               |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * ```
 *
 * The bitrate is `mantissa << exponent`, where the exponent is the
 * 6-bit "BR Exp" field and the mantissa is the 18-bit "BR Mantissa"
 * field (the two most significant bits of the mantissa share the byte
 * with the exponent).  This is the encoding libwebrtc implements in
 * `modules/rtp_rtcp/source/rtcp_packet/remb.cc` and the one Chrome
 * puts on the wire.
 *
 * @see docs/adr/ADR-018-bandwidth-signal-policy.md
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
// The compound demultiplexer (RFC 3550 §6.1)
// -----------------------------------------------------------------------------

/** The kind of an RTCP sub-packet, as determined by its payload type.
 *
 *  The payload type is the only field that decides this; the FMT (or
 *  RC) field refines it further *within* a kind — a PSFB block may be
 *  a PLI, a FIR or a REMB, and `RtcpBlock::fmt` says which. */
enum class NIMRTC_API_STABLE RtcpBlockKind : std::uint8_t {
    kSenderReport,            ///< PT 200 — Sender Report.
    kReceiverReport,          ///< PT 201 — Receiver Report.
    kSourceDescription,       ///< PT 202 — SDES.
    kGoodbye,                 ///< PT 203 — BYE.
    kApplicationDefined,      ///< PT 204 — APP.
    kRtpFeedback,             ///< PT 205 — RTPFB (NACK, transport-cc).
    kPayloadSpecificFeedback, ///< PT 206 — PSFB (PLI, FIR, SLI, REMB).
    kExtendedReport,          ///< PT 207 — XR.
    kUnknown,                 ///< A payload type this build does not know.
};

/** One sub-packet of an RTCP compound datagram.
 *
 *  `packet` points into the caller's buffer — it is a view, not a copy
 *  — and it covers the whole sub-packet, header included.  The RTCP
 *  padding, if the sub-packet carries any, is part of it; the
 *  per-message parsers deal with it themselves. */
struct NIMRTC_API_STABLE RtcpBlock {
    RtcpBlockKind kind = RtcpBlockKind::kUnknown;

    /// The RC field (SR / RR) or the FMT field (the feedback kinds).
    std::uint8_t fmt = 0;

    /// The RTCP payload type (200..207).
    std::uint8_t payload_type = 0;

    /// The whole sub-packet, header included.
    core::ByteSpan packet{};
};

/** Split an RTCP compound datagram into its sub-packets.
 *
 *  The walk follows RFC 3550 §6.1: each sub-packet starts with a
 *  4-byte header whose length field counts 32-bit words *minus one*.
 *
 *  The walk is *forgiving*: a sub-packet whose length field does not
 *  fit in what is left of the datagram, or whose version is not 2,
 *  ends the walk.  Everything before it is still returned.  A malformed
 *  tail must not throw away the well-formed head of the datagram, and
 *  it is not worth failing the receive path over.
 *
 *  An empty datagram yields an empty vector. */
[[nodiscard]] NIMRTC_API_STABLE std::vector<RtcpBlock>
demux_rtcp(core::ByteSpan compound) noexcept;

/// Split an RTCP compound datagram into a caller-owned vector — the
/// allocation-free form of `demux_rtcp()`.
///
/// The receive path demultiplexes every inbound RTCP datagram, and the
/// returned vector used to be a fresh heap allocation per datagram.
/// A host that keeps one vector around and reuses it pays no
/// allocation at all; the vector's capacity grows to the high-water
/// mark and is then reused for every datagram.
///
/// The vector is **cleared** before the walk, so it holds exactly the
/// blocks of this datagram and nothing else.  The blocks' `packet`
/// spans still point into @p compound (the caller owns that buffer),
/// exactly as in the returning overload.
///
/// @param compound  The compound datagram.
/// @param out       The destination vector; cleared and then filled.
void demux_rtcp(core::ByteSpan compound,
                std::vector<RtcpBlock>& out) noexcept;

// -----------------------------------------------------------------------------
// REMB — Receiver Estimated Maximum Bitrate
// -----------------------------------------------------------------------------

/** A decoded REMB message.
 *
 *  `bitrate_bps` is the receiver's estimate of the total available
 *  bandwidth for the session — a *session*-level figure, not a
 *  per-stream one (draft-alvestrand-rmcat-remb-03 §2.1). */
struct NIMRTC_API_STABLE Remb {
    /// The SSRC of the feedback sender.
    std::uint32_t sender_ssrc = 0;

    /// The SSRC of the media source.  The draft says it is unused and
    /// SHALL be 0.
    std::uint32_t media_ssrc = 0;

    /// The estimated available bandwidth, in bits per second.
    std::uint64_t bitrate_bps = 0;

    /// The SSRCs the estimate applies to.  May be empty.
    std::vector<std::uint32_t> ssrcs;
};

/** Decode a REMB message (PSFB, FMT = 15).
 *
 *  @param raw  The whole RTCP sub-packet, header included.
 *
 *  @returns The decoded message, or a `ProtocolError` when the packet
 *           is not a REMB, is truncated, or does not carry the
 *           'R''E''M''B' identifier. */
[[nodiscard]] NIMRTC_API_STABLE core::Result<Remb>
parse_remb(core::ByteSpan raw) noexcept;

}  // namespace nimrtc::rtp
