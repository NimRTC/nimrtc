/**
 * @file src/modules/bwe/src/goog_cc_bwe.cpp
 * @brief GoogCcBwe — delay + loss hybrid bandwidth estimator, opt-in (v1.1 Q1).
 *
 * Simplified Goog-CC mainline (NOT a full port of the WebRTC
 * delay-based controller):
 *
 *   - Delay signal: RTT exceeding the observed minimum by more than
 *     25 ms (or 20 %, whichever is larger) is treated as overuse and
 *     drives one multiplicative back-off (0.85x) per feedback.
 *   - Loss signal: edge-triggered loss event (>= 2 % after clean),
 *     one multiplicative decrease per event using
 *     `config_.decrease_factor` — same rule as the AIMD front-end.
 *   - Clean signal: after the 500 ms post-decrease cooldown and the
 *     configured `change_interval`, probe upwards by
 *     max(additive `increase_bps` share, 5 % of current per second).
 *   - REMB override, [min, max] clamping and EMA smoothing follow the
 *     AIMD front-end so scheduler behaviour stays comparable.
 *   - transport-cc delay signal: when transport-cc drives the estimate,
 *     the arrival series replaces the RTT gate (ADR-018 Decision 4).  The
 *     RTT of a Receiver Report is a windowed value that lags the
 *     per-packet arrival series, and using both would count the same
 *     congestion twice.
 *
 * The engine default stays `"aimd"`; select this backend explicitly via
 * `EngineConfig::bwe_name = "goog-cc"`.
 */

#include <nimrtc/bwe/bwe.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

#include "arrival_trend.hpp"

namespace nimrtc::bwe {

namespace {

// Post-decrease cooldown before probing upwards again.
constexpr core::Milliseconds kDecreaseCooldown{500};

// Delay-overuse gate: RTT must exceed the observed minimum by this
// absolute margin AND this relative margin before we back off.
constexpr std::int64_t kOveruseAbsoluteMs = 25;
constexpr double kOveruseRelativeRatio = 1.2;

// Per-feedback multiplicative back-off on delay overuse.
constexpr double kDelayBackoffFactor = 0.85;

// Clean-path multiplicative probe rate (per second).
constexpr double kProbePerSecond = 0.05;

// Trust fraction for REMB adoption (same as the AIMD front-end).
constexpr double kRembTrustFactor = 0.8;

}  // namespace

struct GoogCcBwe::Impl {
    explicit Impl(plugins::BweConfig config)
        : config_(config),
          current_bps_(config.initial_bitrate_bps),
          smoothed_bps_(config.initial_bitrate_bps) {}

    plugins::BweConfig config_;
    std::uint32_t current_bps_;
    std::uint32_t smoothed_bps_;
    std::optional<core::TimePoint> last_update_time_;
    std::optional<core::TimePoint> last_change_time_;
    std::optional<core::TimePoint> decrease_start_;
    plugins::BweEstimate::Reason last_reason_ =
        plugins::BweEstimate::Reason::Initial;

    // Minimum observed RTT (delay baseline). Monotonic minimum over the
    // stream lifetime; cleared on reset().
    std::optional<core::Milliseconds> min_rtt_;

    double last_loss_rate_ = 0.0;
    bool loss_event_pending_ = false;
    static constexpr double kLossEventThreshold = 0.02;

    // The transport-cc delay trend (ADR-018 Decision 4).
    detail::ArrivalTrend arrival_trend_;

    bool in_cooldown(core::TimePoint now) const {
        return decrease_start_.has_value() &&
               (now - *decrease_start_ < kDecreaseCooldown);
    }

    bool is_overuse(core::Milliseconds rtt) const {
        if (!min_rtt_.has_value() || rtt.count() <= 0) {
            return false;
        }
        const auto baseline = min_rtt_->count();
        const std::int64_t absolute_over = rtt.count() - baseline;
        const double relative =
            static_cast<double>(rtt.count()) /
            static_cast<double>(baseline > 0 ? baseline : 1);
        return absolute_over > kOveruseAbsoluteMs &&
               relative > kOveruseRelativeRatio;
    }

