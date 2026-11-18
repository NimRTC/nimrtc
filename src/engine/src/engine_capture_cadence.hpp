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
 * - **A delivery is identified by its sequence number *and* its
 *   timestamp.**  The same frame handed over twice carries the same
 *   pair and is not counted again.  A frame that repeats the sequence
 *   number but carries a *different* timestamp is a new delivery: it
 *   is how a device that does not number its frames at all (the
 *   ScreenCaptureKit and X11 backends leave `frame_seq` at zero) is
 *   recognised, and such a device must not be counted once and then
 *   ignored for the rest of the session.
 *
 * - **The rate comes from the sequence span, not from the seen count.**
 *   The device's queue is bounded, so when the pump falls behind the
 *   device drops the oldest frame and the pump never sees it.  Dividing
 *   the *seen* frames by the *device's* clock span would then report a
 *   rate below the device's own — a number that answers "how fast did
 *   the engine drain the device?" while claiming to answer "how fast
 *   did the device run?".  The sequence number is the device's own
 *   production counter, so the span between the first and the last
 *   sequence counts the frames the device *produced*, dropped ones
 *   included, and dividing that by the timestamp span gives the rate
 *   the device actually ran at.
 *
 * - **A device that does not number its frames falls back to the seen
 *   count.**  There is then no way to tell a dropped frame from a
 *   delivered one, so the rate is the pipeline's rate at the device's
 *   clock — the best available answer, and the one the fallback names.
 *
 * - **A timestamp span that moved backwards is broken for the session.**
 *   The rate is then reported as unknown (0), never as a
 *   plausible-looking wrong number: the endpoint no longer describes
 *   the interval the count spans, so no division of the two is
 *   meaningful.  A source can restart its clock (a monitor hot-plugged,
 *   a portal re-negotiated), so a *new* frame's timestamp can jump
 *   **back**.  That frame is still a delivery and is counted.
 *
 * - The sequence span is computed modulo 2^32, so a counter that wraps
 *   is handled without a special case.
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
    /** The first delivery's timestamp — the start of the span. */
    std::int64_t  first_ts_us = 0;
    /** The latest delivery's timestamp — the end of the span. */
    std::int64_t  last_ts_us  = 0;
    /** The first delivery's sequence number — the start of the
     *  sequence span. */
    std::uint32_t first_seq   = 0;
    /** The latest delivery's sequence number. */
    std::uint32_t last_seq    = 0;
    /** The deliveries the pump saw.  Cumulative. */
    std::uint64_t frames      = 0;
    /** Whether the device numbers its frames at all.  A device that
     *  leaves `frame_seq` at zero never varies it, and its rate is
     *  then computed from the seen count. */
    bool          seq_usable  = false;
    /** Whether the timestamp span is still usable (strictly increasing). */
    bool          span_valid  = true;

    /** Record one delivered frame (its timestamp and its sequence). */
    void note(std::int64_t capture_ts_us, std::uint32_t frame_seq) noexcept {
        // The *same* delivery handed over twice: the same sequence
        // number *and* the same timestamp.  A repeated sequence with a
        // different timestamp is a different delivery — that is the
        // device that does not number its frames.
        if (frames != 0 && frame_seq == last_seq && capture_ts_us == last_ts_us) {
            return;
        }

        if (frames == 0) {
            first_ts_us = capture_ts_us;
            last_ts_us  = capture_ts_us;
            first_seq   = frame_seq;
            last_seq    = frame_seq;
            span_valid  = true;
            ++frames;
            return;
        }

        // A device that varies its sequence number is counting its own
        // productions, and that count is what the rate is derived from.
        if (frame_seq != last_seq) seq_usable = true;

        // A valid span only grows forward.  Anything else means the
        // device's clock is not the interval we think it is.
        if (capture_ts_us <= last_ts_us) span_valid = false;

        last_ts_us = capture_ts_us;
        last_seq   = frame_seq;
        ++frames;
    }

    /** The number of frames the device *produced* over the span.
     *
     * With a numbering device that is the sequence span (the frames the
     * device dropped at its own bounded queue are included — the device
     * produced them, the pump simply never saw them).  Without one it
     * is the number of deliveries the pump saw, which is the best the
     * engine can know.
     */
    [[nodiscard]] std::uint64_t produced() const noexcept {
        if (frames == 0) return 0;
        if (!seq_usable) return frames;
        return static_cast<std::uint64_t>(last_seq - first_seq) + 1;
    }

    /** The device's frame rate, in frames per second (0 when unknown). */
    [[nodiscard]] double fps() const noexcept {
        if (frames < 2 || !span_valid) return 0.0;
        const auto span_us = last_ts_us - first_ts_us;
        if (span_us <= 0) return 0.0;

        // The number of frames the device *produced* over the span.
        // With a numbering device that is the sequence span (the
        // dropped frames included); without one it is the frames the
        // pump saw, which is the best the engine can know.
        const std::uint64_t produced =
            seq_usable
                ? static_cast<std::uint64_t>(last_seq - first_seq)
                : (frames - 1);
        if (produced == 0) return 0.0;

        return static_cast<double>(produced) * 1'000'000.0 /
               static_cast<double>(span_us);
    }
};

}  // namespace nimrtc::engine::detail
