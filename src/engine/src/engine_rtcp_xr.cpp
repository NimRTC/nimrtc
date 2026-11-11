/**
 * @file src/engine/src/engine_rtcp_xr.cpp
 * @brief The outbound RTCP Extended Report and the TMMBR / TMMBN plane.
 *
 * v1.2.0 (session capability plan X2 / X4 / X5).
 *
 * Three things live here, and they are the *outbound* half of the
 * RTCP XR / TMMBR story:
 *
 *   1. `maybe_send_extended_report()` — the RFC 3611 extended report.
 *      It carries the **Receiver Reference Time** block (BT = 4) that
 *      stamps the sending instant, and the **DLRR** block (BT = 5) that
 *      answers the peer's own Receiver Reference Time report (RFC 3611
 *      §4.5).  The DLRR is only meaningful once the peer's report has
 *      been received — before that there is nothing to answer.
 *
 *   2. `maybe_send_tmmbr()` — the RFC 5104 TMMBR, the "you may not
 *      send me more than this" request.  The rate it reports is the
 *      bandwidth estimate the engine is *currently* using, so the peer
 *      throttles to a number we actually honour.
 *
 *   3. `flush_tmmbn()` — the RFC 5104 TMMBN, the answer to an inbound
 *      TMMBR.  RFC 5104 §4.2.2.2 requires exactly one TMMBN per
 *      batch of TMMBRs, and it must be sent even when the new tuple
 *      is *not* accepted into the bounding set — that is what lets a
 *      participant that missed the last TMMBN recover the state.
 *
 * ## Why the report and the request live together
 *
 * They share one cadence (the RTCP interval) and one send path
 * (SRTCP-protected RTCP over ICE), and — more importantly — they
 * answer the same question from the two sides: "what bit rate is this
 * session actually running at".  The XR report tells the peer what we
 * *measured*; the TMMBR tells it what it *may send*.
 *
 * @see docs/plan/session-capability-plan.md (Track X)
 */
#include "engine_impl.hpp"

#include <nimrtc/rtp/rtcp_tmmbr.hpp>
#include <nimrtc/rtp/rtcp_xr.hpp>
#include <nimrtc/rtp/timeline.hpp>   // rtp::Ntp64 / ntp64_from_system_clock

