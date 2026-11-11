/**
 * @file nimrtc/plugins/net_feedback.hpp
 * @brief INetFeedback — the pluggable *receiver-side network feedback* seam.
 *
 * v1.2.0 M1 (F2 / F4 / F7) + M2 (F5 — the signal-selection policy).
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
 * ## The policy lives here, not in the engine (ADR-018 Decision 5)
 *
 * Which signal drives the estimate depends on two facts the *negotiation*
 * knows and the engine does not: whether the peer advertised
 * `transport-cc`, and whether that feedback is actually arriving.  The
 * engine therefore hands the collector two things — the peer's
 * capabilities (`set_peer_capabilities()`) and the host's policy
 * (`set_config()`) — and the collector resolves the driving signal.
 * The engine reads the answer out of the snapshot; it contains no policy
 * branch of its own.
 *
 * ## Not thread-safe
 *
 * The engine drives the collector from the engine thread, the same thread
 * that calls `snapshot()`.  Implementations do not need to be reentrant.
 *
 * @see docs/adr/ADR-018-bandwidth-signal-policy.md
 * @see docs/plan/video-pipeline-v2-plan.md (F2, F4, F5, F7)
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
// The signal-selection policy (ADR-018 Decision 2)
// ---------------------------------------------------------------------------

/** Which receiver-side signal drives the bandwidth estimate.
 *
 *  ADR-018 Decision 2 splits the choice in two axes:
 *
 *  * the **negotiation** axis — did the peer advertise `transport-cc`?
 *    Only the SDP offer/answer knows this, so the engine tells the
 *    collector what the peer advertised;
 *  * the **freshness** axis — is transport-cc feedback *actually
 *    arriving*?  A peer that advertised the capability and then never
 *    sends it must not starve the estimator.
 *
 *  The policy below is the *override* on top of the automatic default
 *  those two axes produce.  Note the direction of the override: there is
 *  no "force transport-cc", because forcing a signal the peer never sends
 *  produces a silent stall.  There is "require transport-cc", which
 *  reports the breakage instead of hiding it. */
enum class NIMRTC_API_STABLE NetFeedbackPolicy : std::uint8_t {
    /** The automatic default: transport-cc when the peer negotiated it
     *  and its feedback is fresh, loss/RTT otherwise. */
    kAuto = 0,

    /** Always drive the estimate from the loss/RTT path.  REMB stays an
     *  upper-bound hint (ADR-018 Decision 3), never the driver.  This is
     *  the override a host uses when the peer's transport-cc feedback is
     *  unusable (a buggy peer, a middlebox, a version skew). */
    kLossRtt = 1,

    /** Require transport-cc.  When it is unavailable — the peer did not
     *  negotiate it, or its feedback stopped arriving — the collector
     *  reports the path as unavailable (see
     *  `INetFeedback::transport_cc_unavailable()`) instead of falling
     *  back silently.  A host that wants to *know* its transport-cc
     *  path is broken uses this. */
    kRequireTransportCc = 2,
};

/** What the peer advertised for the negotiated session.
 *
 *  The engine fills this in from the remote description's `a=rtcp-fb`
 *  lines (RFC 4585 §4.1).  It is a *fact about the negotiation*, not a
 *  guess: a peer that did not advertise `transport-cc` will not send
 *  it. */
struct NIMRTC_API_STABLE NetFeedbackCapabilities {
    /** The peer advertised `transport-cc`. */
    bool transport_cc = false;

    /** The peer advertised `goog-remb`. */
    bool remb = false;
};

/** The collector's configuration.
 *
 *  Set by the engine from the host's configuration; a host may also
 *  change it live through `INetFeedback::set_config()`. */
struct NIMRTC_API_STABLE NetFeedbackConfig {
    /** The signal-selection policy. */
    NetFeedbackPolicy policy = NetFeedbackPolicy::kAuto;

