/**
 * @file src/modules/bwe/src/bwe.cpp
 * @brief BWE — AIMD bandwidth estimator implementation (P1).
 *
 * ## Algorithm
 *
 * - **AIMD** (Additive Increase / Multiplicative Decrease):
 *   - No loss: increase bitrate by `increase_bps` per second
 *   - Loss event (RFC 8698 §5.1): one multiplicative decrease per loss event,
 *     triggered on the rising edge of the loss rate crossing the 2% threshold
 *     — *not* on every feedback with loss>0 (avoids geometric decay under
 *     persistent loss; P1#7).
 *   - Cooldown (500 ms) before resuming additive increase.
 *
 * - **REMB passthrough**: if receiver reports higher bandwidth, adopt it
 *   (after sanity check against min/max); ignored while a loss event is
 *   pending (receiver's estimate is stale during congestion).
 *
 * - **Smoothing**: exponential moving average to avoid oscillation.
 *
 * @note P1 — simple AIMD. Goog-CC mainline lands in P3.
 */

#include <nimrtc/bwe/bwe.hpp>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

#include <nimrtc/core/log.hpp>

namespace nimrtc::bwe {

namespace {

// Minimum fraction of REMB we must trust (avoid sudden jumps)
constexpr double kRembTrustFactor = 0.8;

// How much to increase per RTT when in increase phase
constexpr double kIncreasePerRtt = 0.05;  // 5% per RTT

constexpr auto kLog = core::log::Level::Debug;

// __attribute__((format(printf, ...))) tells GCC/Clang that `fmt` is a
// printf-style format string. Without it, vsnprintf(fmt, ...) below
// trips -Werror=format-nonliteral on Clang (the format string is a
// parameter, not a string literal). The attribute is harmless on
// MSVC (which silently ignores __attribute__).
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
inline void bwe_debug(const char* fmt, ...) {
    if (core::log::Logger::instance().level() <= kLog) {
        char buf[256];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        core::log::Logger::instance().debug(buf);
    }
}

} // anonymous namespace

// -----------------------------------------------------------------------------
// Bwe::Impl
// -----------------------------------------------------------------------------

struct Bwe::Impl {
    explicit Impl(plugins::BweConfig config)
        : config_(config)
        , current_bps_(config.initial_bitrate_bps)
        , smoothed_bps_(config.initial_bitrate_bps)
        , last_update_time_()
        , state_(State::kIncrease)  // start in increase mode
    {}

    plugins::BweConfig config_;
    std::uint32_t current_bps_;
    std::uint32_t smoothed_bps_;
    std::optional<core::TimePoint> last_update_time_;
    std::optional<core::TimePoint> last_change_time_;
    plugins::BweEstimate::Reason last_reason_ = plugins::BweEstimate::Reason::Initial;

    // Loss history (for trend detection)
    double last_loss_rate_ = 0.0;

    // Loss-event detection (RFC 8698 §5.1 — AIMD applies the multiplicative
    // decrease *once per loss event*, not once per feedback with loss>0).
    // A "loss event" is the rising-edge transition from no-loss (loss_rate
    // below the 2% threshold) to lossy (loss_rate above the threshold).
    // We latch `loss_event_pending_` on that rising edge and clear it once
    // the decrease has fired or the network returns to clean.
    //
    // Without this latch the original code applied the multiplicative
    // decrease on every RTCP feedback that reported loss>0; persistent
    // loss would drop the rate geometrically to zero.  With the latch the
    // decrease fires exactly once per observed congestion event and the
    // increase phase resumes after the cooldown.
    static constexpr double kLossEventThreshold = 0.02;
    bool loss_event_pending_ = false;

    enum class State {
        kIncrease,  // probing, no recent loss
        kDecrease,  // loss detected, backing off
    };
    State state_ = State::kIncrease;

    // Time we entered decrease state
    std::optional<core::TimePoint> decrease_start_;

