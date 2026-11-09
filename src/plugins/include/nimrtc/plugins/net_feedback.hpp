/**
 * @file nimrtc/plugins/net_feedback.hpp
 * @brief INetFeedback — the pluggable *receiver-side network feedback* seam.
 *
 * v1.2.0 M1 (F2 / F4 / F7).
 *
 * ## What this seam is for
 *
 * Until v1.2.0 the engine read `IBwe::estimate()` in its tick but nothing
 * ever fed the estimator: `IBwe::on_feedback()` was called from unit tests
 * only.  The bandwidth target was therefore pinned at the initial value
 * (1 Mbps) forever, and every rate-control feature built on top of it was
 * decorative.
 *
 * This seam is the missing producer.  It sits *between* the wire and the
 * estimator:
 *
 * ```
 *   peer RTCP ──▶ engine demux (F1) ──▶ INetFeedback ──▶ snapshot
 *                                                          │
 *                                                          ▼
 *                                                  IBwe::on_feedback()
 * ```
 *
 * The engine's job (F1) is to *route* an inbound RTCP compound datagram:
 * the Sender Report goes to the cross-end timeline, the NACK / PLI / FIR
 * go to the video sender, and the receiver-side *network* feedback (RR,
 * REMB, transport-cc) goes here.  The collector's job is to aggregate
 * those signals into one snapshot the estimator can consume.
 *
 * ## Why the seam takes (payload type, FMT, bytes) and not parsed structs
 *
 * The plugin layer may not depend on a concrete module (`src/plugins/` is
 * an interface-only library), so it cannot name `rtp::ReceiverReport` or
 * `rtp::TransportCcFeedback`.  Passing the payload type, the FMT and the
 * raw sub-packet keeps the seam free of wire-format types while still
 * telling the collector *what* it is looking at: the collector maps
 * (PT 201) → Receiver Report, (PT 206 / FMT 15) → REMB, (PT 205 / FMT 15)
 * → transport-cc, and ignores everything else.
 *
 * ## Not thread-safe
 *
 * The engine drives the collector from the engine thread, the same thread
 * that calls `snapshot()`.  Implementations do not need to be reentrant.
 *
 * @see docs/adr/ADR-018-bandwidth-signal-policy.md
 * @see docs/plan/video-pipeline-v2-plan.md (F2, F4, F7)
 */

// base.hpp must be before include guard — see transport.hpp for rationale.
#include <nimrtc/core/api_version.hpp>
#include "nimrtc/plugins/base.hpp"

#ifndef NIMRTC_PLUGINS_NET_FEEDBACK_HPP
#define NIMRTC_PLUGINS_NET_FEEDBACK_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <nimrtc/core/time.hpp>   // core::TimePoint / core::Milliseconds

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// Signal selection
// ---------------------------------------------------------------------------

/** Which receiver-side signal is driving the bandwidth estimate.
 *
 *  ADR-018 §"Signal selection" makes the choice explicit: transport-cc
 *  when the peer offers it, loss/RTT otherwise, REMB as an additional
 *  opinion rather than the only one.  The discriminator travels with the
 *  feedback so an estimator can tell *why* it is being told what it is
 *  told — the previous behaviour (REMB-only) made that impossible. */
enum class NIMRTC_API_STABLE NetFeedbackSource : std::uint8_t {
    kLossRtt    = 0,  ///< Receiver Report: fraction lost + RTT + jitter.
    kTransportCc = 1,  ///< Transport-wide congestion control (RFC draft).
    kRemb        = 2,  ///< Receiver Estimated Maximum Bitrate.
};

// ---------------------------------------------------------------------------
// The aggregated snapshot
// ---------------------------------------------------------------------------

/** The receiver-side network signals, aggregated into one value.
 *
 *  Every field is optional in the sense that a collector that has not yet
 *  seen the corresponding feedback leaves it at its default; the `has_*`
 *  flags say which signals actually contributed. */
struct NIMRTC_API_STABLE NetFeedbackSnapshot {
    /** Which signal this snapshot is dominated by. */
    NetFeedbackSource source = NetFeedbackSource::kLossRtt;

    /** When the snapshot was produced (monotonic). */
    core::TimePoint timestamp{};

    // ---- Receiver Report (loss / RTT / jitter) --------------------------

    /** True once a Receiver Report has been seen. */
    bool has_loss_rtt = false;

    /** Fraction of packets lost in the last reporting window, [0.0, 1.0]. */
    double loss_rate = 0.0;

    /** Packets received in the last reporting window. */
    std::uint32_t packets_received = 0;

