/**
 * @file nimrtc/rtp/xr_report_plugin.hpp
 * @brief Plugin adapter: wraps the RFC 3611 XR parser behind plugins::IXrReport.
 *
 * Implements the seam declared in <nimrtc/plugins/xr_report.hpp> by
 * delegating to the concrete RTP module (`rtp::parse_xr_packet`).
 *
 * ## Type conversion
 *
 * The seam's value types (`plugins::XrPacket` / `plugins::XrBlock`) and
 * the module's (`rtp::XrPacket` / `rtp::XrBlock`) describe the same
 * wire format with the same numbering (RFC 3611 §6.2), but they are
 * distinct types on purpose: `src/plugins/` may not depend on a
 * concrete module, so the engine can only speak the seam's types.
 *
 * ## Registration
 *
 * Registered as id "nimrtc_xr" (matches
 * `EngineConfig::xr_report_name` default).  Per ADR-001; the consumer
 * MUST call `nimrtc::rtp::register_default_plugins()` once at startup.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <string_view>

#include <nimrtc/core/time.hpp>          // core::TimePoint
#include <nimrtc/plugins/xr_report.hpp>  // plugins::IXrReport / IXrReportFactory
#include <nimrtc/rtp/rtcp_xr.hpp>        // concrete rtp::XrPacket / XrBlock

namespace nimrtc::rtp {

// ---------------------------------------------------------------------------
// PluginAdapter
// ---------------------------------------------------------------------------

/**
 * @brief Wraps the concrete RFC 3611 XR parser behind plugins::IXrReport.
 *
 * The adapter is a *collector*: it keeps the most recent decoded packet
 * and the counters.  It owns no network state, so `open()` is a no-op.
 */
class NIMRTC_API_STABLE XrReportAdapter : public plugins::IXrReport {
public:
    XrReportAdapter();
    ~XrReportAdapter() override;

    XrReportAdapter(const XrReportAdapter&)            = delete;
    XrReportAdapter& operator=(const XrReportAdapter&) = delete;

    // ---- plugins::IPlugin --------------------------------------------------

    const char* name() const noexcept override;
    plugins::Status open() noexcept override;
    void          close() noexcept override;

    // ---- plugins::IXrReport ------------------------------------------------

    void on_rtcp_block(std::uint8_t payload_type,
                       core::ByteSpan block,
                       core::TimePoint now) noexcept override;

    plugins::XrReportSnapshot snapshot() const noexcept override;

    void reset() noexcept override;

private:
    /// The decoded state: the last packet plus the freshness stamp.
    plugins::XrReportSnapshot snapshot_;

    /// How many XR payload-type blocks were handed to the adapter.
    std::uint64_t packets_seen_ = 0;

    /// How many of them were malformed.
    std::uint64_t packets_malformed_ = 0;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

/**
 * @brief Factory producing XrReportAdapter instances.
 *
 * Registered as id "nimrtc_xr" (matches `EngineConfig::xr_report_name`
 * default).
 */
class NIMRTC_API_STABLE XrReportFactory : public plugins::IXrReportFactory {
public:
    std::string_view id()           const noexcept override;
    std::string_view display_name() const noexcept override;
    plugins::IXrReport* create()    const override;
};

// ---------------------------------------------------------------------------
// Public registration entry point (MSVC static-link workaround)
// ---------------------------------------------------------------------------

namespace detail {
/** Defined in xr_report_plugin.cpp. Forces .obj linkage on consumer call. */
void do_register_xr_report_plugin() noexcept;
}  // namespace detail

/**
 * @brief Register the built-in XR report consumer with core::PluginRegistry.
 *
 * This is called from the module's main `register_default_plugins()`
 * entry point (which the default-registrar table reaches), so a
 * consumer only ever has to call the one entry point.
 */
void register_xr_report_plugin() noexcept;

}  // namespace nimrtc::rtp
