/**
 * @file src/modules/jb/src/jb_plugin.cpp
 * @brief PluginAdapter + PluginFactory implementation for nimrtc::jb.
 *
 * Translates between plugins::IJB types (RtpPacket, MediaSample, TimestampUs)
 * and concrete jb::JitterBuffer / jb::Frame / core::TimePoint.
 *
 * Per ADR-001 + the MSVC static-link workaround established for audio3a/ice/rtp.
 */

#include <nimrtc/jb/jb_plugin.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <utility>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>

namespace nimrtc::jb {

// ---------------------------------------------------------------------------
// Type conversion helpers
// ---------------------------------------------------------------------------

namespace {

/** TimestampUs ↔ core::TimePoint (forward decls for use below). */
core::TimePoint us_to_time_point(plugins::TimestampUs us) noexcept;
plugins::TimestampUs time_point_to_us(core::TimePoint tp) noexcept;

core::TimePoint us_to_time_point(plugins::TimestampUs us) noexcept {
    return core::TimePoint{core::Microseconds{us}};
}

plugins::TimestampUs time_point_to_us(core::TimePoint tp) noexcept {
    return static_cast<plugins::TimestampUs>(
               std::chrono::duration_cast<std::chrono::microseconds>(
                   tp.time_since_epoch()).count());
}

/** Translate plugins::JBConfig → jb::Config (concrete). */
Config to_concrete_config(const plugins::JBConfig& p) noexcept {
    Config c;
    c.initial_delay = core::Milliseconds{p.initial_depth_ms};
    c.min_delay     = core::Milliseconds{p.min_depth_ms};
    c.max_delay     = core::Milliseconds{p.max_depth_ms};
    c.max_packets   = 512;          // plugin interface doesn't expose this
    c.adaptive      = true;
    return c;
}

/** Translate plugins::RtpPacket → rtp::PacketView.
 *  The plugin RtpPacket owns its ext.data buffer (vector); the concrete
 *  PacketView references those bytes via core::ByteSpan. */
rtp::PacketView plugin_to_view(const plugins::RtpPacket& pkt) noexcept {
    rtp::PacketView v;
    v.payload      = pkt.payload;          // BufferView == core::ByteSpan
    v.raw          = pkt.raw;
    v.ssrc         = pkt.header.ssrc;
    v.seq          = pkt.header.seq;
    v.timestamp    = pkt.header.ts;
    v.payload_type = pkt.header.payload_type;
    v.marker       = pkt.header.marker;
    if (!pkt.header.csrc.empty()) {
        v.csrc.assign(pkt.header.csrc.begin(), pkt.header.csrc.end());
    }
    if (pkt.header.ext.has_value()) {
        rtp::Extension ext;
        ext.type = pkt.header.ext->profile;
        ext.data = core::ByteSpan(pkt.header.ext->data.data(),
                                  pkt.header.ext->data.size());
        v.extension = ext;
    }
    return v;
}

/** Translate jb::Frame → plugins::MediaSample (first packet becomes MediaSample;
 *  multi-packet frames collapse to single MediaSample — the plugin interface
 *  doesn't model multi-packet frames). */
plugins::MediaSample frame_to_media_sample(const jb::Frame& f) noexcept {
    plugins::MediaSample s;
    s.kind = plugins::MediaSample::Kind::kAudio;   // default; consumer refines
    if (!f.packets.empty()) {
        const auto& p = f.packets.front();
        s.ssrc      = p.ssrc;
        s.rtp_ts    = p.timestamp;
        s.payload   = p.payload;
        s.seq       = p.seq;
        // is_keyframe: only meaningful for video; jb::Frame doesn't track it
    }
    s.media_us = f.first_arrival.has_value()
                     ? time_point_to_us(f.first_arrival.value())
                     : 0;
    return s;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// PluginAdapter
// ---------------------------------------------------------------------------

PluginAdapter::PluginAdapter(plugins::JBConfig config)
    : concrete_(nullptr)
    , plugin_config_(config) {
    concrete_config_ = to_concrete_config(config);
    concrete_        = std::make_unique<jb::JitterBuffer>(concrete_config_);
    core::log::Logger::instance().debug(
        std::string{"jb::PluginAdapter created (ssrc="} +
        std::to_string(config.ssrc) +
        ", initial=" + std::to_string(config.initial_depth_ms) + "ms)");
}

PluginAdapter::~PluginAdapter() = default;

const char* PluginAdapter::name() const noexcept {
    return "nimrtc::jb::PluginAdapter (adaptive jitter buffer)";
}

plugins::Status PluginAdapter::open() noexcept {
    if (!concrete_) return plugins::kErrInternal;
    return plugins::kOk;
}

void PluginAdapter::close() noexcept {
    if (concrete_) concrete_->reset();
    frames_out_      = 0;
    frames_dropped_  = 0;
    packets_received_ = 0;
}

void PluginAdapter::set_callbacks(plugins::FrameReadyCallback on_frame_ready,
                                 plugins::FrameDropCallback  on_frame_drop) noexcept {
    on_frame_ready_ = std::move(on_frame_ready);
    on_frame_drop_  = std::move(on_frame_drop);
}

plugins::Status PluginAdapter::push(const plugins::RtpPacket& pkt,
                                     plugins::TimestampUs recv_time_us) noexcept {
    if (!concrete_) return plugins::kErrInternal;
    if (pkt.raw.empty()) return plugins::kErrInvalidParam;
    auto view = plugin_to_view(pkt);
    auto arrival = us_to_time_point(recv_time_us);
    concrete_->push(view, arrival, std::nullopt);
    ++packets_received_;
    return plugins::kOk;
}

std::optional<plugins::MediaSample>
PluginAdapter::pop(plugins::TimestampUs now_us,
                   plugins::TimestampUs* play_time_us) noexcept {
    if (!concrete_) return std::nullopt;
    auto now = us_to_time_point(now_us);
    auto frame = concrete_->pop(now);
    if (!frame.has_value()) return std::nullopt;
    if (play_time_us) {
        *play_time_us = time_point_to_us(now);
    }
    ++frames_out_;
    auto sample = frame_to_media_sample(frame.value());
    if (on_frame_ready_) on_frame_ready_(sample);
    return sample;
}

void PluginAdapter::flush() noexcept {
    if (!concrete_) return;
    auto frames = concrete_->flush();
    frames_dropped_ += frames.size();
    for (auto& f : frames) {
        if (on_frame_drop_ && !f.packets.empty()) {
            on_frame_drop_(f.packets.front().seq, 0);
        }
    }
}

void PluginAdapter::apply_feedback(const std::vector<plugins::RtcpPacket>& /*rtcp*/) noexcept {
    // jb::JitterBuffer doesn't accept feedback directly; the concrete module's
    // adaptive logic uses local jitter estimation only. Adapter is a no-op
    // here; future versions may add support.
}

plugins::IJB::Stats PluginAdapter::stats() const noexcept {
    plugins::IJB::Stats s;
    if (concrete_) {
        auto cs = concrete_->stats();
        s.buffer_level_ms = static_cast<int>(cs.current_delay.count());
        s.target_level_ms = static_cast<int>(cs.current_delay.count());
        s.frames_out      = cs.frames_emitted;
        s.frames_dropped  = cs.packets_dropped;
        s.jitter_ms       = static_cast<double>(cs.current_jitter.count()) / 1000.0;
    }
    s.packets_received = packets_received_.load(std::memory_order_relaxed);
    return s;
}

int PluginAdapter::estimated_delay_ms(plugins::TimestampUs /*now_us*/) const noexcept {
    if (!concrete_) return 0;
    return static_cast<int>(concrete_->stats().current_delay.count());
}

// ---------------------------------------------------------------------------
// PluginFactory
// ---------------------------------------------------------------------------

std::string_view PluginFactory::id() const noexcept {
    return "adaptive";   // matches EngineConfig::jb_name default
}

std::string_view PluginFactory::display_name() const noexcept {
    return "Jitter Buffer — adaptive (RFC 3550-style with ref_frame hook)";
}

plugins::IJB* PluginFactory::create() const {
    plugins::JBConfig cfg;
    cfg.name            = "adaptive";
    cfg.ssrc            = 0;
    cfg.clock_rate_hz   = 90000;
    cfg.initial_depth_ms = 40;
    cfg.max_depth_ms    = 200;
    cfg.min_depth_ms    = 10;
    return new PluginAdapter(cfg);
}

// ---------------------------------------------------------------------------
// Public registration entry point
// ---------------------------------------------------------------------------

namespace detail {

void do_register_default_plugins() noexcept {
    static const struct Registrar {
        Registrar() {
            static nimrtc::jb::PluginFactory s_factory{};
            nimrtc::core::PluginRegistry::instance().register_jb(
                std::string_view{s_factory.id()}, &s_factory);
        }
    } s_registrar;
    (void)s_registrar;
}

} // namespace detail

// Non-inline (declared in jb_plugin.hpp) so the symbol is guaranteed
// in nimrtc_jb.lib for consumers that link via static lib + PluginRegistry.
void register_default_plugins() noexcept {
    static const int once = []() {
        detail::do_register_default_plugins();
        return 1;
    }();
    (void)once;
}

} // namespace nimrtc::jb