namespace nimrtc::engine {

namespace {

/** The cadence of the outbound XR report and of the TMMBR.
 *
 *  RFC 3611 §5.2: an XR packet is sent no more often than the RTCP
 *  reporting interval — the XR packet rides along with the regular
 *  RTCP traffic rather than forming a stream of its own.  The same
 *  cadence serves the TMMBR: RFC 5104 §4.2.1.3 says a repeated
 *  request uses the regular RTCP mode for its transmission timing.
 *
 *  The value matches the receiver report's one-second cadence (see
 *  `engine_rtcp.cpp`): a peer that never hears from us thinks the
 *  session died. */
constexpr std::int64_t kXrPeriodMs = 1000;

/** The refresh interval of an unchanged TMMBR.
 *
 *  RFC 5104 §4.2.1.2: "A TMMBR FCI entry MAY be repeated in subsequent
 *  TMMBR messages if no TMMBN message has been received from the media
 *  sender at the time of transmission of the next RTCP packet."  So a
 *  TMMBR whose rate has not changed is still repeated from time to
 *  time — the peer may have missed the previous one.
 *
 *  Five RTCP intervals is the RFC 4585 §3.4 participant timeout, so a
 *  peer that is alive at all has answered by then. */
constexpr std::int64_t kTmmbrRefreshMs = 5 * kXrPeriodMs;

}  // namespace


// ---------------------------------------------------------------------------
// The reporter SSRC
// ---------------------------------------------------------------------------

std::uint32_t NimRTCEngine::rtcp_reporter_ssrc() noexcept {
    // Our own SSRC as the originator of the RTCP reports (RFC 3550
    // §6.1).  It is the *same* SSRC the receiver report uses: both
    // identify us as the party making the statement.  The media SSRCs
    // identify the streams, not the reporter.
    if (impl_->rtcp_sender_ssrc_ == 0) {
        std::random_device rd;
        std::mt19937_64 mt(rd());
        impl_->rtcp_sender_ssrc_ = static_cast<std::uint32_t>(mt() & 0xFFFFFFFFu);
        if (impl_->rtcp_sender_ssrc_ == 0) impl_->rtcp_sender_ssrc_ = 1;
    }
    return impl_->rtcp_sender_ssrc_;
}


// ---------------------------------------------------------------------------
// The outbound send path (SRTCP + ICE)
// ---------------------------------------------------------------------------

bool NimRTCEngine::send_rtcp_packet(std::uint32_t ssrc,
                                    const core::ByteBuffer& bytes) noexcept {
    if (bytes.empty()) return false;

    core::ByteSpan out(bytes.data(), bytes.size());
    std::vector<std::uint8_t> enc;
    // RTCP leaves the engine as SRTCP (RFC 3711 §3.4) once SRTP is
    // keyed — a *different* crypto context from SRTP, so
    // `protect_rtp()` would produce a packet the peer's
    // `unprotect_rtcp()` rejects.
    //
    // The session is keyed by the SSRC the packet is sent under: for a
    // report it is the engine's own RTCP SSRC (RFC 3550 §6.1); for a
    // feedback message it is the SSRC of the *media* sender the message
    // is about (RFC 4585 §6.1).
    if (impl_->srtp_installed && impl_->srtp) {
        auto* sess = impl_->srtp->get_session(ssrc, /*outgoing=*/true);
        if (!sess) return false;   // SRTP installed but no keying yet — skip.
        auto r = sess->protect_rtcp(out);
        if (!r) {
            core::log::Logger::instance().warn(
                "engine[rtcp]: protect_rtcp failed; RTCP packet dropped");
            return false;
        }
        enc.assign(r.value().data(), r.value().data() + r.value().size());
        out = core::ByteSpan(enc.data(), enc.size());
    }

    plugins::BufferView bv{out.data(), out.size()};
    if (ice_t_->send(bv, {}) != plugins::kOk) {
        core::log::Logger::instance().warn(
            "engine[rtcp]: RTCP send failed");
        return false;
    }
    return true;
}


// ---------------------------------------------------------------------------
// The extended report (RFC 3611 §4.4 / §4.5)
// ---------------------------------------------------------------------------

void NimRTCEngine::maybe_send_extended_report() noexcept {
    if (!config_.rtcp_xr_enabled) return;
    if (!ice_t_ || !is_ice_connected()) return;

    // ---- The negotiated gate (RFC 3611 §5.1 / §5.2) --------------------
    //
    // "When the 'rtcp-xr' attribute is present, participants SHOULD NOT
    // send XR blocks other than the ones indicated by the parameters."
    //
    // So each block is gated on the peer having named the format that
    // authorises it:
    //
    //   * the Receiver Reference Time block (BT = 4) is the *initiator*
    //     of the round-trip measurement; RFC 3611 §5.2 lets it be sent
    //     when the peer declared "rcvr-rtt", i.e. that it is prepared to
    //     receive the initiator and to answer it with a DLRR;
    //   * the DLRR block (BT = 5) is the *response*; it is owed as soon
    //     as the peer's own Receiver Reference Time report has been
    //     received (RFC 3611 §4.5), and it is only ever sent to a peer
    //     that asked for the collaboration;
    //   * the Statistics Summary block (BT = 6) is *unilateral*: the
    //     peer's "stat-summary" is a request for *us* to send it.
    // "rcvr-rtt" is collaborative, and the two halves of the exchange
    // are authorised by *different* declarations (RFC 3611 §5.1):
    //
    //   * the peer's "rcvr-rtt" declares that the *peer* is prepared to
    //     receive an initiator and to reply with a response — so it
    //     authorises us to send the RRTR;
    //   * *our* "rcvr-rtt" declares that *we* are prepared to receive
    //     an initiator and to reply with a response — so it authorises
    //     us to send the DLRR.
    const bool may_send_rrtr = impl_->peer_xr_rcvr_rtt;
    const bool may_send_dlrr = impl_->local_xr_rcvr_rtt &&
                               impl_->peer_rr_ssrc != 0;
    const bool may_send_stat = impl_->peer_xr_stat_summary &&
                               impl_->audio_rx.ssrc != 0;

    if (!may_send_rrtr && !may_send_dlrr && !may_send_stat) return;

    const auto now = core::SteadyClock::now();
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - impl_->xr_last_sent)
            .count();
    if (elapsed_ms < kXrPeriodMs) return;
    impl_->xr_last_sent = now;