    /** Packets lost in the last reporting window. */
    std::uint32_t packets_lost = 0;

    /** Cumulative number of packets lost since the stream started. */
    std::uint32_t cumulative_packets_lost = 0;

    /** Round-trip time, measured from the LSR / DLSR pair of a Receiver
     *  Report against our own last Sender Report. */
    core::Milliseconds rtt{0};

    /** Inter-arrival jitter in milliseconds (0 when the RTP clock rate of
     *  the described stream is unknown). */
    double jitter_ms = 0.0;

    // ---- REMB ------------------------------------------------------------

    /** The receiver's own bandwidth estimate, when it sent one. */
    std::optional<std::uint32_t> remb_bps;

    // ---- transport-cc ----------------------------------------------------

    /** True once a transport-cc feedback has been seen. */
    bool has_transport_cc = false;

    /** Packets the peer reported as received. */
    std::uint32_t tc_packets_received = 0;

    /** Packets the peer reported as lost. */
    std::uint32_t tc_packets_lost = 0;

    /** The receive rate implied by the transport-cc arrival series, in
     *  packets per second. */
    double tc_receive_rate_pps = 0.0;

    /** The first sequence number the transport-cc feedback covers. */
    std::uint16_t tc_base_seq = 0;

    /** How many sequence numbers the transport-cc feedback covers. */
    std::uint16_t tc_packet_count = 0;

    /** The per-packet arrival deltas of the last transport-cc feedback,
     *  in milliseconds (the arrival-time series F4 asks for). */
    std::vector<double> tc_arrival_deltas_ms;
};

// ---------------------------------------------------------------------------
// INetFeedback
// ---------------------------------------------------------------------------

/** Pluggable collector of receiver-side network feedback.
 *
 *  Implementations consume *demultiplexed* RTCP sub-packets: the engine
 *  splits the compound datagram (RFC 3550 §6.1) and hands each sub-packet
 *  to the collector that wants it.  A collector that receives a sub-packet
 *  it does not understand must ignore it. */
class NIMRTC_API_STABLE INetFeedback : public IPlugin {
public:
    ~INetFeedback() override = default;

    // IPlugin -----------------------------------------------------------
    const char* name() const noexcept override = 0;
    Status      open() noexcept override = 0;
    void        close() noexcept override = 0;

    // Collector contract -------------------------------------------------

    /** Feed one RTCP sub-packet.
     *
     *  @param payload_type  The RTCP payload type (201 = RR, 205 = RTPFB,
     *                       206 = PSFB).
     *  @param fmt           The RC / FMT field of the sub-packet.
     *  @param block         The whole sub-packet, header included.
     *  @param now           The monotonic instant the packet arrived.
     */
    virtual void on_rtcp_block(std::uint8_t payload_type, std::uint8_t fmt,
                               core::ByteSpan block, core::TimePoint now) noexcept = 0;

    /** Tell the collector about *our* last Sender Report.
     *
     *  The Receiver Report a peer sends describes *our* stream, and its
     *  LSR / DLSR pair only becomes an RTT when the collector knows when
     *  that Sender Report left.  The engine therefore hands the collector
     *  the middle 32 bits of the NTP timestamp it put on the wire and the
     *  instant it sent it.
     *
     *  @param lsr_mid32  The middle 32 bits of the SR's NTP timestamp.
     *  @param sent_at    The monotonic instant the SR was sent.
     */
    virtual void on_local_sender_report(std::uint32_t lsr_mid32,
                                        core::TimePoint sent_at) noexcept = 0;

    /** The RTP clock rate of the stream the feedback describes, in Hz.
     *
     *  The jitter in a Receiver Report is expressed in RTP timestamp units,
     *  so it can only be converted to milliseconds once the clock rate is
     *  known.  0 means "unknown" and the jitter stays 0. */
    virtual void set_stream_clock_rate(std::uint32_t hz) noexcept = 0;

    /** The aggregated snapshot. */
    virtual NetFeedbackSnapshot snapshot() const noexcept = 0;

    /** Reset the collector (e.g. on stream restart). */
    virtual void reset() noexcept = 0;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

/** Factory for network-feedback collectors. */
class NIMRTC_API_STABLE INetFeedbackFactory {
public:
    virtual ~INetFeedbackFactory() = default;
    virtual std::string_view id()           const noexcept = 0;
    virtual std::string_view display_name() const noexcept = 0;

    /** Create a new collector instance. */
    virtual INetFeedback* create() const = 0;
};

} // namespace plugins
#endif // NIMRTC_PLUGINS_NET_FEEDBACK_HPP