    void apply_decrease(std::uint32_t next,
                        core::TimePoint now,
                        plugins::BweEstimate::Reason reason) {
        next = std::clamp(next, config_.min_bitrate_bps,
                          config_.max_bitrate_bps);
        if (next != current_bps_) {
            current_bps_ = next;
            last_reason_ = reason;
            decrease_start_ = now;
            last_change_time_ = now;
            // The send spacing follows the target, so the arrival-trend
            // baseline we just compared against describes a different
            // operating point now.
            arrival_trend_.invalidate();
        }
    }

    void on_feedback(plugins::BweFeedback feedback) {
        // Track the delay baseline first (minimum observed RTT).
        if (feedback.rtt.count() > 0) {
            if (!min_rtt_.has_value() ||
                feedback.rtt.count() < min_rtt_->count()) {
                min_rtt_ = feedback.rtt;
            }
        }

        // Edge-triggered loss event (same rule as the AIMD front-end).
        const bool was_clean = (last_loss_rate_ < kLossEventThreshold);
        const bool is_lossy = (feedback.loss_rate >= kLossEventThreshold);
        if (was_clean && is_lossy) {
            loss_event_pending_ = true;
        }
        if (!is_lossy && last_loss_rate_ >= kLossEventThreshold) {
            loss_event_pending_ = false;
        }

        // ---- The delay signal ---------------------------------------------
        //
        // Two paths, one signal.  The loss/RTT path uses the RTT gate
        // (the RTT against the smallest RTT seen).  The transport-cc
        // path uses the arrival-series trend instead: the RTT of a
        // Receiver Report is a windowed value that lags the per-packet
        // arrival series, and using both would count the same congestion
        // twice.
        bool overuse = false;
        bool delay_overuse = false;
        if (feedback.signal_source == plugins::NetFeedbackSource::kTransportCc) {
            const detail::ArrivalTrend::Sample sample =
                arrival_trend_.update(feedback.arrival_deltas_ms);
            delay_overuse = sample.overuse;
            // A series that carried too few packets says nothing about
            // the delay; the RTT gate is the fallback for it.
            overuse = sample.valid ? sample.overuse
                                    : is_overuse(feedback.rtt);
        } else {
            // The arrival series is not continuous any more: the baseline
            // describes an operating point we stopped watching.
            arrival_trend_.invalidate();
            overuse = is_overuse(feedback.rtt);
        }

        // REMB override (ignored while a loss decrease is pending, and
        // while the delay trend says the path is congested: the
        // receiver's opinion is stale in both cases).
        if (feedback.remb_bps.has_value() && !loss_event_pending_ &&
            !delay_overuse) {
            std::uint32_t remb = static_cast<std::uint32_t>(
                static_cast<double>(*feedback.remb_bps) * kRembTrustFactor);
            remb = std::clamp(remb, config_.min_bitrate_bps,
                              config_.max_bitrate_bps);
            if (remb > smoothed_bps_) {
                current_bps_ = remb;
                last_reason_ = plugins::BweEstimate::Reason::REMBOverride;
                last_change_time_ = feedback.timestamp;
                // The target jumped: the send spacing follows it, so the
                // arrival-trend baseline describes a different operating
                // point now.
                arrival_trend_.invalidate();
            }
        }

        if (loss_event_pending_) {
            apply_decrease(
                static_cast<std::uint32_t>(static_cast<double>(current_bps_) *
                                           config_.decrease_factor),
                feedback.timestamp,
                plugins::BweEstimate::Reason::AIMDDecrease);
            loss_event_pending_ = false;
        } else if (overuse) {
            // Delay back-off fires even inside the post-loss cooldown:
            // a rising delay while we are already backing off is fresh
            // congestion information, not a duplicate of the loss event.
            apply_decrease(
                static_cast<std::uint32_t>(static_cast<double>(current_bps_) *
                                           kDelayBackoffFactor),
                feedback.timestamp,
                delay_overuse ? plugins::BweEstimate::Reason::TransportCcDelay
                               : plugins::BweEstimate::Reason::RttIncrease);
        } else if (!is_lossy && !in_cooldown(feedback.timestamp)) {
            // Clean probe: multiplicative 5 %/s with the additive
            // `increase_bps` share as the floor.
            if (last_change_time_.has_value()) {
                const auto elapsed = feedback.timestamp - *last_change_time_;
                const auto elapsed_ms =
                    std::chrono::duration_cast<core::Milliseconds>(elapsed)
                        .count();
                if (elapsed_ms >= config_.change_interval.count()) {
                    const double elapsed_s =
                        static_cast<double>(elapsed_ms) / 1000.0;
                    const double additive =
                        static_cast<double>(config_.increase_bps) * elapsed_s;
                    const double multiplicative =
                        static_cast<double>(current_bps_) * kProbePerSecond *
                        elapsed_s;
                    std::uint32_t next =
                        current_bps_ +
                        static_cast<std::uint32_t>(
                            (std::max)(additive, multiplicative));
                    next = std::min(next, config_.max_bitrate_bps);
                    if (next > current_bps_) {
                        current_bps_ = next;
                        last_reason_ =
                            plugins::BweEstimate::Reason::AIMDIncrease;
                        last_change_time_ = feedback.timestamp;
                    }
                }
            } else {
                last_change_time_ = feedback.timestamp;
            }
        }

        smoothed_bps_ = static_cast<std::uint32_t>(
            config_.smoothing_alpha * static_cast<double>(current_bps_) +
            (1.0 - config_.smoothing_alpha) *
                static_cast<double>(smoothed_bps_));

        last_update_time_ = feedback.timestamp;
        last_loss_rate_ = feedback.loss_rate;
    }

