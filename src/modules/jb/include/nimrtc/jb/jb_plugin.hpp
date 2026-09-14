/**
 * @file nimrtc/jb/jb_plugin.hpp
 * @brief Plugin adapter: wraps nimrtc::jb::JitterBuffer behind plugins::IJB.
 *
 * Implements the plugin interface defined in <nimrtc/plugins/jb.hpp> by
 * delegating to the concrete JB module (jb::JitterBuffer + jb::Config).
 *
 * ## Type conversion
 *
 * - `plugins::RtpPacket`   ↔ `rtp::PacketView` (forwarded via RTP plugin
 *   conversion logic; both are view types — caller must keep source buffer
 *   alive)
 * - `plugins::TimestampUs` ↔ `core::TimePoint` (microseconds ↔ steady_clock)
 * - `plugins::MediaSample` ← `jb::Frame` (first packet becomes the MediaSample;
 *   multi-packet frames collapse to single MediaSample)
 *
 * ## SSRC handling
 *
 * The plugin's `push()` doesn't carry an SSRC; the SSRC inside `RtpPacket.header`
 * is used as the jitter-buffer key (concrete JitterBuffer is implicitly
 * keyed by RTP timestamp; we route by SSRC at the adapter layer if needed).
 *
 * @note P1.1 (R2). Adapter only — engine refactor (drop direct concrete
 *       includes) is a separate step.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include <nimrtc/jb/jitter_buffer.hpp>   // concrete jb::JitterBuffer / Frame / Config
#include <nimrtc/plugins/jb.hpp>          // plugins::IJB / JBConfig / MediaSample
#include <nimrtc/plugins/rtp.hpp>         // RtpPacket / RtpHeader

namespace nimrtc::jb {

// ---------------------------------------------------------------------------
// PluginAdapter
// ---------------------------------------------------------------------------

/**
 * @brief Wraps a concrete jb::JitterBuffer behind plugins::IJB.
 *
 * Adapter is constructed per SSRC (concrete JitterBuffer holds state for
 * one stream). Use PluginFactory::create() per stream.
 */
class PluginAdapter : public plugins::IJB {
public:
    explicit PluginAdapter(plugins::JBConfig config);
    ~PluginAdapter() override;

    PluginAdapter(const PluginAdapter&)            = delete;
    PluginAdapter& operator=(const PluginAdapter&) = delete;

    // ---- plugins::IPlugin --------------------------------------------------

    const char* name() const noexcept override;
    plugins::Status open() noexcept override;
    void          close() noexcept override;

    // ---- plugins::IJB -----------------------------------------------------

    void set_callbacks(plugins::FrameReadyCallback on_frame_ready,
                       plugins::FrameDropCallback  on_frame_drop) noexcept override;

    plugins::Status push(const plugins::RtpPacket& pkt,
                          plugins::TimestampUs recv_time_us) noexcept override;

    std::optional<plugins::MediaSample>
    pop(plugins::TimestampUs now_us,
        plugins::TimestampUs* play_time_us = nullptr) noexcept override;

    void flush() noexcept override;

    void apply_feedback(const std::vector<plugins::RtcpPacket>& rtcp) noexcept override;

    plugins::IJB::Stats stats() const noexcept override;

    int estimated_delay_ms(plugins::TimestampUs now_us) const noexcept override;

private:
    /** Concrete jitter buffer (jb::JitterBuffer). */
    std::unique_ptr<jb::JitterBuffer> concrete_;

    /** Plugin-side config, retained for diagnostics. */
    plugins::JBConfig plugin_config_;

    plugins::FrameReadyCallback on_frame_ready_;
    plugins::FrameDropCallback  on_frame_drop_;

    /** Cached concrete Config (translated from plugin::JBConfig on open). */
    Config concrete_config_{};

    // Stats counters (mutable: stats() / pop() are const).
    mutable std::atomic<std::uint64_t> frames_out_{0};
    mutable std::atomic<std::uint64_t> frames_dropped_{0};
    mutable std::atomic<std::uint64_t> packets_received_{0};
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

/**
 * @brief Factory producing PluginAdapter instances for one jitter-buffer stream.
 *
 * Registered as id "adaptive" (matches EngineConfig::jb_name default).
 * create() returns a new PluginAdapter with default 40 ms initial delay.
 */
class PluginFactory : public plugins::IJBFactory {
public:
    std::string_view id()           const noexcept override;
    std::string_view display_name() const noexcept override;
    plugins::IJB*    create()      const override;
};

// ---------------------------------------------------------------------------
// Public registration entry point (MSVC static-link workaround)
// ---------------------------------------------------------------------------

namespace detail {
/** Defined in jb_plugin.cpp. Forces .obj linkage on consumer call. */
void do_register_default_plugins() noexcept;
} // namespace detail

/**
 * @brief Register all built-in JB plugins with core::PluginRegistry.
 *
 * @note Not `inline` because the static-local latch would otherwise be
 *       emitted as a weak external symbol that the static lib doesn't
 *       carry; non-inline ensures the symbol is in `nimrtc_jb.lib`.
 */
void register_default_plugins() noexcept;

} // namespace nimrtc::jb
