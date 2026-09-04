/**
 * @file src/modules/jb/src/jitter_buffer.cpp
 * @brief JitterBuffer — fixed-window implementation (P1).
 */

#include <nimrtc/jb/jitter_buffer.hpp>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <optional>
#include <utility>

#include <nimrtc/core/log.hpp>

namespace nimrtc::jb {

namespace {

// Default playout delays
constexpr core::Milliseconds kDefaultAudioDelay{60};
constexpr core::Milliseconds kDefaultVideoDelay{100};

inline void jb_debug(const char* fmt, ...) {
    if (core::log::Logger::instance().level() <= core::log::Level::Debug) {
        char buf[256];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        core::log::Logger::instance().debug(buf);
    }
}

inline void jb_trace(const char* fmt, ...) {
    if (core::log::Logger::instance().level() <= core::log::Level::Trace) {
        char buf[256];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        core::log::Logger::instance().trace(buf);
    }
}

} // anonymous namespace

// -----------------------------------------------------------------------------
// JitterBuffer::Impl
// -----------------------------------------------------------------------------

struct JitterBuffer::Impl {
    explicit Impl(Config config) : config_(config) {
        // Initialise target_delay_ so stats() is well-defined before any
        // push(). The test JbJitterBuffer.ConstructsWithDefaultConfig expects
        // current_delay == 0ms on construction, so leave it at 0 here and let
        // push() bootstrap it to config_.initial_delay on first packet.
        target_delay_ = core::Milliseconds{};
    }

    Config config_;

    // Buffered frames: timestamp → Frame
    std::map<std::uint32_t, Frame> frame_buffer_;

    // Track arrival time of first packet in each frame
    std::map<std::uint32_t, core::TimePoint> frame_arrival_times_;

    // Last received sequence number
    std::optional<std::uint16_t> last_seq_;
    std::uint64_t packets_received_ = 0;
    std::uint64_t packets_dropped_ = 0;
    std::uint64_t frames_emitted_ = 0;

    // Jitter estimation
    double jitter_estimate_ = 0.0;

    // Current target delay
    core::Milliseconds target_delay_;

    // Statistics
    std::size_t buffered_packets_ = 0;

    void push(rtp::PacketView packet, core::TimePoint arrival,
              std::optional<std::uint64_t> ref_frame) {
        ++packets_received_;

        // Check for sequence gap
        if (last_seq_.has_value()) {
            std::uint16_t expected_seq = static_cast<std::uint16_t>(*last_seq_ + 1);
            if (packet.seq != expected_seq) {
                jb_debug("JB: seq gap detected: expected %u, got %u",
                         expected_seq, packet.seq);
            }
        }
        last_seq_ = packet.seq;

        // Determine audio vs video based on payload type
        bool is_audio = (packet.payload_type <= 23) ||
                        (packet.payload_type >= 32 && packet.payload_type <= 35) ||
                        (packet.payload_type >= 64 && packet.payload_type <= 76) ||
                        packet.payload_type == 111;  // opus

        core::Milliseconds base_delay = is_audio ? kDefaultAudioDelay : kDefaultVideoDelay;

        // Initialize target delay
        if (target_delay_.count() == 0) {
            target_delay_ = config_.initial_delay.count() > 0
                            ? config_.initial_delay
                            : base_delay;
        }

        // Add packet to frame buffer
        auto& frame = frame_buffer_[packet.timestamp];
        frame.packets.push_back(packet);
        frame.frame_timestamp = packet.timestamp;

        // Set capture latency
        if (frame.first_arrival.has_value()) {
            frame.capture_latency = arrival - *frame.first_arrival;
        } else {
            frame.first_arrival = arrival;
        }

        // Set ref_frame if provided
        if (ref_frame.has_value()) {
            frame.ref_frame = ref_frame;
        }

        // Store arrival time for this frame
        if (frame_arrival_times_.find(packet.timestamp) == frame_arrival_times_.end()) {
            frame_arrival_times_[packet.timestamp] = arrival;
        }

        buffered_packets_++;

        jb_trace("JB: pushed packet seq=%u ts=%u pt=%u, buffered=%zu",
                 packet.seq, packet.timestamp, packet.payload_type,
                 buffered_packets_);
    }

