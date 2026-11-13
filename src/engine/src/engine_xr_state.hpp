/**
 * @file src/engine/src/engine_xr_state.hpp
 * @brief PRIVATE engine collaborator: the RTCP XR / TMMBR state.
 *
 * Architecture remediation plan §4.3 (D1 — engine split): this state
 * used to live as a dozen loose members inside `NimRTCEngine::Impl`,
 * which made the engine's internal header a catch-all and scattered
 * one protocol's state across the file.
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * ## What the state is
 *
 * Two RFCs' worth of negotiated state:
 *
 *   - **RFC 3611 (RTCP XR)** — the extended-report formats the peer and
 *     we negotiated, and the peer's Receiver Reference Time Report
 *     that our next DLRR answers;
 *   - **RFC 5104 (TMMBR / TMMBN)** — the bandwidth limit the peer
 *     imposed on us, and the acknowledgement we owe it.
 *
 * Grouping them is not cosmetic: the negotiated flags are the *authority*
 * on what may be sent (RFC 3611 §5.1, RFC 5104 §7.2 — "both offerer and
 * answerer MUST NOT use any feedback messages other than what both sides
 * have explicitly indicated as being supported"), and the flags only make
 * sense together with the state they gate.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace nimrtc::engine::detail {

/** The negotiated RTCP XR / TMMBR capabilities and the state they gate.
 *
 *  Not thread-safe: the engine's tick loop is the only writer and the
 *  RTCP path the only reader, on the same thread. */
struct XrState {
    // ---- The negotiated RTCP XR / TMMBR capabilities -------------------
    //
    // RFC 3611 §5.1 and RFC 5104 §7.2 both make the *negotiated*
    // description the authority on what may be sent:
    //
    //   * "When the 'rtcp-xr' attribute is present, participants SHOULD
    //      NOT send XR blocks other than the ones indicated by the
    //      parameters" (§5.1) — so an XR block is only sent when the
    //      peer asked for that format;
    //   * "both offerer and answerer MUST NOT use any feedback messages
    //      other than what both sides have explicitly indicated as being
    //      supported" (RFC 5104 §7.2) — so a TMMBR is only sent when
    //      the peer advertised the "ccm tmmbr" parameter.
    //
    // The flags below are read from the *remote* description (offer or
    // answer — the rule is symmetric, see the note on each flag).

    /** `peer_xr_rcvr_rtt` — the peer's description named the
     *  "rcvr-rtt" format.  The peer then declared that it is prepared to
     *  receive a Receiver Reference Time Report and to answer it with a
     *  DLRR, so we may send the initiator (RFC 3611 §5.2). */
    bool peer_xr_rcvr_rtt = false;

    /** `local_xr_rcvr_rtt` — *our* description named the "rcvr-rtt"
     *  format.
     *
     *  "rcvr-rtt" is a *collaborative* parameter (RFC 3611 §5.1): it
     *  declares that the writer is prepared to receive a Receiver
     *  Reference Time Report and to answer it with a DLRR.  So the DLRR
     *  we send is authorised by *our* declaration — the peer's
     *  declaration authorises the RRTR we send.
     *
     *  RFC 3611 §5.2: "By including a collaborative parameter in its
     *  answer, the answerer declares its ability to receive initiators
     *  and to send responses."  So the flag is set when the engine
     *  emits "rcvr-rtt" — in its offer or in its answer. */
    bool local_xr_rcvr_rtt = false;

    /** `peer_xr_stat_summary` — the peer's description named the
     *  "stat-summary" format.
     *
     *  Whoever writes the parameter is asking the *other* party to send
     *  the corresponding blocks (RFC 3611 §5.2), so the rule is the same
     *  in both offer and answer direction: the peer's "stat-summary"
     *  asks *us* for the blocks. */
    bool peer_xr_stat_summary = false;

