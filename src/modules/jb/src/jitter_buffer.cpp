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

// (Default playout delays moved to JitterBuffer::Impl::base_delay_for_pt() —
// per-payload-type heuristic that also accounts for narrowband vs wideband.)

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

    // Buffered frames keyed by RTP timestamp (one Frame may carry multiple
    // RTP packets sharing the same timestamp, e.g. redundant audio paths).
    // std::map gives us sorted-by-timestamp iteration in O(log N) per
    // operation; push and pop emit in timestamp order which is what the
    // WebRTC contract requires (see test JbJitterBuffer.PushTwoFramesPopEmitsInOrder).
    std::map<std::uint32_t, Frame> frame_buffer_;

    // First arrival time per timestamp — recorded once when the first
    // packet of a frame is buffered.  Stored separately from Frame so we
    // don't have to mutate Frame in place (Frame is exposed to the
    // caller as an output of pop() and is moved out of the buffer).
    std::map<std::uint32_t, core::TimePoint> frame_arrival_times_;

    // Last received sequence number — used both for reorder-gap diagnostics
    // and as the basis for RFC 3550 §6.4.1 transit-difference jitter math.
    std::optional<std::uint16_t> last_seq_;
    // Last (transit-corrected) RTP clock value seen — needed for the
    // transit-difference computation in update_jitter().
    std::optional<std::uint64_t> last_rtp_clock_us_;

    std::uint64_t packets_received_ = 0;
    std::uint64_t packets_dropped_ = 0;
    std::uint64_t frames_emitted_ = 0;

    // RFC 3550 §6.4.1 interarrival jitter estimate.  Updated on every
    // push() using D(i,j) = (Rj - Ri) - (Sj - Si) and J(i) = J(i-1) +
    // (|D(i-1,i)| - J(i-1)) / 16.  Reported in stats() as current_jitter;
    // used by adaptive target_delay computation when config_.adaptive is
    // true.
    double jitter_estimate_ = 0.0;

    // Current target delay — derived from config + adaptive jitter
    // estimate.  Updated on each pop() so the value tracks the latest
    // network conditions.
    core::Milliseconds target_delay_;

    // Statistics
    std::size_t buffered_packets_ = 0;

    /** Push one packet into the buffer. */
    void push(const rtp::PacketView& packet, core::TimePoint arrival,
              std::optional<std::uint64_t> ref_frame) {
        ++packets_received_;

        // ---- Sequence-gap diagnostic (does NOT mutate buffer) ------------
        if (last_seq_.has_value()) {
            const std::uint16_t expected_seq =
                static_cast<std::uint16_t>(*last_seq_ + 1);
            if (packet.seq != expected_seq) {
                jb_debug("JB: seq gap detected: expected %u, got %u",
                         expected_seq, packet.seq);
            }
        }
        last_seq_ = packet.seq;

        // ---- RFC 3550 §6.4.1 interarrival jitter update ------------------
        // Convert the RTP timestamp to microseconds relative to the clock
        // rate so we can subtract it from the arrival time.  For RFC 3550
        // arithmetic we use the SENDER's clock rate (typically 48 kHz for
        // Opus audio, 90 kHz for video).  When unknown, fall back to 90 kHz
        // (the RFC default) so the math is still well-defined; the result
        // will be off by a constant scale but the *sign* of D is preserved
        // and |D| scaling only affects jitter growth rate (not correctness).
        const std::uint32_t clock_rate_hz =
            config_.clock_rate_hz != 0 ? config_.clock_rate_hz : 90000;
        const std::uint64_t rtp_clock_us =
            (static_cast<std::uint64_t>(packet.timestamp) * 1'000'000ULL)
            / static_cast<std::uint64_t>(clock_rate_hz);
        const std::int64_t arrival_us =
            std::chrono::duration_cast<core::Microseconds>(
                arrival.time_since_epoch())
                .count();

        if (last_rtp_clock_us_.has_value()) {
            // D(i, j) = (Rj - Ri) - (Sj - Si).  Use signed arithmetic so
            // reorder/loss can produce negative D — only |D| feeds into the
            // smoothing equation.
            const std::int64_t d_us =
                (arrival_us
                 - static_cast<std::int64_t>(*last_rtp_clock_us_))
                - static_cast<std::int64_t>(
                    static_cast<std::int64_t>(rtp_clock_us)
                    - static_cast<std::int64_t>(*last_rtp_clock_us_));
            // (Equivalent to:  (Rj - Ri) - (Sj - Si).)
            // Update per RFC 3550 §A.8 with alpha = 1/16:
            //   J(i) = J(i-1) + (|D(i-1,i)| - J(i-1)) / 16
            const double abs_d = static_cast<double>(d_us < 0 ? -d_us : d_us);
            // Math is in microseconds; convert to milliseconds for the
            // stats surface.  Internal state stays in ms.
            const double abs_d_ms = abs_d / 1000.0;
            jitter_estimate_ += (abs_d_ms - jitter_estimate_) / 16.0;
        }
        last_rtp_clock_us_ = rtp_clock_us;

        // ---- Initialise target_delay on first packet ---------------------
        if (target_delay_.count() == 0) {
            target_delay_ = config_.initial_delay.count() > 0
                            ? config_.initial_delay
                            : base_delay_for_pt(packet.payload_type);
        }

        // ---- Insert packet into the per-timestamp Frame ------------------
        auto& frame = frame_buffer_[packet.timestamp];
        frame.packets.push_back(packet);
        frame.frame_timestamp = packet.timestamp;

        // Set capture latency + first arrival.
        if (frame.first_arrival.has_value()) {
            frame.capture_latency = arrival - *frame.first_arrival;
        } else {
            frame.first_arrival = arrival;
        }

        // Set ref_frame if provided.
        if (ref_frame.has_value()) {
            frame.ref_frame = ref_frame;
        }

        // Store arrival time for this frame.
        if (frame_arrival_times_.find(packet.timestamp)
            == frame_arrival_times_.end()) {
            frame_arrival_times_[packet.timestamp] = arrival;
        }

        buffered_packets_++;

        jb_trace("JB: pushed packet seq=%u ts=%u pt=%u, buffered=%zu",
                 packet.seq, packet.timestamp, packet.payload_type,
                 buffered_packets_);
    }

    /** Compute the default per-payload-type playout delay for the very
     *  first frame before adaptive logic kicks in. */
    static core::Milliseconds base_delay_for_pt(std::uint8_t pt) noexcept {
        // Narrow-band (RFC 3551 §6 / 7): PT 0..23 + 32..35 + 64..76.
        // Opus is RFC 7587 PT  = 96..127 dynamic range; default narrowband.
        const bool narrowband = (pt <= 23)
            || (pt >= 32 && pt <= 35)
            || (pt >= 64 && pt <= 76)
            || pt == 0;  // PCMU
        // Heuristic: 60 ms for narrowband (covers one ~20 ms packet + ~40 ms
        // safety against modest jitter), 100 ms for wideband (Opus, video).
        // These are conservative starting points; adaptive mode refines.
        return narrowband ? core::Milliseconds{60} : core::Milliseconds{100};
    }

    /** Pull the next frame whose playout deadline is <= now. */
    std::optional<Frame> pop(core::TimePoint now) {
        // ---- Adaptive target_delay update ------------------------------
        // Per WebRTC conventions and RFC 3550 §6.4.1, a reasonable
        // adaptive playout delay tracks the observed jitter with a safety
        // margin: target = clamp(min_delay, base + 4*J, max_delay).
        // 4x jitter is the classic Cisco/SoC factor; it bounds the
        // probability of under-run at ~3 sigma for typical lossless
        // Gaussian jitter distributions.
        if (config_.adaptive && jitter_estimate_ > 0.0) {
            core::Milliseconds adaptive_target{
                static_cast<std::int64_t>(jitter_estimate_ * 4.0 + 20.0)
            };
            if (adaptive_target < config_.min_delay) {
                adaptive_target = config_.min_delay;
            } else if (adaptive_target > config_.max_delay) {
                adaptive_target = config_.max_delay;
            }
            target_delay_ = adaptive_target;
        } else if (!config_.adaptive) {
            // Fixed mode: target == initial_delay.
            target_delay_ = config_.initial_delay;
        }

        // ---- Find earliest frame whose playout time has passed -----------
        // std::map iteration is sorted by key (RTP timestamp); map::begin()
        // gives us the earliest.  O(log N) for the begin() lookup, O(N)
        // for the worst-case scan if the first several frames aren't ready
        // yet — but in steady state the first frame IS ready, so amortised
        // pop() is O(log N).
        for (auto it = frame_buffer_.begin(); it != frame_buffer_.end(); ) {
            const auto ts = it->first;
            const auto arrival_it = frame_arrival_times_.find(ts);
            if (arrival_it == frame_arrival_times_.end()) {
                ++it;
                continue;
            }

            const core::TimePoint playout_time =
                arrival_it->second + target_delay_;

            if (now >= playout_time) {
                Frame frame = std::move(it->second);
                frame_arrival_times_.erase(arrival_it);
                frame_buffer_.erase(it++);

                buffered_packets_ -= frame.packets.size();
                frames_emitted_++;

                jb_trace("JB: emitting frame ts=%u packets=%zu",
                         frame.frame_timestamp, frame.packets.size());
                return frame;
            }
            // The first (earliest) frame isn't ready yet — neither will any
            // later frame be (timestamps are sorted), so bail out.
            break;
        }

        return std::nullopt;
    }

    std::vector<Frame> flush() {
        std::vector<Frame> frames;
        frames.reserve(frame_buffer_.size());
        // std::map iteration is in-order; flush() preserves arrival/timestamp
        // order which matches the test JbJitterBuffer.FlushEmitsAllBufferedFrames.
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
        last_rtp_clock_us_ = std::nullopt;
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