    const std::uint32_t sender_ssrc = rtcp_reporter_ssrc();

    std::vector<rtp::XrBlock> blocks;

    // ---- The Receiver Reference Time block (RFC 3611 §4.4) --------------
    //
    // It carries the wall-clock instant at which the report was sent.
    // It is what makes the DLRR exchange possible at all: the peer
    // echoes our timestamp back in its own DLRR, and the difference
    // between the two is the round-trip time.
    if (may_send_rrtr) {
        rtp::XrBlock block;
        block.type = rtp::XrBlockType::kReceiverReferenceTime;
        block.receiver_reference_time.ntp_timestamp =
            rtp::ntp64_from_system_clock().value;
        blocks.push_back(block);
    }

    // ---- The DLRR block (RFC 3611 §4.5) ---------------------------------
    //
    // One sub-block per Receiver Reference Time Report we received.
    // Without one there is nothing to answer, so the block is omitted
    // entirely — an empty DLRR would tell the peer nothing and would
    // only cost bytes.
    //
    // The sub-block's SSRC is the SSRC of the *receiver* whose report
    // is being answered.  RFC 3611 §4.5 names the field "SSRC of first
    // receiver" and the peer computes its own round-trip time by
    // matching the sub-block against its own reports — so the value
    // here is the originator SSRC of the peer's Receiver Reference
    // Time Report, which is exactly `peer_rr_ssrc`.
    if (may_send_dlrr) {
        rtp::XrDlrrSubReport sub;
        sub.ssrc = impl_->peer_rr_ssrc;
        // The middle 32 bits of the NTP timestamp from that report
        // (RFC 3611 §4.5 — the "last RR" field).
        sub.last_rr = impl_->peer_rr_mid_32;
        // The delay since that report, in 1/65536 s units.  Zero when
        // the report has just arrived (or the clock went backwards).
        const std::int64_t now_us = detail::steady_now_us();
        const std::int64_t delta_us = now_us - impl_->peer_rr_recv_us;
        if (delta_us > 0) {
            sub.dlrr = static_cast<std::uint32_t>(
                (delta_us * 65536) / 1000000);
        }

        rtp::XrBlock block;
        block.type = rtp::XrBlockType::kDlrr;
        block.dlrr.sub_reports.push_back(sub);
        blocks.push_back(block);
    }

