/**
 * @file nimrtc/plugins/xr_report.hpp
 * @brief IXrReport — the pluggable *RTCP Extended Report* (RFC 3611) seam.
 *
 * v1.2.0 — `docs/plan/session-capability-plan.md` X1 (and X2 groundwork).
 *
 * ## What this seam is for
 *
 * RFC 3611 defines the XR packet (RTCP payload type 207): an 8-byte
 * header followed by a stack of *extended report blocks*.  Every block
 * starts with a 4-byte header carrying the block's type (`BT`), a
 * type-specific byte and a length in 32-bit words **minus one** — and
 * that length is exactly what makes the format extensible: a receiver
 * that does not know a block type walks past it by its length instead
 * of failing.
 *
 * Until v1.2.0 NimRTC *recognised* PT 207 in the compound demultiplexer
 * (`RtcpBlockKind::kExtendedReport`) and did nothing else with it: the
 * sub-packet came out of `demux_rtcp()` as an opaque byte span that
 * nothing consumed.
 *
 * ## Why the seam takes (payload type, bytes) and not parsed structs
 *
 * The plugin layer may not depend on a concrete module (`src/plugins/`
 * is an interface-only library, Layout Invariant 2), so it cannot name
 * `rtp::XrPacket` or `rtp::XrBlock`.  The seam therefore defines its
 * *own* value types — a neutral, wire-shaped vocabulary that both the
 * engine and the module implementation speak — and the module-side
 * adapter translates between them.
 *
 * This mirrors the `INetFeedback` seam, which likewise hands the
 * collector the raw sub-packet plus the payload type / FMT so the
 * collector can decide what it understands.
 *
 * ## The block types
 *
 * RFC 3611 §6.2 registers seven block types.  NimRTC carries the three
 * that carry round-trip measurement and network statistics:
 *
 * | BT | Block                | RFC 3611 |
 * |----|----------------------|----------|
 * | 4  | Receiver Reference Time | §4.4   |
 * | 5  | DLRR (Delay since Last Receiver Report) | §4.5 |
 * | 6  | Statistics Summary   | §4.6     |
 *
 * The other four — Loss RLE (1), Duplicate RLE (2), Packet Receipt
 * Times (3), VoIP Metrics (7) — are *recognised* (they are skipped
 * cleanly by the block walk) but not parsed.  They carry no field any
 * NimRTC consumer reads today, and parsing them would be dead code.
 *
 * @note There is **no** block type named "Receiver Estimated Quality"
 *       in RFC 3611: that was the name of an early draft's block.  A
 *       report built with the draft's numbering would not interoperate
 *       with a conforming peer, so this seam only speaks the standard's
 *       actual numbering.
 *
 * ## Relationship to the engine
 *
 * The engine resolves the seam through the registry's XR slot:
 *
 *     core::PluginRegistry::instance().get_xr_report(
 *         EngineConfig::xr_report_name)
 *
 * and drives it through the interface declared here.  The built-in
 * implementation lives in `src/modules/rtp/` and registers under the id
 * "nimrtc_xr"; the engine never includes that module's headers
 * (Layout Invariant 4).
 *
 * ## Thread safety
 *
 * The engine drives the seam from the engine thread — the same thread
 * that reads `snapshot()`.  Implementations need not be reentrant.
 */

// base.hpp must be before include guard — see transport.hpp for rationale.
#include <nimrtc/core/api_version.hpp>
#include "nimrtc/plugins/base.hpp"

#ifndef NIMRTC_PLUGINS_XR_REPORT_HPP
#define NIMRTC_PLUGINS_XR_REPORT_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.

#include <cstdint>
#include <string_view>
#include <vector>

#include <nimrtc/core/time.hpp>   // core::TimePoint

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// The block types (RFC 3611 §6.2)
// ---------------------------------------------------------------------------

/** The extended-report block types NimRTC knows about.
 *
 *  The values are the IANA "RTCP XR Block Type" registry (RFC 3611
 *  §6.2).  A block whose type is not listed here is *skipped*, not an
 *  error — that is what the block's length field is for. */
enum class NIMRTC_API_STABLE XrBlockType : std::uint8_t {
    kUnknown                = 0,
    kLossRle                = 1,  ///< RFC 3611 §4.1 — recognised, not parsed.
    kDuplicateRle           = 2,  ///< RFC 3611 §4.2 — recognised, not parsed.
    kPacketReceiptTimes     = 3,  ///< RFC 3611 §4.3 — recognised, not parsed.
    kReceiverReferenceTime  = 4,  ///< RFC 3611 §4.4 — parsed.
    kDlrr                   = 5,  ///< RFC 3611 §4.5 — parsed.
    kStatisticsSummary      = 6,  ///< RFC 3611 §4.6 — parsed.
    kVoipMetrics            = 7,  ///< RFC 3611 §4.7 — recognised, not parsed.
};

