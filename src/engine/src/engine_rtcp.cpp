/**
 * @file src/engine/src/engine_rtcp.cpp
 * @brief The RTCP receiver report for the inbound audio stream.
 *
 * v1.2.0 (audio pipeline, WP-8; spec B-12 / B-13).
 *
 * The browser's `inbound-rtp` statistics — the loss and the jitter a
 * host sees in `chrome://webrtc-internals` — are derived from the
 * RTCP Receiver Reports the *receiver* sends.  Without them the peer
 * sees a stream that never reports anything: no loss figure, no jitter
 * figure, no round-trip measurement.  This file is the engine side of
 * that loop.
 *
 * Two things live here:
 *
 *   1. `note_inbound_audio_packet()` — the *sequence* bookkeeping of
 *      the inbound audio stream (RFC 3550 Appendix A.1): the extended
 *      highest sequence number, the cumulative loss and the
 *      interarrival jitter (RFC 3550 §6.4.1).
 *
 *   2. `maybe_send_receiver_report()` — the report itself, sent
 *      through the same SRTCP path as every other RTCP packet the
 *      engine emits.
 *
 * The engine is the right place for both: the receive pipeline counts
 * packets and plays out frames, but a *report* is a statement about
 * the transport, and the transport is the engine's business.
 */
#include "engine_impl.hpp"

