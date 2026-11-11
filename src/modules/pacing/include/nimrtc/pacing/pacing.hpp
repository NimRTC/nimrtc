/**
 * @file nimrtc/pacing/pacing.hpp
 * @brief v1.2.0 M3 (video plan P1) — the token-bucket pacer.
 *
 * ## What this module is
 *
 * A **token bucket** on a monotonic clock.  The bucket holds *bytes*; the
 * rate is expressed in **bits per second**, because that is the unit every
 * other knob in the project uses (the bandwidth estimator, `EngineConfig`,
 * the scheduler's BWE budget).  The conversion happens once, inside the
 * bucket: `bytes_per_second = rate_bps / 8`.
 *
 * The bucket is the rate-shaping primitive of the send path.  Its job is
 * to make the bytes leave the machine at a *rate* rather than in a burst:
 * a token bucket accumulates credit in proportion to the elapsed time and
 * hands it out as packets are sent, so a sender that has been idle
 * releases at most one bucket's worth and then settles into the
 * configured rate.
 *
 * ## Why the time is a parameter
 *
 * `refill()`, `available()` and `try_consume()` all take the current time
 * point as an argument.  The bucket never reads a clock itself.  That
 * makes the behaviour **deterministic and testable**: a test advances a
 * synthetic clock by exactly 125 ms and asserts exactly what came out,
 * with no sleeps, no flakiness and no dependence on the host's tick
 * rate.  The scheduler passes `core::SteadyClock::now()`.
 *
 * ## Units
 *
 * - `rate_bps` — bits per second.  0 means **no pacing** (the bucket is
 *   unlimited and every byte is allowed through).
 * - `burst_bytes` — the bucket's capacity: the largest amount of bytes
 *   the bucket may release in one go.  0 means "one burst interval's
 *   worth", which is 100 ms at the configured rate.
 *
 * The internal accounting is in **milli-bytes** so the fractional
 * remainder of a packet's cost is not lost between two calls: a 125 B/s
 * bucket that is asked for 100 B does not lose 20 B to integer division.
 *
 * ## Thread safety
 *
 * A `TokenBucket` is **not** internally synchronised.  It is a value
 * object: the owner (the scheduler) holds it under its own lock.  Copying
 * a bucket copies its state, so two copies are two independent buckets.
 *
 * @see docs/plan/video-pipeline-v2-plan.md (P1)
 */

#pragma once

// Stability classification: NIMRTC_API_UNSTABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>

#include <cstdint>

#include <nimrtc/core/time.hpp>  // core::TimePoint

namespace nimrtc::pacing {

// ---------------------------------------------------------------------------
// PacerConfig
// ---------------------------------------------------------------------------

/** The configuration of one token bucket. */
struct NIMRTC_API_UNSTABLE PacerConfig {
    /** The pacing rate, in bits per second.
     *
     *  0 means **no pacing**: the bucket is unlimited and every byte is
     *  allowed through immediately.  This is the default, so a bucket
     *  that was never configured does not throttle anything. */
    std::uint32_t rate_bps = 0;

    /** The bucket's capacity in bytes — the largest amount of bytes the
     *  bucket may release at once.
     *
     *  0 means "the default burst", which is 100 ms worth of the
     *  configured rate (at least one byte).  A burst that is much larger
     *  than the packet size lets a backlog escape in one go, which is
     *  exactly what pacing is meant to prevent; a burst that is smaller
     *  than the packet size would stall the stream, so the default is
     *  deliberately a fraction of a second rather than a fraction of a
     *  packet. */
    std::uint32_t burst_bytes = 0;
};

// ---------------------------------------------------------------------------
// TokenBucket
// ---------------------------------------------------------------------------

/** A token bucket on a monotonic clock.
 *
 *  Typical use — a sender that must not exceed a rate:
 *
 *  @code
 *  pacing::TokenBucket bucket{pacing::PacerConfig{.rate_bps = 500'000}};
 *  const auto now = core::SteadyClock::now();
 *  if (bucket.try_consume(bytes, now)) {
 *      send(bytes);   // the bytes left the machine within the rate
 *  }
 *  @endcode
 *
 *  A bucket that is asked for more bytes than it holds **refuses** the
 *  request and takes nothing: the caller keeps the bytes and retries
 *  later.  The refusal is counted (`bytes_blocked()`), so a caller can
 *  tell "the rate is the limit" from "the bucket is empty because the
 *  clock did not move".
 */
class NIMRTC_API_UNSTABLE TokenBucket {
public:
    /** Construct an unlimited bucket (`rate_bps == 0`). */
    TokenBucket() noexcept = default;