// ---------------------------------------------------------------------------
// The parsed blocks
// ---------------------------------------------------------------------------

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
 *  The block reports loss, duplicates, jitter and TTL / Hop Limit over
 *  a sequence-number interval.  The flag bits say which of the groups
 *  the block actually carries; the fields of an unreported group are
 *  present but zero, and the receiver MUST ignore them (RFC 3611
 *  §4.6). */
struct NIMRTC_API_STABLE XrStatisticsSummary {
    /// The SSRC of the source the statistics describe.
    std::uint32_t ssrc = 0;

    /// The first sequence number of the reported interval.
    ///
    /// RFC 3611 §4.1: "The first sequence number that this block
    /// reports on."
    std::uint16_t begin_seq = 0;

    /// The *exclusive* end of the reported interval.
    ///
    /// RFC 3611 §4.1: "The last sequence number that this block
    /// reports on **plus one**."  So the field is one past the last
    /// sequence number the block covers, and the interval therefore
    /// holds `end_seq - begin_seq` sequence numbers.
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

// ---------------------------------------------------------------------------
// The aggregated snapshot
// ---------------------------------------------------------------------------

/** What the seam knows about the peer's extended reports.
 *
 *  The snapshot is the *host-facing* view: it answers "has the peer
 *  told us anything yet, and what did it say", without exposing the
 *  wire format.  The per-block detail lives in `last_packet`. */
struct NIMRTC_API_STABLE XrReportSnapshot {
    /** True once at least one XR packet has been received. */
    bool has_data = false;

    /** When the last XR packet arrived (monotonic). */
    core::TimePoint timestamp{};

    /** How many XR packets have been received since the last reset. */
    std::uint64_t packets_received = 0;

    /** How many XR packets were rejected as malformed. */
    std::uint64_t packets_malformed = 0;

    /** The SSRC of the originator of the last XR packet. */
    std::uint32_t last_ssrc = 0;

    /** The blocks of the last XR packet, in wire order. */
    XrPacket last_packet;
};

// ---------------------------------------------------------------------------
// IXrReport
// ---------------------------------------------------------------------------

/** Pluggable consumer of RTCP Extended Reports (RFC 3611).
 *
 *  Implementations consume *demultiplexed* RTCP sub-packets: the engine
 *  splits the compound datagram (RFC 3550 §6.1) and hands each
 *  sub-packet to the seam.  A sub-packet the implementation does not
 *  understand must be ignored — in particular, a PT that is not 207 is
 *  never this seam's business. */
class NIMRTC_API_STABLE IXrReport : public IPlugin {
public:
    ~IXrReport() override = default;

    // IPlugin -----------------------------------------------------------
    const char* name() const noexcept override = 0;
    Status      open() noexcept override = 0;
    void        close() noexcept override = 0;

    // Collector contract -------------------------------------------------

    /** Feed one RTCP sub-packet.
     *
     *  @param payload_type  The RTCP payload type.  Only 207 (XR) is
     *                       this seam's business; anything else is
     *                       ignored.
     *  @param block         The whole sub-packet, header included.
     *  @param now           The monotonic instant the packet arrived.
     *
     *  A malformed XR packet is *counted*, not thrown: the read path
     *  must not fail because one peer sent a broken report.
     */
    virtual void on_rtcp_block(std::uint8_t payload_type,
                               core::ByteSpan block,
                               core::TimePoint now) noexcept = 0;

    /** The aggregated snapshot. */
    virtual XrReportSnapshot snapshot() const noexcept = 0;

    /** Reset the collector (e.g. on stream restart). */
    virtual void reset() noexcept = 0;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

/** Factory for RTCP Extended Report consumers. */
class NIMRTC_API_STABLE IXrReportFactory {
public:
    virtual ~IXrReportFactory() = default;
    virtual std::string_view id()           const noexcept = 0;
    virtual std::string_view display_name() const noexcept = 0;

    /** Create a new consumer instance. */
    virtual IXrReport* create() const = 0;
};

/** Template helper — creates a factory that owns an XR consumer type T. */
template<class T>
class NIMRTC_API_STABLE SimpleXrReportFactory : public IXrReportFactory {
    std::string_view id_;
    std::string_view name_;
public:
    explicit SimpleXrReportFactory(std::string_view id,
                                   std::string_view name) noexcept
        : id_(id), name_(name) {}
    std::string_view id()           const noexcept override { return id_; }
    std::string_view display_name() const noexcept override { return name_; }
    IXrReport* create()             const override { return new T(); }
};

} // namespace plugins
#endif // NIMRTC_PLUGINS_XR_REPORT_HPP