    // ---- The Statistics Summary block (RFC 3611 §4.6) -------------------
    //
    // The summary describes the *inbound* stream we receive, so it is
    // only meaningful once the peer has actually sent us media.  The
    // fields come from the receiver report's own bookkeeping (the
    // sequence space, the cumulative loss and the jitter), so the
    // numbers the peer sees in the summary are the same ones the
    // receiver report carries.
    if (may_send_stat) {
        const Impl::AudioRxState& rx = impl_->audio_rx;

        // The interval is the sequence space since the last summary
        // (RFC 3611 §4.6 — begin_seq / end_seq bracket the reported
        // interval).  The engine reports the whole space it has seen
        // so far; the sequence numbers are 16-bit and wrap, which is
        // exactly what the field is defined as (RFC 3550 A.1).
        const std::uint64_t expected =
            static_cast<std::uint64_t>(rx.max_seq - rx.base_seq) + 1u;
        const std::int64_t lost =
            static_cast<std::int64_t>(expected) -
            static_cast<std::int64_t>(rx.received);

        // RFC 3611 §5.1: the "stat-summary" parameter lists the fields
        // that SHOULD be included in the blocks that are sent.  So the
        // L / J flag bits follow the peer's list: a peer that asked for
        // "loss" only gets the loss flag set, and the jitter group is
        // reported as absent (its fields MUST then be zero — RFC 3611
        // §4.6).
        const std::string& flags = impl_->peer_xr_stat_summary_flags;
        const bool want_loss   = flags.find("loss") != std::string::npos;
        const bool want_jitter = flags.find("jitt") != std::string::npos;

        rtp::XrStatisticsSummary summary;
        summary.ssrc           = rx.ssrc;
        summary.begin_seq      = static_cast<std::uint16_t>(rx.base_seq);
        summary.end_seq        = static_cast<std::uint16_t>(rx.max_seq);
        summary.has_loss       = want_loss;
        // The cumulative loss is a signed quantity (a duplicate or a
        // reordered packet makes it negative); the wire field is
        // unsigned, so a negative value is clamped to zero — RFC 3611
        // §4.6 reports a *count* of lost packets, and a negative count
        // has no representation.
        summary.lost_packets   = lost > 0 ? static_cast<std::uint32_t>(lost) : 0u;
        summary.has_jitter     = want_jitter;
        if (want_jitter) {
            summary.min_jitter  = rx.jitter;
            summary.max_jitter  = rx.jitter;
            summary.mean_jitter = rx.jitter;
            summary.dev_jitter  = 0;
        }

        rtp::XrBlock block;
        block.type = rtp::XrBlockType::kStatisticsSummary;
        block.statistics_summary = summary;
        blocks.push_back(block);
    }

    const core::ByteBuffer packet = rtp::build_xr(sender_ssrc, blocks);
    if (packet.empty()) return;

    if (!send_rtcp_packet(sender_ssrc, packet)) return;

    ++impl_->xr_reports_sent;

    core::log::Logger::instance().debug(
        "engine[xr]: extended report sent (" +
        std::to_string(blocks.size()) + " block(s))");
}


// ---------------------------------------------------------------------------
// The TMMBR (RFC 5104 §4.2.1)
// ---------------------------------------------------------------------------

void NimRTCEngine::maybe_send_tmmbr() noexcept {
    // The feature is opt-in (EngineConfig::tmmbr_enabled), and
    // `disable_tmmbr` overrides it in either direction.
    if (!config_.tmmbr_enabled || config_.disable_tmmbr) return;
    if (!ice_t_ || !is_ice_connected()) return;

    // RFC 5104 §7.2: "both offerer and answerer MUST NOT use any
    // feedback messages other than what both sides have explicitly
    // indicated as being supported.  In other words, only the joint
    // subset of CCM parameters from the offer and answer may be
    // used."  So the peer's description must have carried the
    // "ccm tmmbr" parameter for a TMMBR to be legal at all — the
    // engine's own configuration is the other half of the intersection.
    if (!impl_->peer_tmmbr_negotiated) return;

    // The rate we report is the rate we are *using*.  A TMMBR that
    // announces a limit the sender does not obey is worse than no
    // TMMBR at all — the peer would throttle to a number we are not
    // honouring (RFC 5104 §4.2.1.2).
    if (!bwe_) return;
    const std::uint32_t target_bps = bwe_->estimate().target_bitrate_bps;
    if (target_bps == 0) {
        // No estimate yet: the estimator has not heard from the
        // network.  Reporting "0" would tell the peer to stop sending
        // entirely, which is not what "no opinion" means.
        return;
    }

    // RFC 5104 §4.2.1.2: "A TMMBR FCI entry MAY be repeated in
    // subsequent TMMBR messages if no TMMBN message has been received
    // from the media sender at the time of transmission of the next RTCP
    // packet."
    //
    // So there are two cadences:
    //
    //   * a *changed* rate is reported at the next RTCP slot — it is
    //     new information and the peer should have it as soon as the
    //     regular feedback cadence allows;
    //   * an *unchanged* rate is repeated only occasionally, because
    //     the peer may have missed the previous one.  Repeating it at
    //     every RTCP interval would be pure overhead.
    const auto now = core::SteadyClock::now();
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - impl_->tmmbr_last_sent)
            .count();
    const bool rate_changed = (target_bps != impl_->last_tmmbr_bps);
    const std::int64_t gate_ms = rate_changed ? kXrPeriodMs : kTmmbrRefreshMs;
    if (elapsed_ms < gate_ms) return;

    // The FCI entry names the *media* sender the tuple applies to
    // (RFC 5104 §4.2.1.2).  The peer is the one sending us media, so
    // the tuple applies to the peer's stream — whose SSRC we learn
    // from the inbound RTP header.  Until we have seen a packet there
    // is no media source to name, and a tuple about an unknown source
    // would be meaningless to the peer.
    if (impl_->audio_rx.ssrc == 0) return;

    rtp::TmmbrEntry entry;
    entry.ssrc = impl_->audio_rx.ssrc;
    entry.bitrate_bps = target_bps;

    // The feedback sender's SSRC is the engine's own RTCP SSRC: the
    // TMMBR is a statement *we* make (RFC 4585 §6.1 — the "SSRC of
    // packet sender" field indicates the source of the request).
    const std::uint32_t feedback_ssrc = rtcp_reporter_ssrc();

    const core::ByteBuffer packet = rtp::build_tmmbr(feedback_ssrc, {entry});
    if (packet.empty()) return;

    if (!send_rtcp_packet(feedback_ssrc, packet)) return;

    impl_->tmmbr_last_sent = now;
    impl_->last_tmmbr_bps  = target_bps;
    ++impl_->tmmbr_sent;

    core::log::Logger::instance().info(
        "engine[tmmbr]: TMMBR sent, limit=" + std::to_string(target_bps) + " bps");
}


