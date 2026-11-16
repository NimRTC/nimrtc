/**
 * @file nimrtc/rd_input/coalescer.hpp
 * @brief The input-side coalescer and rate limiter (remote-desktop plan G4).
 *
 * ## The problem
 *
 * A pointer device reports at the *device* rate — a 1000 Hz gaming mouse
 * reports 1000 samples a second.  The *display* shows 60 frames a second.
 * Sending every sample means 1000 events a second on a link that will
 * carry 60 meaningful ones, and every one of the extra 940 is latency the
 * operator feels as lag.
 *
 * So the pointer path is **coalesced to the display rate**: the deltas
 * accumulate between frames and go out as one accumulated move per display
 * interval.  The pointer is still in the right place — the *sum* of the
 * deltas is the same — but the link carries 60 events, not 1000.
 *
 * ## What is never dropped
 *
 * Key events are **never dropped and never coalesced**.  A dropped key-up
 * leaves a key stuck down on the remote machine; a coalesced key press is a
 * lost character.  So the coalescer keeps the essential events (key, button,
 * wheel, text) in an ordered queue and passes every one of them through, in
 * order, no matter what the pointer is doing.
 *
 * ## The rate limit
 *
 * The input path has its **own** rate limit, separate from the media path's:
 * a stuck device that keeps reporting the same motion must not be able to
 * flood the link.  The limiter bounds how many pointer events leave per
 * second; when the budget is exhausted the pending pointer delta is
 * *dropped* (not queued) and the counter says so.
 *
 * The limiter deliberately does **not** apply to the essential events.  That
 * is a trade-off, and it is the trade-off the plan asks for: plan G4 says
 * "key events are never dropped; the input path has its own rate limit so a
 * stuck device cannot flood the link" — the flood the plan is worried about
 * is a stuck *pointer*, and the acceptance criterion is that the key events
 * are lossless.  A host that needs a hard bound on the key path as well
 * can bound it at the transport layer, where a drop is a transport decision
 * rather than an input-path decision.
 *
 * ## The clock
 *
 * The coalescer is driven by an explicit `now_us` — the caller supplies the
 * host's monotonic clock.  The module never reads a clock itself: a test
 * has to be able to drive a 1000 Hz stream over a simulated second
 * deterministically, and a wall clock would make that a race.
 *
 * Thread-safety: not thread-safe, like every other pipeline object in this
 * codebase.  One coalescer per producing thread.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <deque>
#include <vector>

#include "nimrtc/rd_input/input_event.hpp"

namespace nimrtc::rd_input {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/** The coalescer's configuration. */
struct NIMRTC_API_STABLE CoalescerConfig {
    /** The display's refresh rate, in frames per second.  The pointer is
     *  coalesced to this rate.  60 is the common case. */
    std::uint32_t display_hz = 60;

    /** The maximum number of *pointer* events the input path emits per
     *  second.  This is the input path's own rate limit: it is separate
     *  from the media path's, so a stuck pointer device cannot flood the
     *  link.  A value of 0 means "no separate limit" (the display rate is
     *  then the only bound). */
    std::uint32_t max_pointer_events_per_second = 0;

    /** The frame interval in microseconds, derived from `display_hz`.
     *  A `display_hz` of 0 yields 0 (no coalescing — every pointer sample
     *  goes out as its own event). */
    [[nodiscard]] std::uint64_t frame_interval_us() const noexcept {
        if (display_hz == 0) return 0;
        return 1000000ull / display_hz;
    }
};

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

/** What the coalescer has done.  A host shows these in its telemetry. */
struct NIMRTC_API_STABLE CoalescerStats {
    /** Pointer-move events handed to the coalescer. */
    std::uint64_t pointer_events_in = 0;
    /** Pointer-move events the coalescer emitted. */
    std::uint64_t pointer_events_out = 0;
    /** Pointer events the rate limiter dropped. */
    std::uint64_t pointer_events_dropped = 0;
    /** Essential events (key / button / wheel / text) handed in. */
    std::uint64_t essential_events_in = 0;
    /** Essential events emitted.  Always equal to `essential_events_in`:
     *  the coalescer never drops one. */
    std::uint64_t essential_events_out = 0;
    /** Key events handed in. */
    std::uint64_t key_events_in = 0;
};

// ---------------------------------------------------------------------------
// The coalescer
// ---------------------------------------------------------------------------

/**
 * @brief Coalesces pointer motion to the display rate and rate-limits the
 *        input path, while passing every key / button / wheel / text event
 *        through, in order, without loss.
 */
class NIMRTC_API_STABLE InputCoalescer {
public:
    explicit InputCoalescer(const CoalescerConfig& config = CoalescerConfig{});

    /** The configuration this coalescer was built with. */
    [[nodiscard]] const CoalescerConfig& config() const noexcept { return config_; }

    /**
     * @brief Hand an event to the coalescer.
     *
     * @param event  the captured event.
     * @param now_us the host's monotonic clock, in microseconds.
     * @return true if the event was accepted (always true — the coalescer
     *         never refuses an event; the pointer's *rate* is what it
     *         bounds, not its arrival).
     */
    bool push(const InputEvent& event, std::uint64_t now_us);

    /**
     * @brief Take the events that should go on the wire now.
     *
     * The essential events come out first, in the order they arrived.  The
     * accumulated pointer motion comes out last, and only when a display
     * interval has passed.  A pointer event that carries an absolute
     * position (a button press) positions the remote cursor itself, so a
     * late motion event does not misplace a click.
     *
     * @param now_us the host's monotonic clock, in microseconds.
     * @return the events to send.  Empty when nothing is due.
     */
    [[nodiscard]] std::vector<InputEvent> drain(std::uint64_t now_us);

    /** Discard everything that is pending.  Used when the session resets. */
    void reset() noexcept;

    /** The statistics so far. */
    [[nodiscard]] const CoalescerStats& stats() const noexcept { return stats_; }

private:
    /** Emit the accumulated pointer delta, if the display rate allows it. */
    void flush_pointer(std::uint64_t now_us, std::vector<InputEvent>& out);

    CoalescerConfig config_;
    CoalescerStats   stats_{};

    /// The accumulated pointer motion, not yet emitted.
    std::int32_t pending_dx_ = 0;
    std::int32_t pending_dy_ = 0;
    bool         pending_valid_ = false;

    /// The last position the pointer was reported at (for a button event's
    /// absolute coordinates).
    std::int32_t last_x_ = 0;
    std::int32_t last_y_ = 0;

    /// When the last pointer event was emitted.
    std::uint64_t last_pointer_emit_us_ = 0;

    /// When the pointer rate budget was last refilled.
    std::uint64_t rate_window_start_us_ = 0;
    std::uint32_t pointer_events_this_window_ = 0;

    /// The capture timestamp of the most recent pointer sample, stamped on
    /// the coalesced motion event.
    std::uint64_t pending_motion_ts_us_ = 0;

    /// The essential events, in arrival order.
    std::deque<InputEvent> essential_;
};

} // namespace nimrtc::rd_input
