#pragma once


// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is a
// v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>
#include <cstdint>
#include <optional>

#include <nimrtc/core/time.hpp>

// =============================================================================
// nimrtc::jb_strategy
// -----------------------------------------------------------------------------
// Shared adaptive jitter-buffer strategy.
//
// Before this module the audio jitter buffer (`nimrtc::jb`) and the video
// jitter buffer (`nimrtc::video_jb`) each carried their own notion of
// "how much delay should we play out with".  The audio side had an
// RFC 3550 §6.4.1 interarrival-jitter estimator driving an adaptive target
// delay; the video side had no jitter model at all and only a fixed frame
// timeout.  v1.3 track Q2 ("视频侧自适应 JB 统一") collapses the two into
// one implementation with two front-ends:
//
//   - `nimrtc::jb`         — audio front-end (packet-level, RTP timestamp keyed)
//   - `nimrtc::video_jb`   — video front-end (frame-level, FU-A aware)
//
// Both front-ends own their own buffering data structures; the *policy*
// (how to estimate jitter and how to derive a playout delay from it) lives
// here exactly once.
//
// ## Jitter model
//
// RFC 3550 §6.4.1 defines the interarrival jitter as a smoothed absolute
// transit difference:
//
//     D(i-1,i) = (R(i-1) - R(i)) - (S(i-1) - S(i))
//     J(i)     = J(i-1) + (|D(i-1,i)| - J(i-1)) / 16
//
// where R is the arrival time in RTP timestamp units and S is the media
// clock.  `AdaptiveDelayTracker` implements this with microsecond inputs.
//
// ## Delay model
//
// The playout delay tracks the jitter estimate with a safety margin:
//
//     target = clamp(base_delay + jitter_multiplier * J, min_delay, max_delay)
//
// `base_delay` covers the frame/packet duration plus a small cushion;
// `jitter_multiplier` is the classic 4-sigma safety factor.
//
// The tracker is *not* thread-safe: each front-end owns one instance and
// drives it from its own single-threaded processing path.
// =============================================================================
namespace nimrtc::jb_strategy {

// -----------------------------------------------------------------------------
// DelayPolicy — the tunable knobs of the shared strategy.
// -----------------------------------------------------------------------------
struct NIMRTC_API_STABLE DelayPolicy {
    /** Fixed component of the playout delay (packet/frame duration + cushion). */
    core::Milliseconds base_delay = core::Milliseconds{20};

    /** Lower bound of the playout delay. */
    core::Milliseconds min_delay = core::Milliseconds{20};

    /** Upper bound of the playout delay. */
    core::Milliseconds max_delay = core::Milliseconds{400};

    /** Safety factor applied to the jitter estimate (4 ≈ 4 sigma). */
    double jitter_multiplier = 4.0;

    /** When false the target delay is pinned to `base_delay` (fixed mode). */
    bool adaptive = true;
};

// -----------------------------------------------------------------------------
// AdaptiveDelayTracker — jitter estimator + target delay derivation.
// -----------------------------------------------------------------------------
class NIMRTC_API_STABLE AdaptiveDelayTracker {
public:
    AdaptiveDelayTracker() noexcept;
    explicit AdaptiveDelayTracker(DelayPolicy policy) noexcept;

    /** Feed one packet arrival.  `arrival_us` is the monotonic arrival time
     *  in microseconds, `media_clock_us` is the packet's media-clock reading
     *  converted to microseconds.  Both must use the same time base so the
     *  transit difference is meaningful. */
    void observe(std::int64_t arrival_us, std::int64_t media_clock_us) noexcept;

    /** Smoothed interarrival jitter estimate, in milliseconds. */
    core::Milliseconds jitter() const noexcept;

    /** Current playout delay derived from the jitter estimate, clamped to
     *  [min_delay, max_delay].  In fixed mode this is `base_delay`. */
    core::Milliseconds target_delay() const noexcept;

    /** Number of observations fed into the estimator so far. */
    std::uint64_t observations() const noexcept;

    /** Discard all accumulated state (new stream / reset). */
    void reset() noexcept;

    /** Live reconfigure.  The accumulated jitter estimate is preserved. */
    void set_policy(DelayPolicy policy) noexcept;

    DelayPolicy policy() const noexcept;

private:
    DelayPolicy                policy_;
    double                     jitter_ms_       = 0.0;
    std::optional<std::int64_t> last_transit_us_;
    std::uint64_t              observations_    = 0;
};

} // namespace nimrtc::jb_strategy
