/**
 * @file nimrtc/rtp/rtp_plugin.hpp
 * @brief Plugin adapter: wraps nimrtc::rtp::Parser / PacketBuilder behind plugins::IRTP.
 *
 * Implements the plugin interface defined in <nimrtc/plugins/rtp.hpp> by
 * delegating to the concrete RTP module (rtp::Parser + rtp::PacketBuilder).
 *
 * ## Type conversion
 *
 * - `plugins::RtpHeader`  ↔ `rtp::PacketView`'s header fields (no 1:1 struct)
 * - `plugins::RtpPacket`  ← `rtp::PacketView` (view-conversion only)
 * - `plugins::RtcpType`   ≈ `rtp::RtcpType` (same RFC values; aliased)
 *
 * ## Lifetime contract
 *
 * `parse_packet(BufferView)` returns an RtpPacket whose `payload` and `raw`
 * views point INTO the caller-supplied buffer. The caller must keep that
 * buffer alive for as long as the RtpPacket is in use (same as the rtp
 * module's PacketView contract).
 *
 * ## Registration
 *
 * Registered as id "webrtc" (matches EngineConfig::rtp_name default).
 * Per ADR-001; consumer MUST call `nimrtc::rtp::register_default_plugins()`
 * once at startup (see audio3a_plugin.hpp / ice.hpp for the same pattern).
 */

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <nimrtc/plugins/rtp.hpp>     // plugins::IRTP / IRTPFactory / RtpPacket / RtpHeader
#include <nimrtc/rtp/packet.hpp>       // concrete rtp::Parser, PacketBuilder, PacketView, RTCP types

namespace nimrtc::rtp {

// ---------------------------------------------------------------------------
// PluginAdapter
// ---------------------------------------------------------------------------

/**
 * @brief Wraps a concrete rtp::Parser + rtp::PacketBuilder behind plugins::IRTP.
 *
 * Construction is parameterless (parser is stateless). open() with the
 * ITransport-style API is a no-op (RTP has no network state); kept for
 * IPlugin uniformity.
 */
class PluginAdapter : public plugins::IRTP {
public:
    PluginAdapter();
    ~PluginAdapter() override;

    PluginAdapter(const PluginAdapter&)            = delete;
    PluginAdapter& operator=(const PluginAdapter&) = delete;

    // ---- plugins::IPlugin --------------------------------------------------

    const char* name() const noexcept override;
    plugins::Status open() noexcept override;
    void          close() noexcept override;

    // ---- plugins::IRTP ----------------------------------------------------

    void set_callbacks(plugins::RtpRecvCallback     on_rtp,
                       plugins::RtcpRecvCallback    on_rtcp,
                       plugins::RtpParseErrorCallback on_error) noexcept override;

    std::optional<plugins::RtpPacket>
    parse_packet(plugins::BufferView raw) const noexcept override;

    std::size_t build_packet(const plugins::RtpHeader& hdr,
                             plugins::BufferView payload,
                             plugins::OutPacket& out) const noexcept override;

    std::vector<plugins::RtcpPacket>
    parse_rtcp(plugins::BufferView raw) const noexcept override;

    std::size_t build_rtcp(const std::vector<plugins::RtcpPacket>& compound,
                           plugins::OutPacket& out) const noexcept override;

    void record_inbound(const plugins::RtpPacket& pkt,
                        plugins::TimestampUs now_us) noexcept override;

    plugins::IRTP::Stats stats() const noexcept override;

private:
    /** Concrete-side parser. Stateless — owned for API uniformity only. */
    std::unique_ptr<rtp::Parser> parser_;

    plugins::RtpRecvCallback       on_rtp_;
    plugins::RtcpRecvCallback      on_rtcp_;
    plugins::RtpParseErrorCallback on_error_;

    // Stats counters (atomic for thread safety; record_inbound may be called
    // from a different thread than stats()).
    mutable std::atomic<std::uint64_t> packets_in_{0};
    mutable std::atomic<std::uint64_t> bytes_in_{0};
    mutable std::atomic<std::uint64_t> packets_out_{0};
    mutable std::atomic<std::uint64_t> bytes_out_{0};
    mutable std::atomic<std::uint64_t> parse_errors_{0};
    mutable std::atomic<std::uint32_t> last_seq_{0};
    mutable std::atomic<plugins::TimestampUs> last_recv_us_{0};
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

/**
 * @brief Factory producing PluginAdapter instances.
 *
 * Registered as id "webrtc" (matches EngineConfig::rtp_name default).
 */
class PluginFactory : public plugins::IRTPFactory {
public:
    std::string_view id()           const noexcept override;
    std::string_view display_name() const noexcept override;
    plugins::IRTP*   create()      const override;
};

// ---------------------------------------------------------------------------
// Public registration entry point (MSVC static-link workaround)
// ---------------------------------------------------------------------------

namespace detail {
/** Defined in rtp_plugin.cpp. Forces .obj linkage on consumer call. */
void do_register_default_plugins() noexcept;
} // namespace detail

/**
 * @brief Register all built-in RTP plugins with core::PluginRegistry.
 *
 * @note Not `inline` because the static-local latch would otherwise be
 *       emitted as a weak external symbol that the static lib doesn't
 *       carry; non-inline ensures the symbol is in `nimrtc_rtp.lib`.
 */
void register_default_plugins() noexcept;

} // namespace nimrtc::rtp