    /** The `stat-summary` field indicators the peer asked for, already
     *  narrowed to the ones the engine produces ("loss" and "jitt").
     *
     *  RFC 3611 §5.1: "The 'stat-summary' parameter contains a list
     *  indicating which fields SHOULD be included in the Statistics
     *  Summary report blocks that are sent."  So the L / J flag bits of
     *  the block we send follow the peer's list: a peer that asked for
     *  "loss" gets a block with the loss flag set and the jitter flag
     *  clear. */
    std::string peer_xr_stat_summary_flags;

    /** `peer_tmmbr_negotiated` — the peer's description carried the
     *  "ccm tmmbr" parameter and the engine supports TMMBR, so the
     *  joint subset of RFC 5104 §7.2 includes it. */
    bool peer_tmmbr_negotiated = false;

    // ---- The outbound XR state ----------------------------------------
    //
    // The *outbound* XR state: the peer's Receiver Reference Time
    // report that our next DLRR answers (RFC 3611 §4.5), and the
    // instant we received it (the DLRR's delay field).
    //
    // `peer_rr_ssrc` is the SSRC of the *peer* that sent the Receiver
    // Reference Time Report; `peer_rr_mid_32` is the middle 32 bits of
    // that report's NTP timestamp.  The DLRR sub-block echoes both
    // (RFC 3611 §4.5).
    std::uint32_t peer_rr_ssrc    = 0;
    std::uint32_t peer_rr_mid_32  = 0;
    std::int64_t  peer_rr_recv_us = 0;

    /** When the last XR report went out (steady clock).  The XR report
     *  rides on the receiver-report cadence (RFC 3611 §5.2: an XR
     *  packet is sent no more often than the RTCP interval). */
    std::chrono::steady_clock::time_point xr_last_sent{};

    /** The number of outbound XR reports the engine sent. */
    std::uint64_t xr_reports_sent = 0;

    // ---- The inbound TMMBR limit --------------------------------------
    //
    // A peer that sends us a TMMBR is telling us "you may not send me
    // more than this".  The engine honours the limit: the scheduler's
    // budget is clamped to it, so the engine obeys the very limit it
    // acknowledges with a TMMBN.
    //
    // 0 = no limit received.  The value is the *lowest* limit any
    // peer's TMMBR has asked for: the tightest constraint wins
    // (RFC 5104 §4.2.1.2 — the receiver reports the highest bit rate
    // it *currently* supports).
    std::uint32_t peer_tmmbr_limit_bps = 0;

    /** The SSRC of the peer that sent the last TMMBR.
     *
     *  It is the *owner* of the bounding tuple the TMMBN reports
     *  (RFC 5104 §4.2.2.1): in the unicast case the receiver that
     *  issued the TMMBR becomes the owner of the tuple as soon as the
     *  sender acknowledges it (§4.2.2.2). */
    std::uint32_t tmmbr_peer_ssrc = 0;

    /** The number of inbound TMMBR messages the engine honoured. */
    std::uint64_t tmmbr_received = 0;

    /** The number of outbound TMMBR messages. */
    std::uint64_t tmmbr_sent = 0;

    /** The number of outbound TMMBN messages. */
    std::uint64_t tmmbn_sent = 0;

    /** A TMMBN is pending: the peer asked us to confirm the bounding
     *  set (RFC 5104 §4.2.2.2).  Only one TMMBN is sent even if
     *  several TMMBRs arrive in between (RFC 5104 §4.2.2.2). */
    bool pending_tmmbn = false;

    /** The bit rate the last outbound TMMBR reported, in bits per
     *  second.  It is the observable that proves the TMMBR follows the
     *  bandwidth estimate rather than a stale number. */
    std::uint32_t last_tmmbr_bps = 0;

    /** When the last TMMBR went out (steady clock).  RFC 5104
     *  §4.2.1.3: a repeated request uses the regular RTCP timing, so
     *  the engine repeats it on the receiver-report cadence rather than
     *  on every estimate change. */
    std::chrono::steady_clock::time_point tmmbr_last_sent{};
};

}  // namespace nimrtc::engine::detail
