/**
 * @file src/modules/pacing/src/token_bucket.cpp
 * @brief The token-bucket implementation (v1.2.0 M3, video plan P1).
 *
 * The bucket is a pure value object: it never reads a clock, it never
 * allocates and it never touches global state.  Everything it needs
 * arrives through its arguments, which is what makes it deterministic
 * under test.
 *
 * ## The arithmetic
 *
 * The credit is kept in **milli-bytes** so that a bucket with a rate
 * that is not a multiple of 8000 bits/s does not lose the fractional
 * remainder of every packet:
 *
 *     credit_milli += (bytes_per_second * elapsed_us) / 1000
 *
 * `bytes_per_second * elapsed_us` is the number of *milli-bytes* that
 * elapsed time is worth, so the division by 1000 converts the
 * bytes-per-second into the milli-byte scale.  A packet of `n` bytes
 * costs `n * 1000` milli-bytes.
 *
 * ## Overflow
 *
 * The product `bytes_per_second * elapsed_us` must not overflow a
 * 64-bit integer.  The elapsed time is therefore clamped to
 * `kMaxElapsedUs` (10 s) before the multiplication: the credit is capped
 * at the burst ceiling anyway, so a longer idle gap cannot change the
 * outcome — it can only save the multiplication from overflowing.
 * With the clamp the largest possible product is
 * (2^32 / 8) * 10^7 < 2^63, which is comfortably inside uint64.
 */

#include <nimrtc/pacing/pacing.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>

namespace nimrtc::pacing {

namespace {

/** The longest elapsed interval the refill arithmetic accepts.
 *
 *  Ten seconds is far longer than any drain cadence, and the credit is
 *  clamped to the burst ceiling regardless — the clamp only exists to
 *  keep the multiplication inside 64 bits. */
constexpr std::uint64_t kMaxElapsedUs = 10'000'000;

/** The default burst, expressed as a fraction of the rate.
 *
 *  100 ms of traffic: long enough to absorb a scheduling jitter, short
 *  enough that a backlog does not escape in one go. */
constexpr std::uint64_t kDefaultBurstNumerator = 1;   // of 10
constexpr std::uint64_t kDefaultBurstDenominator = 10;

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / configuration
// ---------------------------------------------------------------------------

TokenBucket::TokenBucket(PacerConfig cfg) noexcept {
    set_config(cfg);
}

void TokenBucket::set_config(const PacerConfig& cfg) noexcept {
    rate_bps_       = cfg.rate_bps;
    burst_bytes_cfg_ = cfg.burst_bytes;

    // The credit earned under the old configuration is kept, but it may
    // not exceed the new burst ceiling: a rate change must not hand out
    // a free burst.
    const std::uint64_t cap_milli =
        static_cast<std::uint64_t>(effective_burst_bytes()) * 1000u;
    if (credit_milli_bytes_ > cap_milli) {
        credit_milli_bytes_ = cap_milli;
    }
}

PacerConfig TokenBucket::config() const noexcept {
    PacerConfig cfg;
    cfg.rate_bps    = rate_bps_;
    cfg.burst_bytes = burst_bytes_cfg_;
    return cfg;
}

void TokenBucket::set_rate_bps(std::uint32_t rate_bps) noexcept {
    rate_bps_ = rate_bps;

    const std::uint64_t cap_milli =
        static_cast<std::uint64_t>(effective_burst_bytes()) * 1000u;
    if (credit_milli_bytes_ > cap_milli) {
        credit_milli_bytes_ = cap_milli;
    }
}

std::uint32_t TokenBucket::rate_bps() const noexcept {
    return rate_bps_;
}

std::uint32_t TokenBucket::burst_bytes() const noexcept {
    return effective_burst_bytes();
}

// ---------------------------------------------------------------------------
// The bucket arithmetic
// ---------------------------------------------------------------------------

std::uint64_t TokenBucket::rate_bytes_per_sec() const noexcept {
    return static_cast<std::uint64_t>(rate_bps_) / 8u;
}

std::uint32_t TokenBucket::effective_burst_bytes() const noexcept {
    if (burst_bytes_cfg_ != 0) return burst_bytes_cfg_;

    // No rate means no pacing, so there is no burst to speak of.
    if (rate_bps_ == 0) return 0;

    const std::uint64_t bytes_per_sec = rate_bytes_per_sec();
    const std::uint64_t burst =
        (bytes_per_sec * kDefaultBurstNumerator) / kDefaultBurstDenominator;
    // A rate below 80 bits/s would round down to zero bytes of burst,
    // which would stall the stream completely: the smallest useful
    // bucket holds one byte.
    return static_cast<std::uint32_t>(std::max<std::uint64_t>(burst, 1u));
}

std::uint64_t TokenBucket::refill(core::TimePoint now) noexcept {
    // An unlimited bucket has no credit to track: everything is allowed.
    if (rate_bps_ == 0) {
        last_time_     = now;
        has_last_time_ = true;
        return std::numeric_limits<std::uint64_t>::max();
    }

    const std::uint64_t cap_milli =
        static_cast<std::uint64_t>(effective_burst_bytes()) * 1000u;

    if (!has_last_time_) {
        // A fresh bucket starts **full**: the classic token bucket hands
        // out its whole capacity once and then settles into the rate.
        // Without this the very first packet of a stream would wait for
        // a full tick interval before any credit had accumulated.
        last_time_          = now;
        has_last_time_      = true;
        credit_milli_bytes_ = cap_milli;
        return credit_milli_bytes_ / 1000u;
    }

    std::uint64_t elapsed_us = 0;
    {
        const auto delta = now - last_time_;
        // A time point that is not newer than the last one grants no
        // credit: the bucket never runs backwards, whatever the clock
        // does.  The reference point is only moved *forward*, so a
        // glitching clock cannot buy extra credit later on.
        if (delta.count() > 0) {
            elapsed_us = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(delta).count());
            last_time_ = now;
        }
    }
    has_last_time_ = true;