namespace nimrtc::engine {

namespace {

/** The receiver-report cadence.
 *
 *  RFC 3550 §6.4.2 recommends one report every 5–10 s, but that
 *  recommendation predates the browsers' statistics: Chrome derives
 *  its `inbound-rtp` loss and jitter figures from the reports it
 *  receives, so a report only every 10 s would make the numbers in
 *  `webrtc-internals` lag by ten seconds.  One second is what the
 *  WebRTC stack sends. */
constexpr std::int64_t kReceiverReportPeriodMs = 1000;

/** RFC 3550 A.1 `MAX_DROPOUT` — a jump larger than this is treated as
 *  a stream restart rather than as a reordering. */
constexpr std::int32_t kMaxDropout = 3000;

/** RFC 3550 A.1 `MAX_MISORDER` — a jump smaller than this is treated
 *  as a reordering or a duplicate. */
constexpr std::int32_t kMaxMisorder = 100;

}  // namespace


// ---------------------------------------------------------------------------
// The sequence bookkeeping (RFC 3550 Appendix A.1)
// ---------------------------------------------------------------------------

void NimRTCEngine::note_inbound_audio_packet(std::uint32_t ssrc,
                                              std::uint16_t seq,
                                              std::uint32_t rtp_timestamp,
                                              std::int64_t arrival_us) noexcept {
    if (ssrc == 0) return;

    detail::AudioState::AudioRxState& rx = impl_->audio_state.audio_rx;

    // A different source is a different sequence space: everything we
    // counted so far belongs to the old stream (RFC 3550 A.1 — the
    // counters are per source).
    const bool new_source = (rx.ssrc != ssrc);

    // `udelta` is the signed distance from the highest sequence number
    // we have seen, computed on the 16-bit sequence space (RFC 3550
    // A.1).  A negative value means the packet is older than the
    // highest one — a reordering, not a new packet.
    const std::int32_t udelta =
        static_cast<std::int32_t>(seq) -
        static_cast<std::int32_t>(static_cast<std::uint16_t>(rx.max_seq));

    // A very large jump means the sender restarted its sequence space.
    // RFC 3550 A.1 treats that as a new `base_seq`; we go one step
    // further and restart the counters with it, because the old
    // counters describe a sequence space that no longer exists (keeping
    // them would report a cumulative loss of −(the old packet count)).
    const bool restarted = !new_source &&
                           udelta > kMaxMisorder &&
                           udelta >= kMaxDropout;

    if (new_source || restarted) {
        rx.ssrc            = ssrc;
        rx.base_seq        = seq;
        rx.max_seq         = seq;
        rx.received        = 1;
        rx.expected_prior  = 1;
        rx.received_prior  = 1;
        rx.jitter          = 0;
        rx.have_transit    = false;
        rx.last_sr_lsr     = 0;
        rx.last_sr_recv_us = 0;
    } else {
        if (udelta < kMaxDropout) {
            // In order (or a small reorder).  A wrap of the 16-bit
            // sequence space bumps the cycle counter, which is what
            // makes the extended highest sequence number monotonic.
            if (static_cast<std::int32_t>(seq) <
                static_cast<std::int32_t>(static_cast<std::uint16_t>(rx.max_seq))) {
                rx.max_seq += 65536u;
            }
            rx.max_seq = (rx.max_seq & 0xFFFF0000u) | seq;
        } else {
            // A duplicate or a modest reordering: it does not move the
            // highest sequence number, but it *is* a received packet.
        }
        ++rx.received;
    }

    // ---- Interarrival jitter (RFC 3550 §6.4.1) ------------------------
    //
    // The jitter is the EWMA of the absolute differences between the
    // *transit* times — the arrival time minus the RTP timestamp, both
    // expressed in the media clock's units.  The clock rate is the
    // negotiated one (48 kHz for Opus); the arrival instant is the
    // engine's monotonic clock, converted into the same units.
    const std::uint32_t clock_rate =
        config_.audio_codec.clock_rate != 0
            ? static_cast<std::uint32_t>(config_.audio_codec.clock_rate)
            : 48000u;

    const std::int64_t arrival_ticks =
        (arrival_us * static_cast<std::int64_t>(clock_rate)) / 1000000;

    // The transit time is modular over 2^32 (RFC 3550 A.1).
    const std::int32_t transit = static_cast<std::int32_t>(
        static_cast<std::uint32_t>(arrival_ticks -
                                    static_cast<std::int64_t>(rtp_timestamp)));

    if (rx.have_transit) {
        // The difference is taken modulo 2^32 as well: the transit time
        // wraps when the RTP timestamp does, and a wrap must not look
        // like a multi-second arrival gap.
        const std::int32_t d = static_cast<std::int32_t>(
            static_cast<std::uint32_t>(transit) -
            static_cast<std::uint32_t>(rx.last_transit));
        const std::int64_t magnitude =
            d < 0 ? -static_cast<std::int64_t>(d) : static_cast<std::int64_t>(d);
        // J += (|D| - J) / 16 — the smoothing factor RFC 3550 fixes.
        rx.jitter += static_cast<std::uint32_t>(
            (magnitude - static_cast<std::int64_t>(rx.jitter)) / 16);
    }
    rx.last_transit = transit;
    rx.have_transit  = true;
}


// ---------------------------------------------------------------------------
// The receiver report (RFC 3550 §6.4.2)
// ---------------------------------------------------------------------------

void NimRTCEngine::maybe_send_receiver_report() noexcept {
    if (!ice_t_ || !is_ice_connected()) return;

    detail::AudioState::AudioRxState& rx = impl_->audio_state.audio_rx;
    // No inbound audio yet: there is nothing to report about.
    if (rx.ssrc == 0) return;

    const auto now = core::SteadyClock::now();
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - impl_->audio_state.rr_last_sent)
            .count();
    if (elapsed_ms < kReceiverReportPeriodMs) return;
    impl_->audio_state.rr_last_sent = now;

    // ---- The report block (RFC 3550 §6.4.1) ---------------------------
    //
    //   expected = max_seq − base_seq + 1   (the extended space)
    //   lost     = expected − received       (signed: a duplicate or
    //                                         a reordered packet makes
    //                                         it negative)
    const std::uint64_t expected =
        static_cast<std::uint64_t>(rx.max_seq - rx.base_seq) + 1u;
    const std::int64_t lost =
        static_cast<std::int64_t>(expected) - static_cast<std::int64_t>(rx.received);

