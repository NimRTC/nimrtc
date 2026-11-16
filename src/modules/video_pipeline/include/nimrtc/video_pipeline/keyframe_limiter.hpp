/**
 * @file nimrtc/video_pipeline/keyframe_limiter.hpp
 * @brief Keyframe-storm limiter — remote-desktop plan B14.
 *
 * ## The problem
 *
 * A peer that asks for a key frame every 100 ms is either broken, or is
 * a peer that keeps losing the picture.  Answering every request
 * produces an IDR storm: an IDR costs roughly an order of magnitude
 * more bytes than an inter frame, so a storm does not "recover" the
 * picture — it saturates the link and makes the picture *worse*.
 *
 * The plan says it plainly (B14): "a peer that requests a keyframe every
 * 100 ms produces at most the rate-limited number of IDRs.  The existing
 * `FirCoalescer` is finally wired into the engine."
 *
 * ## The mechanism
 *
 * `FirCoalescer` (RFC 5104 §4.3.1.1) is the *sender-side* rate limiter
 * for FIR messages: it remembers when the last FIR was sent per media
 * SSRC and refuses a second one inside the minimum interval.  It was
 * written but never connected to the keyframe path, so a PLI storm
 * reached the encoder unchecked.
 *
 * This class is the **encoder-side** counterpart: it decides whether a
 * keyframe request is *granted now* or *deferred*, and it is the thing
 * the pipeline's keyframe-request path consults.  It reuses the
 * coalescer's interval gate (the same policy, the same number) and adds
 * two things the coalescer does not have:
 *
 *  - a **cost budget** — a keyframe is N times the cost of an inter
 *    frame, so the limiter keeps a running estimate of the "keyframe
 *    budget" it has spent and refuses to blow past it inside one
 *    window.  This is the encoder-side counterpart of the plan's D3
 *    ("keyframe-cost budgeting" on the *controller* side).
 *  - a **hard cap** — the maximum number of granted keyframes inside one
 *    window, whatever the cost budget says.
 *
 * ## The one thing that is never deferred
 *
 * A **resolution change** forces a keyframe *and* the new parameter
 * sets (SPS / PPS, VPS, AV1 sequence header — plan B13).  Deferring it
 * breaks the decode: the peer receives a frame whose geometry its
 * parameter sets do not describe and cannot decode it at all.  So the
 * resolution-change request is **always granted immediately**, and the
 * limiter reports it as such (`KeyframeDecision::kGranted` with the
 * reason `kResolutionChange`).
 *
 * ## Threading
 *
 * Not thread-safe.  One limiter per outbound video stream, driven from
 * the thread that owns the encoder.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v1.13.x follow-up.
#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <string_view>

#include <nimrtc/video_rtcp_feedback/video_rtcp_feedback.hpp>

namespace nimrtc::video_pipeline {

// ---------------------------------------------------------------------------
// The vocabulary
// ---------------------------------------------------------------------------

/** Why a key frame was asked for.  The reason decides whether the
 *  request may be deferred. */
enum class NIMRTC_API_STABLE KeyframeReason : std::uint8_t {
    /** A Picture Loss Indication (RFC 4585 §6.3.1) from the peer. */
    kPli = 0,
    /** A Full Intra Request (RFC 5104 §4.3.1) from the peer. */
    kFir = 1,
    /** The resolution changed — a parameter-set change (plan B13).
     *  **Never deferred.** */
    kResolutionChange = 2,
    /** The codec was switched.  A new codec cannot decode the old
     *  stream, so this is treated like a resolution change: it is never
     *  deferred. */
    kCodecSwitch = 3,
    /** The encoder asked for a key frame on its own (e.g. after a
     *  restart). */
    kEncoderRequest = 4,
};

/** Human-readable name of a key-frame reason. */
NIMRTC_API_STABLE std::string_view keyframe_reason_name(
    KeyframeReason reason) noexcept;

/** The limiter's verdict on one key-frame request. */
enum class NIMRTC_API_STABLE KeyframeDecision : std::uint8_t {
    /** The key frame happens now. */
    kGranted = 0,
    /** The request was held back; the encoder may keep producing inter
     *  frames and the request is retried later. */
    kDeferred = 1,
    /** The request was dropped — the limiter's budget is exhausted and
     *  deferring it would not help. */
    kRejected = 2,
};