    std::optional<Frame> pop(core::TimePoint now) {
        // Calculate adaptive delay if enabled
        if (config_.adaptive && jitter_estimate_ > 0) {
            double jitter_ms = jitter_estimate_;
            core::Milliseconds adaptive_target{
                static_cast<std::int64_t>(jitter_ms * 4.0 + 20.0)
            };

            if (adaptive_target < config_.min_delay) {
                adaptive_target = config_.min_delay;
            } else if (adaptive_target > config_.max_delay) {
                adaptive_target = config_.max_delay;
            }

            target_delay_ = adaptive_target;
        }

        // Find next frame ready to play
        for (auto it = frame_buffer_.begin(); it != frame_buffer_.end(); ) {
            auto ts = it->first;
            auto arrival_it = frame_arrival_times_.find(ts);

            if (arrival_it == frame_arrival_times_.end()) {
                ++it;
                continue;
            }

            core::TimePoint playout_time = arrival_it->second + target_delay_;

            if (now >= playout_time) {
                Frame frame = std::move(it->second);
                frame_arrival_times_.erase(arrival_it);
                frame_buffer_.erase(it++);

                buffered_packets_ -= frame.packets.size();
                frames_emitted_++;

                jb_trace("JB: emitting frame ts=%u packets=%zu",
                         frame.frame_timestamp, frame.packets.size());

                return frame;
            } else {
                ++it;
            }
        }

        return std::nullopt;
    }

    std::vector<Frame> flush() {
        std::vector<Frame> frames;

        for (auto& [ts, frame] : frame_buffer_) {
            frames.push_back(std::move(frame));
        }

        frame_buffer_.clear();
        frame_arrival_times_.clear();
        buffered_packets_ = 0;

        jb_debug("JB: flushed %zu frames", frames.size());

        return frames;
    }

    void reset() noexcept {
        frame_buffer_.clear();
        frame_arrival_times_.clear();
        last_seq_ = std::nullopt;
        packets_received_ = 0;
        packets_dropped_ = 0;
        frames_emitted_ = 0;
        jitter_estimate_ = 0.0;
        buffered_packets_ = 0;
        target_delay_ = {};
    }

    Stats stats() const noexcept {
        return Stats{
            .buffered_packets = buffered_packets_,
            .frames_emitted   = frames_emitted_,
            .packets_dropped  = packets_dropped_,
            .current_delay    = target_delay_,
            .current_jitter   = core::Milliseconds{
                static_cast<std::int64_t>(jitter_estimate_)
            },
        };
    }

    void update_config(Config config) {
        config_ = config;
        if (!config_.adaptive) {
            target_delay_ = config_.initial_delay;
        }
    }

    Config config() const noexcept { return config_; }
};

// -----------------------------------------------------------------------------
// JitterBuffer public interface
// -----------------------------------------------------------------------------

JitterBuffer::JitterBuffer()
    : impl_(std::make_unique<Impl>(Config{})) {}

JitterBuffer::JitterBuffer(Config config)
    : impl_(std::make_unique<Impl>(config)) {}

JitterBuffer::~JitterBuffer() = default;

JitterBuffer::JitterBuffer(JitterBuffer&&) noexcept = default;
JitterBuffer& JitterBuffer::operator=(JitterBuffer&&) noexcept = default;

void JitterBuffer::push(rtp::PacketView packet,
                        core::TimePoint arrival,
                        std::optional<std::uint64_t> ref_frame) {
    impl_->push(packet, arrival, ref_frame);
}

std::optional<Frame> JitterBuffer::pop(core::TimePoint now) {
    return impl_->pop(now);
}

std::vector<Frame> JitterBuffer::flush() {
    return impl_->flush();
}

void JitterBuffer::reset() noexcept {
    impl_->reset();
}

Stats JitterBuffer::stats() const noexcept {
    return impl_->stats();
}

void JitterBuffer::update_config(Config config) {
    impl_->update_config(config);
}

Config JitterBuffer::config() const noexcept {
    return impl_->config();
}

} // namespace nimrtc::jb
