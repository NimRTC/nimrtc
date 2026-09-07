/**
 * @file src/modules/bwe/src/bwe.cpp
 * @brief BWE — AIMD bandwidth estimator implementation (P1).
 *
 * ## Algorithm
 *
 * - **AIMD** (Additive Increase / Multiplicative Decrease):
 *   - No loss: increase bitrate by `increase_bps` per second
 *   - Loss detected: decrease bitrate by `decrease_factor` (multiply)
 *
 * - **REMB passthrough**: if receiver reports higher bandwidth, adopt it
 *   (after sanity check against min/max)
 *
 * - **Smoothing**: exponential moving average to avoid oscillation
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

    enum class State {
        kIncrease,  // probing, no recent loss
        kDecrease,  // loss detected, backing off
        kHold,      // waiting for cooldown before increasing again
    };
    State state_ = State::kIncrease;

    // Time we entered decrease state
    std::optional<core::TimePoint> decrease_start_;

    void on_feedback(plugins::BweFeedback feedback) {
        // Clamp REMB if provided
        if (feedback.remb_bps.has_value()) {
            std::uint32_t remb = *feedback.remb_bps;
            // Apply trust factor and clamp
            remb = static_cast<std::uint32_t>(remb * kRembTrustFactor);
            if (remb > config_.max_bitrate_bps) remb = config_.max_bitrate_bps;
            if (remb < config_.min_bitrate_bps) remb = config_.min_bitrate_bps;

            // If REMB suggests we should go higher, consider it
            if (remb > smoothed_bps_) {
                // Use REMB as target if it's higher than our current estimate
                current_bps_ = remb;
                last_reason_ = plugins::BweEstimate::Reason::REMBOverride;
                bwe_debug("BWE: REMB override: %u bps", remb);
            }
        }

        // Update based on loss
        if (feedback.loss_rate > 0.0) {
            // Loss detected: multiplicative decrease
            std::uint32_t new_rate = static_cast<std::uint32_t>(
                current_bps_ * config_.decrease_factor);

            // Clamp
            new_rate = std::max(new_rate, config_.min_bitrate_bps);
            new_rate = std::min(new_rate, config_.max_bitrate_bps);

            if (new_rate != current_bps_) {
                current_bps_ = new_rate;
                last_reason_ = plugins::BweEstimate::Reason::AIMDDecrease;
                decrease_start_ = feedback.timestamp;
                state_ = State::kDecrease;
                bwe_debug("BWE: loss %.1f%%, decreased to %u bps",
                         feedback.loss_rate * 100.0, current_bps_);
            }
        } else {
            // No loss: increase phase
            if (state_ == State::kDecrease) {
                // Was decreasing, check if cooldown passed
                auto cooldown = core::Milliseconds{500};
                if (decrease_start_.has_value() &&
                    feedback.timestamp - *decrease_start_ >= cooldown) {
                    state_ = State::kIncrease;
                    bwe_debug("BWE: cooldown complete, resuming increase");
                }
            }

            if (state_ == State::kIncrease || state_ == State::kHold) {
                // Additive increase: add increase_bps proportional to elapsed time
                if (last_change_time_.has_value()) {
                    auto elapsed = feedback.timestamp - *last_change_time_;
                    auto elapsed_ms = std::chrono::duration_cast<core::Milliseconds>(elapsed).count();

                    // Ensure minimum interval between changes
                    if (elapsed_ms >= config_.change_interval.count()) {
                        // Calculate increase amount
                        double increase_amount = (elapsed_ms / 1000.0) *
                                                 config_.increase_bps;

                        std::uint32_t new_rate = current_bps_ +
                                                 static_cast<std::uint32_t>(increase_amount);

                        // Also consider per-RTT increase
                        if (feedback.rtt.count() > 0) {
                            double rtt_factor = feedback.rtt.count() / 1000.0;  // convert to seconds
                            double rtt_increase = current_bps_ * kIncreasePerRtt * rtt_factor;
                            new_rate = static_cast<std::uint32_t>(
                                new_rate + static_cast<std::uint32_t>(rtt_increase));
                        }

                        // Clamp
                        new_rate = std::min(new_rate, config_.max_bitrate_bps);

                        if (new_rate > current_bps_) {
                            current_bps_ = new_rate;
                            last_reason_ = plugins::BweEstimate::Reason::AIMDIncrease;
                            last_change_time_ = feedback.timestamp;
                            state_ = State::kIncrease;
                            bwe_debug("BWE: increased to %u bps", current_bps_);
                        }
                    }
                } else {
                    last_change_time_ = feedback.timestamp;
                }
            }
        }

        // Apply smoothing
        smoothed_bps_ = static_cast<std::uint32_t>(
            config_.smoothing_alpha * current_bps_ +
            (1.0 - config_.smoothing_alpha) * smoothed_bps_);

        last_update_time_ = feedback.timestamp;
        last_loss_rate_ = feedback.loss_rate;
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