/** Human-readable name of a key-frame decision. */
NIMRTC_API_STABLE std::string_view keyframe_decision_name(
    KeyframeDecision decision) noexcept;

/** Why the limiter decided what it decided. */
enum class NIMRTC_API_STABLE KeyframeDecisionReason : std::uint8_t {
    /** No reason recorded (the default). */
    kNone = 0,
    /** Nothing was limiting: this is the first key frame, or the
     *  minimum interval has passed. */
    kNoLimiter = 1,
    /** The minimum interval between key frames has not elapsed. */
    kMinInterval = 2,
    /** The key-frame cost budget for the current window is spent. */
    kCostBudget = 3,
    /** The hard cap on key frames per window is reached. */
    kHardCap = 4,
    /** A resolution change (or a codec switch): the request bypasses
     *  every limiter, because deferring it breaks the decode. */
    kResolutionChange = 5,
    /** The limiter is disabled — every request is granted. */
    kDisabled = 6,
};

/** Human-readable name of a decision reason. */
NIMRTC_API_STABLE std::string_view keyframe_decision_reason_name(
    KeyframeDecisionReason reason) noexcept;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/** The limiter's configuration. */
struct NIMRTC_API_STABLE KeyframeLimiterConfig {
    /** The minimum interval between two granted key frames, in
     *  microseconds.
     *
     *  0 disables the interval gate (every request that passes the other
     *  gates is granted).  The default is 250 ms — the same number
     *  `video_rtcp_feedback::kFirMinIntervalUs` uses for the FIR
     *  coalescer, because the two are the *same policy* seen from two
     *  ends. */
    std::int64_t min_interval_us = 250'000;

    /** The key-frame **cost budget**, expressed in "inter frame
     *  equivalents" per window.
     *
     *  A key frame is not free: it costs roughly `keyframe_cost_ratio`
     *  times an inter frame.  The limiter keeps a running estimate of
     *  the budget spent inside the current window and refuses to spend
     *  more than `keyframe_budget` of it.  `keyframe_budget == 0`
     *  disables the cost gate. */
    std::uint32_t keyframe_budget = 32;

    /** How many times more expensive a key frame is than an inter frame.
     *  Used to convert the budget into a "key frames per window" bound.
     *  Must be at least 1. */
    std::uint32_t keyframe_cost_ratio = 8;

    /** The hard cap on granted key frames inside one window, whatever
     *  the cost budget says.  0 disables the cap. */
    std::uint32_t max_keyframes_per_window = 4;

    /** The length of the accounting window, in microseconds.  The
     *  budget and the cap are reset when the window rolls over. */
    std::int64_t window_us = 1'000'000;

    /** The media SSRC the key frames belong to.  It is the key the
     *  `FirCoalescer` accounts per (RFC 5104 §4.3.1.1 rate-limits per
     *  media SSRC), so the limiter must know which stream it is
     *  guarding. */
    std::uint32_t media_ssrc = 0;

    /** True iff the limiter does nothing (every request is granted).
     *  The default is false — the limiter is *on*. */
    bool disabled = false;
};

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

/** The limiter's counters, so a host can see it working. */
struct NIMRTC_API_STABLE KeyframeLimiterStats {
    /** Total key-frame requests offered to the limiter. */
    std::uint64_t requests = 0;
    /** Requests granted immediately. */
    std::uint64_t granted = 0;
    /** Requests held back (retried later). */
    std::uint64_t deferred = 0;
    /** Requests dropped outright. */
    std::uint64_t rejected = 0;
    /** Grants that bypassed the limiter because of a resolution change. */
    std::uint64_t forced_grants = 0;
    /** The number of granted key frames inside the current window. */
    std::uint32_t keyframes_in_window = 0;
};

// ---------------------------------------------------------------------------
// KeyframeLimiter
// ---------------------------------------------------------------------------

/**
 * @brief The key-frame request arbiter (remote-desktop plan B14).
 *
 * The pipeline's key-frame-request path asks the limiter before it arms
 * the encoder.  The limiter answers with a decision and a reason, and
 * keeps the counters.
 */
class NIMRTC_API_STABLE KeyframeLimiter {
public:
    KeyframeLimiter() noexcept;

    explicit KeyframeLimiter(KeyframeLimiterConfig cfg) noexcept;

