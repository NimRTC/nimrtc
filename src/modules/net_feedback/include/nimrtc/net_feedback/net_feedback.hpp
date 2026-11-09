/**
 * @file nimrtc/net_feedback/net_feedback.hpp
 * @brief The receiver-side network-feedback collector (v1.2.0 M1, F2).
 *
 * The module implements `plugins::INetFeedback`: it consumes the
 * receiver-side network signals of an inbound RTCP compound datagram —
 * the Receiver Report (loss, RTT, jitter), REMB (the receiver's own
 * bandwidth opinion) and transport-cc (the per-packet arrival series) —
 * and aggregates them into one `plugins::NetFeedbackSnapshot` the engine
 * can hand to the bandwidth estimator.
 *
 * It owns no transport, opens no socket and sends nothing.  The wire
 * formats belong to the `rtp` module; this module only aggregates.
 *
 * The engine reaches it through the registry's net-feedback slot (the id
 * is "nimrtc_net_feedback"); it never includes this header, because the
 * engine may not depend on a concrete module (constraint L4).
 *
 * @see docs/adr/ADR-018-bandwidth-signal-policy.md
 * @see docs/plan/video-pipeline-v2-plan.md (Track F)
 */
#ifndef NIMRTC_NET_FEEDBACK_NET_FEEDBACK_HPP
#define NIMRTC_NET_FEEDBACK_NET_FEEDBACK_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).

#include <nimrtc/core/api_version.hpp>

#include <cstdint>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/time.hpp>
#include <nimrtc/plugins/net_feedback.hpp>

namespace nimrtc::net_feedback {

/** The built-in collector.
 *
 *  The collector is a pure aggregator: it holds the last snapshot it
 *  produced, the description of our own last Sender Report (needed to
 *  turn an LSR / DLSR pair into an RTT) and the RTP clock rate of the
 *  stream the feedback describes (needed to turn the report's jitter
 *  into milliseconds). */
class NIMRTC_API_STABLE Collector : public plugins::INetFeedback {
public:
    Collector();
    ~Collector() override;

    Collector(const Collector&) = delete;
    Collector& operator=(const Collector&) = delete;

    // IPlugin -----------------------------------------------------------
    const char* name() const noexcept override;
    plugins::Status open() noexcept override;
    void close() noexcept override;

    // Collector contract -------------------------------------------------

    void on_rtcp_block(std::uint8_t payload_type, std::uint8_t fmt,
                       core::ByteSpan block, core::TimePoint now) noexcept override;

    void on_local_sender_report(std::uint32_t lsr_mid32,
                                core::TimePoint sent_at) noexcept override;

    void set_stream_clock_rate(std::uint32_t hz) noexcept override;

    plugins::NetFeedbackSnapshot snapshot() const noexcept override;

    void reset() noexcept override;

private:
    void handle_receiver_report(core::ByteSpan block, core::TimePoint now) noexcept;
    void handle_remb(core::ByteSpan block, core::TimePoint now) noexcept;
    void handle_transport_cc(core::ByteSpan block, core::TimePoint now) noexcept;

    /** Recompute the snapshot's `source` from the signals seen so far. */
    void refresh_source() noexcept;

    plugins::NetFeedbackSnapshot snap_{};

    /** The RTP clock rate of the stream the feedback describes (0 = unknown). */
    std::uint32_t clock_rate_hz_ = 0;

    /** The middle 32 bits of the NTP timestamp of our last Sender Report. */
    std::uint32_t local_lsr_mid32_ = 0;

    /** When our last Sender Report went out. */
    core::TimePoint local_sr_sent_at_{};

    /** Whether a Sender Report has been announced at all. */
    bool has_local_sr_ = false;

    /** The cumulative loss of the previous Receiver Report, so the
     *  per-window loss count can be derived. */
    std::int32_t prev_cumulative_lost_ = 0;

    /** Whether `prev_cumulative_lost_` holds a real value. */
    bool has_prev_cumulative_lost_ = false;
};

/**
 * @brief Register the module's default plugins.
 *
 * Called by `core::register_all_default_plugins()` (through the registrar
 * table in `src/core/src/pal_default_registrars.cpp`).  It registers the
 * collector under the id "nimrtc_net_feedback" in the registry's
 * net-feedback category.
 *
 * Idempotent: the registration happens once, on the first call.
 */
void NIMRTC_API_STABLE register_default_plugins() noexcept;

} // namespace nimrtc::net_feedback
#endif // NIMRTC_NET_FEEDBACK_NET_FEEDBACK_HPP