// ---------------------------------------------------------------------------
// The TMMBN (RFC 5104 §4.2.2)
// ---------------------------------------------------------------------------

void NimRTCEngine::flush_tmmbn() noexcept {
    if (!impl_->pending_tmmbn) return;
    if (!ice_t_ || !is_ice_connected()) return;

    // Only one TMMBN is sent even if several TMMBRs arrived in
    // between (RFC 5104 §4.2.2.2) — so the pending flag is cleared
    // *before* the send, and a send failure does not leave a stale
    // pending flag behind.
    impl_->pending_tmmbn = false;

    // The bounding set: the tuple the peer asked for, owned by *us*.
    // RFC 5104 §4.2.2.1 — the TMMBN FCI entry gives the **owner** of
    // the tuple, and the owner of the limit the peer asked us to obey
    // is the peer itself (it is the receiver whose limitation we are
    // honouring).  In the unicast case (RFC 5104 §4.2.2.2) the
    // receiver becomes the owner as soon as it has issued its first
    // TMMBR, so the owner is the TMMBR's sender SSRC.
    rtp::TmmbrEntry entry;
    entry.ssrc = impl_->tmmbr_peer_ssrc;
    entry.bitrate_bps = impl_->peer_tmmbr_limit_bps;

    const std::uint32_t feedback_ssrc = rtcp_reporter_ssrc();

    const core::ByteBuffer packet = rtp::build_tmmbn(feedback_ssrc, {entry});
    if (packet.empty()) return;

    if (!send_rtcp_packet(feedback_ssrc, packet)) return;

    ++impl_->tmmbn_sent;

    core::log::Logger::instance().info(
        "engine[tmmbr]: TMMBN sent, limit=" +
        std::to_string(impl_->peer_tmmbr_limit_bps) + " bps");
}


// ---------------------------------------------------------------------------
// The inbound TMMBR (RFC 5104 §4.2.1.2, from the media-sender's side)
// ---------------------------------------------------------------------------

void NimRTCEngine::handle_inbound_tmmbr(core::ByteSpan packet) noexcept {
    // A malformed TMMBR is counted, not fatal: one broken packet from
    // the peer must not take the read path down.
    auto msg = rtp::parse_tmmbr_message(packet);
    if (!msg) {
        core::log::Logger::instance().warn(
            "engine[tmmbr]: malformed inbound TMMBR ignored");
        return;
    }

    // Every FCI entry is a tuple about a media sender (RFC 5104
    // §4.2.1.1).  A message may carry several of them — the multicast
    // and translator topologies need that — so each is handled on its
    // own.
    for (const rtp::TmmbrEntry& entry : msg.value().entries) {
        on_tmmbr_received(msg.value().sender_ssrc, entry.ssrc,
                          static_cast<std::uint32_t>(
                              entry.bitrate_bps > 0xFFFFFFFFull
                                  ? 0xFFFFFFFFull
                                  : entry.bitrate_bps));
    }
}