    void on_feedback(plugins::BweFeedback feedback) {
        // ---- Loss event detection (P1#7 fix) --------------------------------
        // Edge-triggered: a "loss event" fires when the loss rate crosses the
        // threshold from below.  Subsequent feedback samples at the same or
        // higher loss rate do NOT retrigger the decrease — only one
        // multiplicative decrease per event (RFC 8698 §5.1).
        const bool was_no_loss = (last_loss_rate_ < kLossEventThreshold);
        const bool is_loss_now = (feedback.loss_rate >= kLossEventThreshold);
        if (was_no_loss && is_loss_now) {
            loss_event_pending_ = true;
        }
        // Clear the latch on a clean (sub-low) feedback so a future loss
        // event retriggers the decrease path.
        if (!is_loss_now && last_loss_rate_ >= kLossEventThreshold) {
            loss_event_pending_ = false;
        }

        // ---- REMB override (P1#7 fix: skip during loss events) ------------
        // REMB during an active loss event is suspect (the receiver's
        // estimate is also stale), so we ignore REMB while a decrease is
        // pending and let the loss-event path own the rate update.
        if (feedback.remb_bps.has_value() && !loss_event_pending_) {
            std::uint32_t remb = *feedback.remb_bps;
            // Apply trust factor and clamp
            remb = static_cast<std::uint32_t>(remb * kRembTrustFactor);
            if (remb > config_.max_bitrate_bps) remb = config_.max_bitrate_bps;
            if (remb < config_.min_bitrate_bps) remb = config_.min_bitrate_bps;

            // If REMB suggests we should go higher, adopt it.
            if (remb > smoothed_bps_) {
                current_bps_  = remb;
                last_reason_  = plugins::BweEstimate::Reason::REMBOverride;
                bwe_debug("BWE: REMB override: %u bps", remb);
            }
        }

        // ---- Apply multiplicative decrease on loss event -------------------
        if (loss_event_pending_) {
            // Compute new rate and clamp.
            std::uint32_t new_rate = static_cast<std::uint32_t>(
                current_bps_ * config_.decrease_factor);
            new_rate = std::max(new_rate, config_.min_bitrate_bps);
            new_rate = std::min(new_rate, config_.max_bitrate_bps);

            if (new_rate != current_bps_) {
                current_bps_    = new_rate;
                last_reason_    = plugins::BweEstimate::Reason::AIMDDecrease;
                decrease_start_ = feedback.timestamp;
                state_          = State::kDecrease;
                bwe_debug("BWE: loss event (rate %.1f%%, was %.1f%%), "
                         "decreased to %u bps",
                         feedback.loss_rate * 100.0,
                         last_loss_rate_ * 100.0, current_bps_);
            }
            // One decrease per loss event — clear the latch so a second
            // feedback at the same loss rate does not decrease again.
            loss_event_pending_ = false;
        } else {
            // ---- No loss event: cooldown check + additive increase ---------
            if (state_ == State::kDecrease) {
                // Was decreasing, check if cooldown passed.
                constexpr core::Milliseconds kDecreaseCooldown{500};
                if (decrease_start_.has_value() &&
                    feedback.timestamp - *decrease_start_ >= kDecreaseCooldown) {
                    state_ = State::kIncrease;
                    bwe_debug("BWE: cooldown complete, resuming increase");
                }
            }

            if (state_ == State::kIncrease) {
                // Additive increase: add increase_bps proportional to elapsed time.
                if (last_change_time_.has_value()) {
                    const auto elapsed = feedback.timestamp - *last_change_time_;
                    const auto elapsed_ms =
                        std::chrono::duration_cast<core::Milliseconds>(elapsed).count();

                    if (elapsed_ms >= config_.change_interval.count()) {
                        const double increase_amount =
                            (static_cast<double>(elapsed_ms) / 1000.0) * config_.increase_bps;

                        std::uint32_t new_rate = current_bps_ +
                            static_cast<std::uint32_t>(increase_amount);

                        // Also consider per-RTT increase (CC-style boost).
                        if (feedback.rtt.count() > 0) {
                            const double rtt_factor =
                                static_cast<double>(feedback.rtt.count()) / 1000.0;  // seconds
                            const double rtt_increase =
                                current_bps_ * kIncreasePerRtt * rtt_factor;
                            new_rate += static_cast<std::uint32_t>(rtt_increase);
                        }

                        new_rate = std::min(new_rate, config_.max_bitrate_bps);

                        if (new_rate > current_bps_) {
                            current_bps_    = new_rate;
                            last_reason_    = plugins::BweEstimate::Reason::AIMDIncrease;
                            last_change_time_ = feedback.timestamp;
                            bwe_debug("BWE: increased to %u bps", current_bps_);
                        }
                    }
                } else {
                    last_change_time_ = feedback.timestamp;
                }
            }
        }

        // ---- Smoothing ---------------------------------------------------
        smoothed_bps_ = static_cast<std::uint32_t>(
            config_.smoothing_alpha * current_bps_ +
            (1.0 - config_.smoothing_alpha) * smoothed_bps_);

        last_update_time_ = feedback.timestamp;
        last_loss_rate_   = feedback.loss_rate;
    }

