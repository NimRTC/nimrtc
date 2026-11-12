/**
 * @file src/modules/rtp/src/xr_report_plugin.cpp
 * @brief Plugin adapter for the RTCP Extended Report seam.
 *
 * v1.2.0 ??`docs/plan/session-capability-plan.md` X1.
 *
 * Translates between the seam's neutral value types
 * (`plugins::XrPacket` / `plugins::XrBlock`) and the concrete module's
 * wire-level types (`rtp::XrPacket` / `rtp::XrBlock`), and registers
 * the adapter behind `plugins::IXrReportFactory` under the id
 * "nimrtc_xr".
 *
 * The engine reaches this seam only through the registry's XR slot
 * (`core::PluginRegistry::get_xr_report(EngineConfig::xr_report_name)`)
 * and never names this module's headers (Layout Invariant 4).
 *
 * Per ADR-001 + the MSVC static-link workaround established for
 * audio3a / ice: the explicit-registration entry point
 * `register_default_plugins()` forces the .obj (with its registrar) into
 * the consumer's link.
 */

#include <nimrtc/rtp/xr_report_plugin.hpp>

#include <cstdint>
#include <utility>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>

namespace nimrtc::rtp {

namespace {

// ---------------------------------------------------------------------------
// Type conversion helpers
// ---------------------------------------------------------------------------

/// Map the concrete block type onto the seam's enum.  The two enums
/// share their numbering (both follow the IANA "RTCP XR Block Type"
/// registry, RFC 3611 ?6.2), but the mapping is written out rather
/// than cast so that a divergence in either enum is a compile error
/// here instead of a silent mis-parse on the wire.
plugins::XrBlockType to_plugin_block_type(XrBlockType t) noexcept {
    switch (t) {
        case XrBlockType::kLossRle:            return plugins::XrBlockType::kLossRle;
        case XrBlockType::kDuplicateRle:       return plugins::XrBlockType::kDuplicateRle;
        case XrBlockType::kPacketReceiptTimes: return plugins::XrBlockType::kPacketReceiptTimes;
        case XrBlockType::kReceiverReferenceTime: return plugins::XrBlockType::kReceiverReferenceTime;
        case XrBlockType::kDlrr:               return plugins::XrBlockType::kDlrr;
        case XrBlockType::kStatisticsSummary:  return plugins::XrBlockType::kStatisticsSummary;
        case XrBlockType::kVoipMetrics:        return plugins::XrBlockType::kVoipMetrics;
        case XrBlockType::kUnknown:            break;
    }
    return plugins::XrBlockType::kUnknown;
}

/// Convert one concrete block into its seam-side counterpart.
plugins::XrBlock to_plugin_block(const XrBlock& b) noexcept {
    plugins::XrBlock out;
    out.type           = to_plugin_block_type(b.type);
    out.type_specific  = b.type_specific;
    out.receiver_reference_time.ntp_timestamp = b.receiver_reference_time.ntp_timestamp;

    out.dlrr.sub_reports.reserve(b.dlrr.sub_reports.size());
    for (const XrDlrrSubReport& sr : b.dlrr.sub_reports) {
        plugins::XrDlrrSubReport p;
        p.ssrc    = sr.ssrc;
        p.last_rr = sr.last_rr;
        p.dlrr    = sr.dlrr;
        out.dlrr.sub_reports.push_back(p);
    }

    const XrStatisticsSummary& s = b.statistics_summary;
    out.statistics_summary.ssrc                = s.ssrc;
    out.statistics_summary.begin_seq           = s.begin_seq;
    out.statistics_summary.end_seq             = s.end_seq;
    out.statistics_summary.has_loss            = s.has_loss;
    out.statistics_summary.lost_packets        = s.lost_packets;
    out.statistics_summary.has_duplicates      = s.has_duplicates;
    out.statistics_summary.dup_packets         = s.dup_packets;
    out.statistics_summary.has_jitter          = s.has_jitter;
    out.statistics_summary.min_jitter          = s.min_jitter;
    out.statistics_summary.max_jitter          = s.max_jitter;
    out.statistics_summary.mean_jitter         = s.mean_jitter;
    out.statistics_summary.dev_jitter          = s.dev_jitter;
    out.statistics_summary.ttl_or_hop_limit_kind = s.ttl_or_hop_limit_kind;
    out.statistics_summary.min_ttl_or_hl        = s.min_ttl_or_hl;
    out.statistics_summary.max_ttl_or_hl        = s.max_ttl_or_hl;
    out.statistics_summary.mean_ttl_or_hl       = s.mean_ttl_or_hl;
    out.statistics_summary.dev_ttl_or_hl        = s.dev_ttl_or_hl;

    return out;
}

/// Convert a whole concrete packet into its seam-side counterpart.
plugins::XrPacket to_plugin_packet(const XrPacket& p) noexcept {
    plugins::XrPacket out;
    out.ssrc = p.ssrc;
    out.blocks.reserve(p.blocks.size());
    for (const XrBlock& b : p.blocks) {
        out.blocks.push_back(to_plugin_block(b));
    }
    return out;
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// XrReportAdapter
// ---------------------------------------------------------------------------

XrReportAdapter::XrReportAdapter() = default;
XrReportAdapter::~XrReportAdapter() = default;

const char* XrReportAdapter::name() const noexcept {
    return "nimrtc::rtp::XrReportAdapter (RFC 3611 RTCP XR)";
}

plugins::Status XrReportAdapter::open() noexcept {
    // The XR consumer is stateless at the network level; nothing to do.
    return plugins::kOk;
}

void XrReportAdapter::close() noexcept {
    // No-op.
}

void XrReportAdapter::on_rtcp_block(std::uint8_t payload_type,
                                  core::ByteSpan block,
                                  core::TimePoint now) noexcept {
    // Only the XR payload type is this seam's business (RFC 3611 section 2).
    if (payload_type != kXrPayloadType) return;

    ++packets_seen_;

    auto res = parse_xr_packet(block);
    if (!res) {
        // A broken report from the peer must not fail the read path:
        // count it and move on.
        ++packets_malformed_;
        core::log::Logger::instance().debug(
            "rtp[xr]: malformed XR packet ignored: " +
            res.error().message());
        return;
    }

    snapshot_.has_data      = true;
    snapshot_.timestamp     = now;
    snapshot_.packets_received += 1;
    snapshot_.last_ssrc     = res.value().ssrc;
    snapshot_.last_packet   = to_plugin_packet(res.value());
}

plugins::XrReportSnapshot XrReportAdapter::snapshot() const noexcept {
    plugins::XrReportSnapshot out = snapshot_;
    out.packets_malformed = packets_malformed_;
    return out;
}

void XrReportAdapter::reset() noexcept {
    snapshot_ = plugins::XrReportSnapshot{};
    packets_seen_       = 0;
    packets_malformed_  = 0;
}

// ---------------------------------------------------------------------------
// XrReportFactory
// ---------------------------------------------------------------------------

std::string_view XrReportFactory::id() const noexcept {
    return "nimrtc_xr";
}

std::string_view XrReportFactory::display_name() const noexcept {
    return "RTCP XR - RFC 3611 extended reports (BT 4 / 5 / 6)";
}

plugins::IXrReport* XrReportFactory::create() const {
    return new XrReportAdapter();
}

// ---------------------------------------------------------------------------
// Public registration entry point
// ---------------------------------------------------------------------------

namespace detail {

void do_register_xr_report_plugin() noexcept {
    static const struct Registrar {
        Registrar() {
            static nimrtc::rtp::XrReportFactory s_factory{};
            nimrtc::core::PluginRegistry::instance().register_xr_report(
                std::string_view{s_factory.id()}, &s_factory);
        }
    } s_registrar;
    (void)s_registrar;
}

}  // namespace detail

// Non-inline (declared in xr_report_plugin.hpp) so the symbol is
// guaranteed in nimrtc_rtp.lib for consumers that link via static lib +
// PluginRegistry.
void register_xr_report_plugin() noexcept {
    static const int once = []() {
        detail::do_register_xr_report_plugin();
        return 1;
    }();
    (void)once;
}

}  // namespace nimrtc::rtp