void NimRTCEngine::on_tmmbr_received(std::uint32_t sender_ssrc,
                                     std::uint32_t media_ssrc,
                                     std::uint32_t bitrate_bps) noexcept {
    // The peer's request applies to *our* outbound streams.  The
    // tightest limit wins: two receivers, one sender, and the sender
    // has to satisfy both (RFC 5104 §3.5.4.2 — the bounding set is the
    // tightest constraint).
    //
    // A tuple that names a media source we do not send is not ours to
    // honour: the peer is asking some *other* sender to throttle.  The
    // engine only sends audio (and video), so a tuple about an unknown
    // SSRC is recorded but not applied.
    if (media_ssrc != 0 && media_ssrc != impl_->audio_ssrc) {
        core::log::Logger::instance().debug(
            "engine[tmmbr]: inbound TMMBR names a foreign media source; ignored");
        return;
    }

    if (bitrate_bps == 0) {
        // "No limit" — nothing to apply.
        return;
    }

    if (impl_->peer_tmmbr_limit_bps == 0 ||
        bitrate_bps < impl_->peer_tmmbr_limit_bps) {
        impl_->peer_tmmbr_limit_bps = bitrate_bps;
    }

    impl_->tmmbr_peer_ssrc = sender_ssrc;
    ++impl_->tmmbr_received;

    // A TMMBN is owed for every TMMBR we receive (RFC 5104 §4.2.2.2).
    impl_->pending_tmmbn = true;

    core::log::Logger::instance().info(
        "engine[tmmbr]: inbound TMMBR, limit now " +
        std::to_string(impl_->peer_tmmbr_limit_bps) + " bps");
}


// ---------------------------------------------------------------------------
// The peer's Receiver Reference Time report (RFC 3611 §4.4)
// ---------------------------------------------------------------------------

void NimRTCEngine::note_peer_reference_time(core::ByteSpan packet) noexcept {
    // A malformed XR packet is ignored: the XR consumer has already
    // counted it, and the DLRR we would build from it would be
    // meaningless.
    auto parsed = rtp::parse_xr_packet(packet);
    if (!parsed) return;

    const rtp::XrPacket& xr = parsed.value();

    // Find the Receiver Reference Time block (BT = 4).  Its NTP
    // timestamp is what the peer will match against the DLRR we send
    // back (RFC 3611 §4.5).
    for (const rtp::XrBlock& block : xr.blocks) {
        if (block.type != rtp::XrBlockType::kReceiverReferenceTime) continue;

        impl_->peer_rr_ssrc = xr.ssrc;
        impl_->peer_rr_mid_32 =
            rtp::Ntp64{block.receiver_reference_time.ntp_timestamp}.mid_32();
        impl_->peer_rr_recv_us = detail::steady_now_us();
        return;
    }
}


// ---------------------------------------------------------------------------
// The observability accessors
// ---------------------------------------------------------------------------
std::uint32_t NimRTCEngine::peer_tmmbr_limit_bps() const noexcept {
    return impl_->peer_tmmbr_limit_bps;
}

std::uint32_t NimRTCEngine::last_tmmbr_bps() const noexcept {
    return impl_->last_tmmbr_bps;
}

std::uint64_t NimRTCEngine::tmmbr_sent_count() const noexcept {
    return impl_->tmmbr_sent + impl_->tmmbn_sent;
}

std::uint64_t NimRTCEngine::tmmbr_received_count() const noexcept {
    return impl_->tmmbr_received;
}

std::uint64_t NimRTCEngine::xr_reports_sent_count() const noexcept {
    return impl_->xr_reports_sent;
}

}  // namespace nimrtc::engine