    // The fraction lost is a *rate over the interval* since the last
    // report (RFC 3550 A.3): the loss of the interval divided by the
    // expected number of packets in it.
    std::uint8_t fraction_lost = 0;
    if (expected > rx.expected_prior) {
        const std::uint64_t expected_interval = expected - rx.expected_prior;
        const std::int64_t received_interval =
            static_cast<std::int64_t>(rx.received) -
            static_cast<std::int64_t>(rx.received_prior);
        const std::int64_t lost_interval =
            static_cast<std::int64_t>(expected_interval) - received_interval;
        // A negative interval loss means the interval gained packets
        // (duplicates / reordering); it is reported as 0 % rather than
        // as a negative percentage.
        const std::int64_t clamped =
            lost_interval > 0 ? lost_interval : 0;
        fraction_lost = static_cast<std::uint8_t>(
            (clamped << 8) / static_cast<std::int64_t>(expected_interval));
    }
    rx.expected_prior = expected;
    rx.received_prior = rx.received;

    rtp::ReportBlock rb{};
    rb.ssrc = rx.ssrc;
    // The cumulative loss is a 24-bit signed field; the value is
    // sign-extended into the 32-bit carrier (RFC 3550 §6.4.1).
    rb.cumulative_packets_lost = static_cast<std::int32_t>(lost);
    rb.fraction_lost          = fraction_lost;
    rb.extended_highest_seq   = rx.max_seq;
    rb.interarrival_jitter    = rx.jitter;
    rb.last_sr_ntp            = rx.last_sr_lsr;

    // The delay since the last Sender Report, in 1/65536 s units
    // (RFC 3550 §6.4.1).  Zero when no SR has been received — the
    // LSR field then says "no report" as well.
    if (rx.last_sr_recv_us > 0) {
        const std::int64_t now_us = detail::steady_now_us();
        const std::int64_t delta_us = now_us - rx.last_sr_recv_us;
        if (delta_us > 0) {
            rb.delay_since_last_sr = static_cast<std::uint32_t>(
                (delta_us * 65536) / 1000000);
        }
    }

    // ---- Build and send ------------------------------------------------
    //
    // Our own RTCP SSRC identifies us as the reporter (RFC 3550 §6.1);
    // it is distinct from the media SSRCs, which identify the streams.
    if (impl_->audio_state.rtcp_sender_ssrc_ == 0) {
        std::random_device rd;
        std::mt19937_64 mt(rd());
        impl_->audio_state.rtcp_sender_ssrc_ = static_cast<std::uint32_t>(mt() & 0xFFFFFFFFu);
        if (impl_->audio_state.rtcp_sender_ssrc_ == 0) impl_->audio_state.rtcp_sender_ssrc_ = 1;
    }

    rtp::RrBuilder rr;
    rr.set_ssrc(impl_->audio_state.rtcp_sender_ssrc_);
    rr.add_report_block(rb);
    const core::ByteBuffer rr_bytes = rr.build();
    if (rr_bytes.empty()) return;

    // RTCP leaves the engine as SRTCP (RFC 3711 §3.4) once SRTP is
    // keyed — a *different* crypto context from SRTP, so protect_rtp
    // would produce a packet the peer's unprotect_rtcp rejects.
    core::ByteSpan out(rr_bytes.data(), rr_bytes.size());
    std::vector<std::uint8_t> enc;
    if (impl_->srtp_installed && impl_->srtp) {
        auto* sess = impl_->srtp->get_session(impl_->audio_state.rtcp_sender_ssrc_,
                                              /*outgoing=*/true);
        if (!sess) return;   // SRTP installed but no keying yet — skip.
        auto r = sess->protect_rtcp(out);
        if (!r) {
            core::log::Logger::instance().warn(
                "engine[rtcp]: protect_rtcp failed; receiver report dropped");
            return;
        }
        enc.assign(r.value().data(), r.value().data() + r.value().size());
        out = core::ByteSpan(enc.data(), enc.size());
    }

    plugins::BufferView bv{out.data(), out.size()};
    if (ice_t_->send(bv, {}) != plugins::kOk) {
        core::log::Logger::instance().warn(
            "engine[rtcp]: receiver report send failed");
    }
}

}  // namespace nimrtc::engine