    if (elapsed_us > kMaxElapsedUs) elapsed_us = kMaxElapsedUs;

    const std::uint64_t bytes_per_sec = rate_bytes_per_sec();
    if (elapsed_us != 0 && bytes_per_sec != 0) {
        // milli-bytes earned = bytes/s * us / 1000
        credit_milli_bytes_ += (bytes_per_sec * elapsed_us) / 1000u;
    }

    if (credit_milli_bytes_ > cap_milli) {
        credit_milli_bytes_ = cap_milli;
    }

    return credit_milli_bytes_ / 1000u;
}

std::uint64_t TokenBucket::available(core::TimePoint now) noexcept {
    return refill(now);
}

bool TokenBucket::try_consume(std::uint64_t bytes, core::TimePoint now) noexcept {
    // An unlimited bucket always allows the bytes.
    if (rate_bps_ == 0) {
        bytes_sent_ += bytes;
        return true;
    }

    // Taking nothing is always allowed.
    if (bytes == 0) return true;

    refill(now);

    // A request this large cannot be satisfied by any bucket; refuse it
    // rather than overflow the cost computation.
    constexpr std::uint64_t kMaxAffordable =
        std::numeric_limits<std::uint64_t>::max() / 1000u;
    if (bytes > kMaxAffordable) {
        ++bytes_blocked_;
        return false;
    }

    const std::uint64_t cost_milli = bytes * 1000u;
    if (credit_milli_bytes_ < cost_milli) {
        ++bytes_blocked_;
        return false;
    }

    credit_milli_bytes_ -= cost_milli;
    bytes_sent_ += bytes;
    return true;
}

void TokenBucket::refund(std::uint64_t bytes) noexcept {
    if (bytes == 0) return;

    // A bucket that is not paced never charged for the bytes in the first
    // place (`try_consume` short-circuits on rate_bps_ == 0 and only
    // bumps the telemetry), so there is nothing to give back.
    if (rate_bps_ == 0) return;

    constexpr std::uint64_t kMaxAffordable =
        std::numeric_limits<std::uint64_t>::max() / 1000u;
    if (bytes > kMaxAffordable) return;

    credit_milli_bytes_ += bytes * 1000u;

    // The credit may not exceed the burst ceiling: refunding must not
    // manufacture a burst the bucket could never have held.
    const std::uint64_t cap_milli =
        static_cast<std::uint64_t>(effective_burst_bytes()) * 1000u;
    if (credit_milli_bytes_ > cap_milli) {
        credit_milli_bytes_ = cap_milli;
    }

    // The telemetry must reflect that the bytes did not go out.
    bytes_sent_ = (bytes_sent_ > bytes) ? (bytes_sent_ - bytes) : 0;
}

void TokenBucket::reset(core::TimePoint now) noexcept {    credit_milli_bytes_ = 0;
    last_time_          = now;
    has_last_time_      = true;
    bytes_sent_         = 0;
    bytes_blocked_      = 0;
}

std::uint64_t TokenBucket::bytes_sent() const noexcept {
    return bytes_sent_;
}

std::uint64_t TokenBucket::bytes_blocked() const noexcept {
    return bytes_blocked_;
}

}  // namespace nimrtc::pacing
