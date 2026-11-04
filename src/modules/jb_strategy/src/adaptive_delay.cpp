/**
 * @file src/modules/jb_strategy/src/adaptive_delay.cpp
 * @brief Shared adaptive jitter-buffer strategy (RFC 3550 §6.4.1).
 */

#include <nimrtc/jb_strategy/adaptive_delay.hpp>

#include <algorithm>
#include <cmath>

namespace nimrtc::jb_strategy {

namespace {

// RFC 3550 §6.4.1 smoothing factor: J += (|D| - J) / 16.
constexpr double kSmoothingDivisor = 16.0;

} // anonymous namespace

AdaptiveDelayTracker::AdaptiveDelayTracker() noexcept = default;

AdaptiveDelayTracker::AdaptiveDelayTracker(DelayPolicy policy) noexcept
    : policy_(policy) {}

void AdaptiveDelayTracker::observe(std::int64_t arrival_us,
                                    std::int64_t media_clock_us) noexcept {
    // The transit time is the difference between the wall/monotonic arrival
    // and the media clock reading of the same packet.  Two packets sent
    // back-to-back by the sender arrive with (nearly) the same transit, so
    // the *difference* of consecutive transits is the network's arrival
    // jitter — exactly the D(i-1,i) of RFC 3550 §6.4.1.
    const std::int64_t transit_us = arrival_us - media_clock_us;

    if (last_transit_us_.has_value()) {
        const std::int64_t d_us = transit_us - *last_transit_us_;
        const double abs_d_ms = static_cast<double>(d_us < 0 ? -d_us : d_us) / 1000.0;
        jitter_ms_ += (abs_d_ms - jitter_ms_) / kSmoothingDivisor;
    }
    last_transit_us_ = transit_us;
    ++observations_;
}

core::Milliseconds AdaptiveDelayTracker::jitter() const noexcept {
    return core::Milliseconds{static_cast<std::int64_t>(jitter_ms_)};
}

core::Milliseconds AdaptiveDelayTracker::target_delay() const noexcept {
    if (!policy_.adaptive) {
        return policy_.base_delay;
    }
    const double target =
        static_cast<double>(policy_.base_delay.count())
        + policy_.jitter_multiplier * jitter_ms_;

    auto clamped = static_cast<std::int64_t>(std::llround(target));
    if (clamped < policy_.min_delay.count()) {
        clamped = policy_.min_delay.count();
    } else if (clamped > policy_.max_delay.count()) {
        clamped = policy_.max_delay.count();
    }
    return core::Milliseconds{clamped};
}

std::uint64_t AdaptiveDelayTracker::observations() const noexcept {
    return observations_;
}

void AdaptiveDelayTracker::reset() noexcept {
    jitter_ms_ = 0.0;
    last_transit_us_.reset();
    observations_ = 0;
}

void AdaptiveDelayTracker::set_policy(DelayPolicy policy) noexcept {
    policy_ = policy;
}

DelayPolicy AdaptiveDelayTracker::policy() const noexcept {
    return policy_;
}

} // namespace nimrtc::jb_strategy
