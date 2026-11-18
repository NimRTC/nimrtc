/**
 * @file src/engine/src/engine_capture_cadence.hpp
 * @brief The capture cadence record (v1.2 plan Track S / S7).
 *
 * ## What this file is
 *
 * The tally the video pump keeps of the frames the *capture device*
 * delivered: how many, and over what span of the device's own clock.
 * `Stats::remote_desktop.capture_fps` is read from it.
 *
 * It is a separate header, with no dependence on the engine, on the
 * plugins or on the core library, for one reason: the counting rule is
 * subtle enough that it is pinned by a **deterministic** unit test
 * rather than by driving a device through a timing-sensitive stall
 * (`tests/test_engine_remote_desktop_stats.cpp`).  The rule and the
 * test must be the same rule, so both include this file.
 *
 * ## The rule
 *
 * - **Frames are counted by sequence number.**  A device's timestamp is
 *   only sampled as the *span endpoint*; it is never assumed monotonic,
 *   because it is not:
 *
 *   - the device's queue is bounded, so when the pump falls behind the
 *     device drops the oldest frame and the next handoff can arrive
 *     after frames the pump never saw -- the count must not lose them;
 *   - a source can restart its clock (a monitor hot-plugged, a portal
 *     re-negotiated), so a *new* frame's timestamp can jump **back**.
 *     That frame is still a delivery and is counted.
 *
 * - **A repeated sequence number is the same delivery** handed over
 *   twice and is not counted again.
 *
 * - **A span that moved backwards is broken for the session.**  The
 *   rate is then reported as unknown (0), never as a plausible-looking
 *   wrong number: the endpoint no longer describes the interval the
 *   count spans, so no division of the two is meaningful.
 *
 * - The rate is `(count - 1) / (last - first)` over the device's own
 *   clock, so a device that stamps frames itself yields the rate it
 *   actually ran at rather than the rate the engine *drained* it.
 */

#pragma once

#include <cstdint>

namespace nimrtc::engine::detail {

/**
 * The capture cadence of one session.
 *
 * Header-only and trivially destructible: it lives inline in the
 * engine's video state, is touched only from the tick thread, and costs
 * one branch per popped frame.
 */
struct CaptureCadence {
    std::int64_t  first_ts_us = 0;
    std::int64_t  last_ts_us  = 0;
    std::uint64_t frames      = 0;
    bool          has_seq     = false;
    std::uint32_t last_seq    = 0;
    /** Whether the timestamp span is still usable (strictly increasing). */
    bool          span_valid  = true;

    /** Record one delivered frame (its timestamp and its sequence). */
    void note(std::int64_t capture_ts_us, std::uint32_t frame_seq) noexcept {
        if (has_seq && frame_seq == last_seq) return;   // same delivery

        if (frames == 0) {
            first_ts_us = capture_ts_us;
            last_ts_us  = capture_ts_us;
            has_seq     = true;
            last_seq    = frame_seq;
            span_valid  = true;
            ++frames;
            return;
        }

        // A valid span only grows forward.  Anything else means the
        // device's clock is not the interval we think it is.
        if (capture_ts_us <= last_ts_us) span_valid = false;

        last_ts_us = capture_ts_us;
        last_seq   = frame_seq;
        ++frames;
    }

    /** The device's frame rate, in frames per second (0 when unknown). */
    [[nodiscard]] double fps() const noexcept {
        if (frames < 2 || !span_valid) return 0.0;
        const auto span_us = last_ts_us - first_ts_us;
        if (span_us <= 0) return 0.0;
        return static_cast<double>(frames - 1) * 1'000'000.0 /
               static_cast<double>(span_us);
    }
};

}  // namespace nimrtc::engine::detail
