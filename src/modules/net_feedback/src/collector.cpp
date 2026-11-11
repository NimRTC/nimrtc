/**
 * @file src/modules/net_feedback/src/collector.cpp
 * @brief The receiver-side network-feedback collector (v1.2.0 M1, F2 + M2, F5).
 *
 * The collector turns three kinds of inbound RTCP into one snapshot:
 *
 *   - **Receiver Report** (PT 201): the fraction of packets the peer
 *     lost, the jitter, and — through the LSR / DLSR pair against our own
 *     last Sender Report — the round-trip time.
 *   - **REMB** (PSFB, PT 206 / FMT 15): the peer's own opinion about the
 *     available bandwidth.
 *   - **transport-cc** (RTPFB, PT 205 / FMT 15): the per-packet arrival
 *     series, from which the receive rate is derived.
 *
 * The wire formats belong to the `rtp` module (`rtp::Parser`,
 * `rtp::parse_remb`); this file only aggregates.  That split is what
 * keeps the collector small: a new feedback format is a change in the
 * `rtp` module, not here.
 *
 * The collector also owns the **signal-selection policy** (ADR-018
 * Decision 5).  The engine hands it two facts it cannot know itself —
 * what the peer advertised (`a=rtcp-fb`) and what the host wants
 * (`NetFeedbackConfig::policy`) — and the collector resolves which
 * signal drives:
 *
 * ```
 *   peer advertised transport-cc?  no  ─────────────▶ loss/RTT
 *                                 yes │ feedback fresh? no ─▶ loss/RTT
 *                                     │                 yes ─▶ transport-cc
 * ```
 *
 * The engine reads the answer out of the snapshot; it holds no policy
 * branch of its own.
 */

#include <nimrtc/net_feedback/net_feedback.hpp>

#include <algorithm>
#include <chrono>

#include <nimrtc/rtp/packet.hpp>
#include <nimrtc/rtp/rtcp.hpp>