    /** Construct a bucket from a configuration. */
    explicit TokenBucket(PacerConfig cfg) noexcept;

    /** Replace the configuration.
     *
     *  The accumulated credit is **kept** and clamped to the new burst
     *  ceiling, so lowering the rate does not throw away credit that was
     *  already earned (and raising it does not grant a free burst). */
    void set_config(const PacerConfig& cfg) noexcept;

    /** The current configuration (the *effective* burst is reported as
     *  the configured value — 0 still means "the default burst"). */
    [[nodiscard]] PacerConfig config() const noexcept;

    /** Change the rate, keeping the accumulated credit. */
    void set_rate_bps(std::uint32_t rate_bps) noexcept;

    /** The configured rate, in bits per second. */
    [[nodiscard]] std::uint32_t rate_bps() const noexcept;

    /** The bucket's capacity in bytes — the effective burst ceiling
     *  (the configured value resolved against the default). */
    [[nodiscard]] std::uint32_t burst_bytes() const noexcept;

    /** Refill for the time that elapsed since the last call and return
     *  the credit now available, in bytes.
     *
     *  @param now  The current time point.  A time point that is *older*
     *              than the last one (a non-monotonic clock) grants no
     *              credit — the bucket never runs backwards. */
    std::uint64_t refill(core::TimePoint now) noexcept;

    /** The credit available right now, in bytes.  The bucket is refilled
     *  first, exactly as `refill()` does. */
    [[nodiscard]] std::uint64_t available(core::TimePoint now) noexcept;

    /** Try to take `bytes` out of the bucket.
     *
     *  @return true when the credit was available and has been taken;
     *          false when it was not (nothing is taken in that case). */
    bool try_consume(std::uint64_t bytes, core::TimePoint now) noexcept;

    /** Drop the accumulated credit, restart the clock and clear the
     *  telemetry counters. */
    void reset(core::TimePoint now) noexcept;

    /** The bytes that were successfully taken out of the bucket. */
    [[nodiscard]] std::uint64_t bytes_sent() const noexcept;

    /** The number of `try_consume()` calls that were refused because the
     *  credit was short. */
    [[nodiscard]] std::uint64_t bytes_blocked() const noexcept;

private:
    /** The rate, in bits per second.  0 = unlimited. */
    std::uint32_t rate_bps_ = 0;

    /** The burst ceiling, in bytes.  0 = the default burst. */
    std::uint32_t burst_bytes_cfg_ = 0;

    /** The accumulated credit, in milli-bytes. */
    std::uint64_t credit_milli_bytes_ = 0;

    /** The time point the credit was last computed for. */
    core::TimePoint last_time_{};

    /** True once `last_time_` holds a meaningful value. */
    bool has_last_time_ = false;

    /** Telemetry. */
    std::uint64_t bytes_sent_ = 0;
    std::uint64_t bytes_blocked_ = 0;

    /** The rate in bytes per second (the rate is stored in bits). */
    [[nodiscard]] std::uint64_t rate_bytes_per_sec() const noexcept;

    /** The effective burst ceiling, in bytes. */
    [[nodiscard]] std::uint32_t effective_burst_bytes() const noexcept;
};

}  // namespace nimrtc::pacing