    /** How long transport-cc feedback may stay silent before the policy
     *  treats it as stale and falls back to the loss/RTT path.
     *
     *  This is the freshness axis of ADR-018 Decision 2.  It has to be
     *  comfortably larger than the peer's feedback interval (Chrome
     *  sends transport-cc roughly every 50-100 ms) and comfortably
     *  smaller than the time a human notices a frozen picture. */
    core::Milliseconds transport_cc_timeout = core::Milliseconds{3000};
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
    /** Which signal this snapshot is dominated by.
     *
     *  This is the *resolved* source: the collector applies the policy
     *  (negotiation state + freshness) to the signals it has seen, so
     *  the value answers "which signal is driving now", not "which
     *  signal arrived last". */
    NetFeedbackSource source = NetFeedbackSource::kLossRtt;

    /** When the snapshot was produced (monotonic).
     *
     *  The timestamp moves exactly when a *new* feedback report arrived,
     *  which is what lets the engine feed the estimator once per report
     *  rather than once per tick.  Re-evaluating the policy against a
     *  later instant does **not** move it. */
    core::TimePoint timestamp{};

    // ---- Receiver Report (loss / RTT / jitter) --------------------------

    /** True once a Receiver Report has been seen. */
    bool has_loss_rtt = false;

    /** When the last Receiver Report arrived. */
    core::TimePoint loss_rtt_timestamp{};

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

    /** When the last REMB arrived. */
    core::TimePoint remb_timestamp{};

    // ---- transport-cc ----------------------------------------------------

    /** True once a transport-cc feedback has been seen. */
    bool has_transport_cc = false;

    /** When the last transport-cc feedback arrived. */
    core::TimePoint transport_cc_timestamp{};

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

    /** The aggregated snapshot.
     *
     *  `source` is the *resolved* driving signal: the policy has already
     *  been applied to the signals the collector has seen. */
    virtual NetFeedbackSnapshot snapshot() const noexcept = 0;

    // ---- The policy (ADR-018 Decision 5) -------------------------------

    /** Apply the collector's configuration (policy + freshness timeout).
     *
     *  The engine calls this once at open() with the host's settings.
     *  A live change re-resolves the driving signal immediately. */
    virtual void set_config(const NetFeedbackConfig& config) noexcept = 0;

    /** The configuration in force. */
    virtual NetFeedbackConfig config() const noexcept = 0;

    /** Tell the collector what the peer advertised.
     *
     *  The engine calls this after the SDP negotiation, with the
     *  `a=rtcp-fb` facts of the remote description.  Until it is called
     *  the collector assumes the peer advertised nothing, so the
     *  transport-cc path stays off and the loss/RTT path drives. */
    virtual void set_peer_capabilities(const NetFeedbackCapabilities& caps) noexcept = 0;

    /** The capabilities in force. */
    virtual NetFeedbackCapabilities peer_capabilities() const noexcept = 0;

    /** True when the policy demands transport-cc but it is not available.
     *
     *  "Not available" means: the peer did not advertise it, no
     *  transport-cc feedback ever arrived, or the last one is older than
     *  `NetFeedbackConfig::transport_cc_timeout`.  Only meaningful under
     *  `NetFeedbackPolicy::kRequireTransportCc`; it is the "fail loudly"
     *  signal of ADR-018 Decision 2. */
    virtual bool transport_cc_unavailable() const noexcept = 0;

    /** Re-evaluate the policy against the current time.
     *
     *  The freshness axis of the policy can only be evaluated against a
     *  clock, and the collector has none of its own (its inputs carry
     *  their timestamps).  The engine calls this from its tick with its
     *  monotonic clock, so a transport-cc path that went silent is
     *  detected even when no new feedback arrives.
     *
     *  Re-evaluating never changes `NetFeedbackSnapshot::timestamp` —
     *  that timestamp marks *new data*, and the engine uses it to feed
     *  the estimator once per report. */
    virtual void evaluate(core::TimePoint now) noexcept = 0;

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