    plugins::BweEstimate estimate() const {
        plugins::BweEstimate e;
        e.target_bitrate_bps = smoothed_bps_;
        // Pacing slightly higher for burst smoothing
        e.pacing_bitrate_bps = static_cast<std::uint32_t>(smoothed_bps_ * 1.1);
        if (e.pacing_bitrate_bps > config_.max_bitrate_bps) {
            e.pacing_bitrate_bps = config_.max_bitrate_bps;
        }
        e.timestamp = last_update_time_.value_or(core::SteadyClock::now());
        e.reason = last_reason_;
        return e;
    }

    void update_config(plugins::BweConfig config) {
        config_ = config;
        // Clamp current values to new limits
        current_bps_ = std::clamp(current_bps_,
                                  config_.min_bitrate_bps,
                                  config_.max_bitrate_bps);
        smoothed_bps_ = std::clamp(smoothed_bps_,
                                   config_.min_bitrate_bps,
                                   config_.max_bitrate_bps);
    }

    plugins::BweConfig config() const { return config_; }

    void reset() {
        current_bps_ = config_.initial_bitrate_bps;
        smoothed_bps_ = config_.initial_bitrate_bps;
        last_update_time_ = std::nullopt;
        last_change_time_ = std::nullopt;
        last_loss_rate_ = 0.0;
        loss_event_pending_ = false;
        state_ = State::kIncrease;
        decrease_start_ = std::nullopt;
        last_reason_ = plugins::BweEstimate::Reason::Initial;
    }
};

// -----------------------------------------------------------------------------
// Bwe public interface (plugins::IBwe)
// -----------------------------------------------------------------------------

Bwe::Bwe(plugins::BweConfig config)
    : impl_(std::make_unique<Impl>(config)) {}

Bwe::~Bwe() = default;

Bwe::Bwe(Bwe&&) noexcept = default;
Bwe& Bwe::operator=(Bwe&&) noexcept = default;

const char* Bwe::name() const noexcept {
    return "nimrtc::bwe::Bwe (AIMD bandwidth estimator)";
}

plugins::Status Bwe::open() noexcept {
    return plugins::kOk;
}

void Bwe::close() noexcept {
    // No-op — Bwe holds no external resources.
}

void Bwe::on_feedback(plugins::BweFeedback feedback) noexcept {
    impl_->on_feedback(feedback);
}

plugins::BweEstimate Bwe::estimate() const noexcept {
    return impl_->estimate();
}

void Bwe::update_config(plugins::BweConfig config) noexcept {
    impl_->update_config(config);
}

plugins::BweConfig Bwe::config() const noexcept {
    return impl_->config();
}

void Bwe::reset() noexcept {
    impl_->reset();
}

} // namespace nimrtc::bwe