    /**
     * @brief Build a limiter around an existing FIR coalescer.
     *
     *  This is the **wiring** plan B14 asks for: the `FirCoalescer`
     *  (RFC 5104 §4.3.1.1) is the mechanism that was written but never
     *  connected, and the limiter is what puts it on the key-frame
     *  path.  The coalescer owns the "may I send a key-frame request
     *  now" gate; the limiter owns the "may the encoder produce a key
     *  frame now" gate — and the two carry the *same* interval, so the
     *  request rate and the key-frame rate cannot disagree.
     *
     *  The coalescer is owned by the caller and must outlive the
     *  limiter.
     */
    explicit KeyframeLimiter(video_rtcp_feedback::FirCoalescer& coalescer,
                              KeyframeLimiterConfig cfg) noexcept;

    /** True iff the limiter was built around a FIR coalescer. */
    bool uses_fir_coalescer() const noexcept { return coalescer_ != nullptr; }

    /** The coalescer the limiter consults (null when there is none). */
    video_rtcp_feedback::FirCoalescer* fir_coalescer() const noexcept {
        return coalescer_;
    }

    /** Offer a key-frame request to the limiter.
     *
     *  @param reason Why the key frame was asked for.
     *  @param now_us Monotonic timestamp in microseconds.
     *  @param carries_parameter_sets True iff the granted key frame must
     *        also carry the codec's parameter sets (SPS / PPS, VPS, AV1
     *        sequence header).  This is the case for a resolution change
     *        (plan B13) and it is what makes deferral unsafe.
     *  @return The verdict.  `kGranted` means the encoder should be
     *          armed now; `kDeferred` means "not yet"; `kRejected` means
     *          "do not bother". */
    KeyframeDecision request(KeyframeReason reason,
                             std::int64_t now_us,
                             bool carries_parameter_sets = false) noexcept;

    /** The verdict of the most recent `request()`, and its reason. */
    KeyframeDecision last_decision() const noexcept { return last_decision_; }
    KeyframeDecisionReason last_reason() const noexcept { return last_reason_; }

    /** True iff the most recent `request()` was granted *with* the
     *  parameter sets attached.  A grant that carries the parameter sets
     *  is never deferred — a deferred parameter-set change breaks the
     *  decode. */
    bool last_carries_parameter_sets() const noexcept {
        return last_carries_parameter_sets_;
    }

    /**
     * @brief True iff a deferred request is still waiting.
     *
     *  A deferred request is **held**, not dropped: the caller should
     *  re-offer it once the minimum interval has elapsed, so the picture
     *  still recovers even if the peer only asked once.  The limiter
     *  itself does not drive a timer — the caller owns the clock.
     */
    bool pending() const noexcept { return pending_; }

    /** Mark the held request as satisfied (it was granted, or it was
     *  dropped on purpose). */
    void clear_pending() noexcept { pending_ = false; }

    /** The current configuration. */
    KeyframeLimiterConfig config() const noexcept { return cfg_; }

    /** Replace the configuration.  The counters are kept. */
    void configure(KeyframeLimiterConfig cfg) noexcept;

    /** Snapshot of the counters. */
    KeyframeLimiterStats stats() const noexcept { return stats_; }

    /** Forget the accounting window (used on a stream restart). */
    void reset() noexcept;

private:
    /** Roll the accounting window over if `now_us` is past its end. */
    void roll_window(std::int64_t now_us) noexcept;

    KeyframeLimiterConfig cfg_{};
    KeyframeLimiterStats   stats_{};
    KeyframeDecision       last_decision_ = KeyframeDecision::kGranted;
    KeyframeDecisionReason last_reason_   = KeyframeDecisionReason::kNone;

    /** The monotonic time of the last granted key frame. */
    std::int64_t last_granted_us_ = 0;
    /** True once a key frame has been granted (so `last_granted_us_` is
     *  meaningful). */
    bool has_granted_ = false;
    /** The start of the current accounting window. */
    std::int64_t window_start_us_ = 0;
    /** True once the window start is meaningful. */
    bool has_window_ = false;
    /** True iff the most recent grant carried the parameter sets. */
    bool last_carries_parameter_sets_ = false;
    /** True iff a deferred request is still waiting to be re-offered. */
    bool pending_ = false;
    /** The FIR coalescer the limiter is wired to (may be null). */
    video_rtcp_feedback::FirCoalescer* coalescer_ = nullptr;
};

} // namespace nimrtc::video_pipeline