    plugins::BweEstimate estimate() const {
        plugins::BweEstimate e;
        e.target_bitrate_bps = smoothed_bps_;
        e.pacing_bitrate_bps =
            static_cast<std::uint32_t>(static_cast<double>(smoothed_bps_) *
                                       1.1);
        if (e.pacing_bitrate_bps > config_.max_bitrate_bps) {
            e.pacing_bitrate_bps = config_.max_bitrate_bps;
        }
        e.timestamp = last_update_time_.value_or(core::SteadyClock::now());
        e.reason = last_reason_;
        return e;
    }

    void update_config(plugins::BweConfig config) {
        config_ = config;
        current_bps_ = std::clamp(current_bps_, config_.min_bitrate_bps,
                                  config_.max_bitrate_bps);
        smoothed_bps_ = std::clamp(smoothed_bps_, config_.min_bitrate_bps,
                                   config_.max_bitrate_bps);
    }

    plugins::BweConfig config() const { return config_; }

    void reset() {
        current_bps_ = config_.initial_bitrate_bps;
        smoothed_bps_ = config_.initial_bitrate_bps;
        last_update_time_ = std::nullopt;
        last_change_time_ = std::nullopt;
        decrease_start_ = std::nullopt;
        last_reason_ = plugins::BweEstimate::Reason::Initial;
        min_rtt_ = std::nullopt;
        last_loss_rate_ = 0.0;
        loss_event_pending_ = false;
        arrival_trend_.reset();
    }
};

GoogCcBwe::GoogCcBwe(plugins::BweConfig config)
    : impl_(std::make_unique<Impl>(config)) {}

GoogCcBwe::~GoogCcBwe() = default;

GoogCcBwe::GoogCcBwe(GoogCcBwe&&) noexcept = default;
GoogCcBwe& GoogCcBwe::operator=(GoogCcBwe&&) noexcept = default;

const char* GoogCcBwe::name() const noexcept {
    return "nimrtc::bwe::GoogCcBwe (delay+loss hybrid, opt-in)";
}

plugins::Status GoogCcBwe::open() noexcept { return plugins::kOk; }

void GoogCcBwe::close() noexcept {}

void GoogCcBwe::on_feedback(plugins::BweFeedback feedback) noexcept {
    impl_->on_feedback(feedback);
}

plugins::BweEstimate GoogCcBwe::estimate() const noexcept {
    return impl_->estimate();
}

void GoogCcBwe::update_config(plugins::BweConfig config) noexcept {
    impl_->update_config(config);
}

plugins::BweConfig GoogCcBwe::config() const noexcept {
    return impl_->config();
}

void GoogCcBwe::reset() noexcept { impl_->reset(); }

}  // namespace nimrtc::bwe