namespace nimrtc::net_feedback {

namespace {

// RTCP payload types (RFC 3550 §6.1 / RFC 4585 / RFC 5880).
constexpr std::uint8_t kPtReceiverReport = 201;
constexpr std::uint8_t kPtRtpFeedback    = 205;
constexpr std::uint8_t kPtPayloadSpecific = 206;

// The FMT that selects transport-cc inside an RTPFB message.
constexpr std::uint8_t kFmtTransportCc = 15;
// The FMT that selects REMB inside a PSFB message.
constexpr std::uint8_t kFmtRemb = 15;

// The transport-cc receive delta unit: 250 microseconds.
constexpr double kDeltaUnitMs = 0.25;

// An upper bound on the arrival series we keep, so a long-lived
// connection cannot grow the snapshot without limit.
constexpr std::size_t kMaxArrivalSamples = 1024;

} // namespace

// ---------------------------------------------------------------------------
// Construction / lifecycle
// ---------------------------------------------------------------------------

Collector::Collector() = default;
Collector::~Collector() = default;

const char* Collector::name() const noexcept {
    return "nimrtc_net_feedback";
}

plugins::Status Collector::open() noexcept {
    reset();
    return plugins::kOk;
}

void Collector::close() noexcept {
    reset();
}

void Collector::reset() noexcept {
    snap_ = plugins::NetFeedbackSnapshot{};
    has_local_sr_ = false;
    local_lsr_mid32_ = 0;
    local_sr_sent_at_ = core::TimePoint{};
    prev_cumulative_lost_ = 0;
    has_prev_cumulative_lost_ = false;
    transport_cc_unavailable_ = false;
    // The clock rate is a property of the negotiated stream, not of the
    // collector's state, so it survives a reset.  So do the policy and
    // the peer's capabilities: both describe the *session*, not the
    // feedback stream, and a stream restart does not renegotiate them.
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void Collector::set_stream_clock_rate(std::uint32_t hz) noexcept {
    clock_rate_hz_ = hz;
}

void Collector::set_config(const plugins::NetFeedbackConfig& config) noexcept {
    config_ = config;
    // Re-resolve against the most recent instant the collector knows,
    // so a live policy change takes effect at once.  The engine calls
    // `evaluate()` from its tick with the real clock, which keeps the
    // freshness axis moving between ticks.
    refresh_source(snap_.timestamp);
}

plugins::NetFeedbackConfig Collector::config() const noexcept {
    return config_;
}

void Collector::set_peer_capabilities(const plugins::NetFeedbackCapabilities& caps) noexcept {
    caps_ = caps;
    refresh_source(snap_.timestamp);
}

plugins::NetFeedbackCapabilities Collector::peer_capabilities() const noexcept {
    return caps_;
}

bool Collector::transport_cc_unavailable() const noexcept {
    return transport_cc_unavailable_;
}

void Collector::evaluate(core::TimePoint now) noexcept {
    // Only the *resolution* changes here: `snap_.timestamp` marks new
    // data and must not move, or the engine would feed the estimator a
    // duplicate sample on every tick.
    refresh_source(now);
}

// ---------------------------------------------------------------------------
// Local sender report
// ---------------------------------------------------------------------------

void Collector::on_local_sender_report(std::uint32_t lsr_mid32,
                                       core::TimePoint sent_at) noexcept {
    local_lsr_mid32_ = lsr_mid32;
    local_sr_sent_at_ = sent_at;
    has_local_sr_ = true;
}

// ---------------------------------------------------------------------------
// Inbound RTCP
// ---------------------------------------------------------------------------

void Collector::on_rtcp_block(std::uint8_t payload_type, std::uint8_t fmt,
                              core::ByteSpan block, core::TimePoint now) noexcept {
    switch (payload_type) {
        case kPtReceiverReport:
            handle_receiver_report(block, now);
            break;
        case kPtPayloadSpecific:
            if (fmt == kFmtRemb) {
                handle_remb(block, now);
            }
            break;
        case kPtRtpFeedback:
            if (fmt == kFmtTransportCc) {
                handle_transport_cc(block, now);
            }
            break;
        default:
            // Not ours (a Sender Report, a NACK, a PLI, ...).  The
            // collector ignores what it does not understand.
            break;
    }
}

// ---------------------------------------------------------------------------
// Receiver Report — loss, jitter, RTT
// ---------------------------------------------------------------------------

void Collector::handle_receiver_report(core::ByteSpan block,
                                       core::TimePoint now) noexcept {
    rtp::Parser parser;
    auto rr = parser.parse_rr(block);
    if (!rr) {
        return;   // Malformed report: keep the previous snapshot.
    }

    for (const rtp::ReportBlock& rb : rr.value().report_blocks) {
        snap_.has_loss_rtt = true;
        snap_.timestamp = now;
        snap_.loss_rtt_timestamp = now;

        // The fraction lost is an 8-bit value scaled by 256 (RFC 3550
        // §6.4.1): 0 means no loss, 255 means "all or more than all".
        snap_.loss_rate = static_cast<double>(rb.fraction_lost) / 256.0;

        const std::int32_t cumulative = rb.cumulative_packets_lost;
        snap_.cumulative_packets_lost =
            cumulative > 0 ? static_cast<std::uint32_t>(cumulative) : 0u;

        // The report block carries the *cumulative* loss, not a per-window
        // count; the window count is the difference between two reports.
        if (has_prev_cumulative_lost_) {
            const std::int32_t delta = cumulative - prev_cumulative_lost_;
            snap_.packets_lost = delta > 0 ? static_cast<std::uint32_t>(delta) : 0u;
        } else {
            snap_.packets_lost = 0u;
        }
        prev_cumulative_lost_ = cumulative;
        has_prev_cumulative_lost_ = true;

        // The jitter is expressed in RTP timestamp units of the stream the
        // report describes, so it needs that stream's clock rate.
        if (clock_rate_hz_ > 0) {
            snap_.jitter_ms = (static_cast<double>(rb.interarrival_jitter) * 1000.0) /
                              static_cast<double>(clock_rate_hz_);
        }

        // ---- RTT from the LSR / DLSR pair --------------------------------
        //
        // The report block refers to *our* Sender Report: `last_sr_ntp` is
        // the middle 32 bits of the NTP timestamp we put on the wire, and
        // `delay_since_last_sr` is how long the peer had that report when
        // it sent this one.  The round trip is therefore
        //
        //     RTT = (now - our SR send time) - DLSR
        //
        // and it is only meaningful when the LSR matches the report we
        // actually sent.
        if (has_local_sr_ && rb.last_sr_ntp == local_lsr_mid32_) {
            const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                        now - local_sr_sent_at_)
                                        .count();
            const std::int64_t dlsr_us =
                (static_cast<std::int64_t>(rb.delay_since_last_sr) * 1000000) / 65536;
            const std::int64_t rtt_us = elapsed_us - dlsr_us;
            if (rtt_us >= 0) {
                snap_.rtt = core::Milliseconds{static_cast<std::int64_t>(rtt_us / 1000)};
            }
        }
    }

    refresh_source(now);
}

// ---------------------------------------------------------------------------
// REMB
// ---------------------------------------------------------------------------

void Collector::handle_remb(core::ByteSpan block, core::TimePoint now) noexcept {
    auto remb = rtp::parse_remb(block);
    if (!remb) {
        return;
    }
    // The REMB bitrate is a 64-bit field on the wire but a session-level
    // estimate in practice; the estimator's field is 32 bits, so clamp
    // rather than wrap.
    const std::uint64_t bps = remb.value().bitrate_bps;
    snap_.remb_bps = bps > 0xFFFFFFFFull
                         ? std::optional<std::uint32_t>{0xFFFFFFFFu}
                         : std::optional<std::uint32_t>{static_cast<std::uint32_t>(bps)};
    snap_.timestamp = now;
    snap_.remb_timestamp = now;
    refresh_source(now);
}

// ---------------------------------------------------------------------------
// transport-cc
// ---------------------------------------------------------------------------

void Collector::handle_transport_cc(core::ByteSpan block, core::TimePoint now) noexcept {
    rtp::Parser parser;
    auto cc = parser.parse_transport_cc(block);
    if (!cc) {
        return;
    }
    const rtp::TransportCcFeedback& f = cc.value();

    std::uint32_t received = 0;
    std::uint32_t lost = 0;
    double total_delta_ms = 0.0;
    std::vector<double> deltas;
    deltas.reserve(f.packets.size());

    for (const rtp::TransportCcPacketStatus& p : f.packets) {
        switch (p.status) {
            case rtp::PacketStatus::NotReceived:
                ++lost;
                break;
            case rtp::PacketStatus::Received:
            case rtp::PacketStatus::ReceivedSmall: {
                ++received;
                const double delta_ms = static_cast<double>(p.delta_250us) * kDeltaUnitMs;
                deltas.push_back(delta_ms);
                total_delta_ms += delta_ms;
                break;
            }
            case rtp::PacketStatus::ReceivedNoDelta:
                // The packet arrived, but the feedback carries no arrival
                // delta for it — it contributes to the received count but
                // not to the arrival series.
                ++received;
                break;
        }
    }

    snap_.has_transport_cc = true;
    snap_.timestamp = now;
    snap_.transport_cc_timestamp = now;
    snap_.tc_packets_received = received;
    snap_.tc_packets_lost = lost;
    snap_.tc_base_seq = f.base_seq;
    snap_.tc_packet_count = f.packet_status_count;

    // The receive rate is the number of arrived packets over the span the
    // arrival series covers.
    if (total_delta_ms > 0.0) {
        snap_.tc_receive_rate_pps =
            static_cast<double>(received) / (total_delta_ms / 1000.0);
    } else {
        snap_.tc_receive_rate_pps = 0.0;
    }

    if (deltas.size() > kMaxArrivalSamples) {
        deltas.erase(deltas.begin(),
                     deltas.begin() +
                         static_cast<std::ptrdiff_t>(deltas.size() - kMaxArrivalSamples));
    }
    snap_.tc_arrival_deltas_ms = std::move(deltas);

    refresh_source(now);
}

// ---------------------------------------------------------------------------
// Source selection — the policy (ADR-018 Decisions 2 and 5)
// ---------------------------------------------------------------------------

bool Collector::transport_cc_stale(core::TimePoint now) const noexcept {
    if (!snap_.has_transport_cc) {
        return true;   // Nothing arrived yet: certainly not fresh.
    }
    if (config_.transport_cc_timeout.count() <= 0) {
        return false;  // A non-positive timeout disables the freshness axis.
    }
    if (now < snap_.transport_cc_timestamp) {
        return false;  // Clock went backwards: do not call it stale.
    }
    return (now - snap_.transport_cc_timestamp) > config_.transport_cc_timeout;
}

void Collector::refresh_source(core::TimePoint now) noexcept {
    const bool have_rr   = snap_.has_loss_rtt;
    const bool have_remb = snap_.remb_bps.has_value();

    // The transport-cc path is usable only when the peer *negotiated* it
    // (a peer that did not advertise it will never send it) and the last
    // feedback is still fresh.
    const bool tc_fresh   = snap_.has_transport_cc && !transport_cc_stale(now);
    const bool tc_usable = caps_.transport_cc && tc_fresh;

    // The loss/RTT path, and REMB as its last resort.
    const plugins::NetFeedbackSource fallback =
        have_rr ? plugins::NetFeedbackSource::kLossRtt
                : (have_remb ? plugins::NetFeedbackSource::kRemb
                             : plugins::NetFeedbackSource::kLossRtt);

    switch (config_.policy) {
        case plugins::NetFeedbackPolicy::kRequireTransportCc:
            // "Fail loudly": the path is reported as unavailable instead
            // of being papered over by the fallback.  The fallback still
            // drives the estimate — starving the estimator would be a
            // worse failure than a degraded one — but the host can see
            // what happened through `transport_cc_unavailable()`.
            if (tc_usable) {
                snap_.source = plugins::NetFeedbackSource::kTransportCc;
                transport_cc_unavailable_ = false;
            } else {
                snap_.source = fallback;
                transport_cc_unavailable_ = true;
            }
            break;

        case plugins::NetFeedbackPolicy::kLossRtt:
            // The override: the loss/RTT path always drives.  REMB stays
            // an upper-bound hint (ADR-018 Decision 3), never the driver.
            snap_.source = fallback;
            transport_cc_unavailable_ = false;
            break;

        case plugins::NetFeedbackPolicy::kAuto:
        default:
            // The automatic default (ADR-018 Decision 2's table).
            snap_.source = tc_usable ? plugins::NetFeedbackSource::kTransportCc
                                      : fallback;
            transport_cc_unavailable_ = false;
            break;
    }
}

// ---------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------

plugins::NetFeedbackSnapshot Collector::snapshot() const noexcept {
    return snap_;
}

} // namespace nimrtc::net_feedback
