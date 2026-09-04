/**
 * @file nimrtc/plugins/jb.hpp
 * @brief IJB — pluggable jitter buffer interface.
 *
 * Replace this to implement adaptive jitter buffers, fixed-size buffers,
 * or proprietary schemes that exploit ref_frame alignment (NimRTC §8.4).
 *
 * ## Implementing a custom JB plugin
 *
 * 1. Implement `IJB` for your buffer strategy.
 * 2. Register: `PluginRegistry::instance().register_jb("my_jb", factory);`
 * 3. Set `NimRTCEngine::Config::jb_name = "my_jb"`.
 *
 * @note P0 scaffold — interface stable, binary layout TBD P1.
 */

// base.hpp must be before include guard — see transport.hpp for rationale.
#include "nimrtc/plugins/base.hpp"
#include "nimrtc/plugins/rtp.hpp"   // RtpPacket, MediaSample

#ifndef NIMRTC_PLUGINS_JB_HPP
#define NIMRTC_PLUGINS_JB_HPP

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

struct JBConfig {
    std::string_view name;
    uint32_t ssrc = 0;
    uint32_t clock_rate_hz = 90000;
    int initial_depth_ms = 40;
    int max_depth_ms = 200;
    int min_depth_ms = 10;
    int target_delay_ms = 0;
};

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

using FrameReadyCallback = std::function<void(MediaSample)>;
using FrameDropCallback = std::function<void(uint16_t seq, TimestampUs delay_ms)>;

// ---------------------------------------------------------------------------
// IJB
// ---------------------------------------------------------------------------

class IJB : public IPlugin {
public:
    virtual void set_callbacks(FrameReadyCallback on_frame_ready,
                               FrameDropCallback  on_frame_drop) noexcept = 0;

    virtual Status
    push(const RtpPacket& pkt, TimestampUs recv_time_us) noexcept = 0;

    virtual std::optional<MediaSample>
    pop(TimestampUs now_us, TimestampUs* play_time_us = nullptr) noexcept = 0;

    virtual void flush() noexcept = 0;

    virtual void
    apply_feedback(const std::vector<RtcpPacket>& rtcp) noexcept = 0;

    struct Stats {
        int     buffer_level_ms = 0;
        int     target_level_ms = 0;
        uint64_t frames_out = 0;
        uint64_t frames_dropped = 0;
        uint64_t packets_received = 0;
        double   jitter_ms = 0.0;
    };
    virtual Stats stats() const noexcept = 0;

    virtual int estimated_delay_ms(TimestampUs now_us) const noexcept = 0;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

class IJBFactory {
public:
    virtual ~IJBFactory() = default;
    virtual std::string_view id()          const noexcept = 0;
    virtual std::string_view display_name()const noexcept = 0;
    virtual IJB* create()                  const = 0;
};

template<class T>
class SimpleJBFactory : public IJBFactory {
    std::string_view id_;
    std::string_view name_;
public:
    explicit SimpleJBFactory(std::string_view id, std::string_view name) noexcept
        : id_(id), name_(name) {}
    virtual std::string_view id()          const noexcept override { return id_; }
    virtual std::string_view display_name()const noexcept override { return name_; }
    virtual IJB* create()                  const override { return new T(); }
};

} // namespace nimrtc::plugins
#endif // NIMRTC_PLUGINS_JB_HPP
