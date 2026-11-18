/**
 * @file src/engine/src/engine_rate_window.hpp
 * @brief PRIVATE engine collaborator: the **windowed outbound rate**
 *        measurement (v1.2 plan Track E / E6).
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * ## What this file is
 *
 * `IVideoBitrateController::on_measured_bitrate(bps)` is the seam the
 * rate controller is *designed* around: the link's estimate says what
 * the path can carry, and the measurement says what the encoder is
 * *actually* putting on the wire.  The controller compares the two and
 * applies a multiplicative decrease when the encoder overruns the
 * estimate (see `BitrateController::budget()`).
 *
 * Until this collaborator existed, **nothing in the engine ever called
 * that seam**.  The engine had the encoder's outbound byte counters
 * (`video_bytes_sent`) and the controller had the seam, and the two
 * were never joined — so the closed loop the saturation benchmark
 * measures was not the loop the product runs.
 *
 * ## The rule
 *
 * - **The window is one second.**  A rate is "bytes over a span of
 *   time", and the span has to be long enough to be a *rate* rather
 *   than an instantaneous spike, and short enough to react before the
 *   link has already failed.
 *
 * - **The window is closed, not sliding.**  The measurement is taken
 *   over the interval between two observations, and it is reported
 *   once that interval has elapsed.  A sliding window would need the
 *   engine to remember every sample in the window; a closed window
 *   needs two numbers and an anchor.
 *
 * - **The first observation reports nothing.**  There is no interval
 *   yet at the first observation, so there is no rate: the controller
 *   is handed 0, which its own contract reads as "no measurement"
 *   (`measured_bps_ == 0` means "not congested"), so nothing happens.
 *   Reporting a rate computed over a zero-length interval would be a
 *   division by zero dressed up as a number.
 *
 * - **A byte counter that goes backwards breaks the measurement for
 *   that interval.**  The counters are cumulative and monotonic, so a
 *   decrease means the session was reset underneath the measurement.
 *   The rate is then reported as 0 (unknown) rather than as a
 *   plausible-looking wrong number — the same rule the capture-cadence
 *   collaborator follows (see `engine_capture_cadence.hpp`).
 *
 * - **A counter that does not move reports 0.**  "The encoder sent
 *   nothing in the last second" is a *fact*, and the controller needs
 *   to know it: a stream that has gone quiet is not a stream that is
 *   overrunning the link.  Reporting the previous rate would be a lie
 *   about the current interval.
 *
 * ## Why it is a collaborator and not three members
 *
 * The measurement is three numbers that belong together: the byte
 * count at the anchor, the anchor's timestamp, and the last reported
 * rate.  Splitting them across the video state would put them next to
 * each other only by luck, and the *rule* — which interval, which
 * direction, what a reset means — is the part worth pinning by a
 * deterministic test rather than by a timing-sensitive one.
 */

#pragma once

#include <cstdint>

namespace nimrtc::engine::detail {

/**
 * The windowed outbound rate of one stream.
 *
 * Header-only and trivially destructible: it lives inline in the
 * engine's video state, is touched only from the tick thread, and costs
 * one comparison per tick.
 */
struct RateWindow {
    /** The length of the measurement window, in microseconds. */
    static constexpr std::int64_t kWindowUs = 1'000'000;

    /** The byte count as of the last observation (the anchor). */
    std::uint64_t anchor_bytes = 0;

    /** The timestamp of the last observation (the anchor), in
     *  microseconds.  Zero means "no observation yet". */
    std::int64_t anchor_us = 0;

    /** The rate reported by the most recent *completed* window, in bits
     *  per second.  Zero means "no completed window yet". */
    std::uint32_t last_bps = 0;

    /** Whether at least one observation has been made. */
    bool have_anchor = false;

    /**
     * Observe the byte counter and, if a window has closed, report the
     * rate it spanned.
     *
     * @param bytes     The stream's cumulative outbound byte count.
     * @param now_us    The current monotonic time, in microseconds.
     * @param out_bps   Receives the window's rate in bits per second, or
     *                  0 when no window has closed yet.
     * @return          True when a window closed and `out_bps` was
     *                  written.
     */
    bool observe(std::uint64_t bytes, std::int64_t now_us,
                 std::uint32_t& out_bps) noexcept {
        out_bps = 0;

        if (!have_anchor) {
            // The first observation establishes the anchor.  There is no
            // interval yet, so there is no rate.
            anchor_bytes = bytes;
            anchor_us    = now_us;
            have_anchor  = true;
            return false;
        }

        // The window has not closed yet: keep the anchor where it is,
        // so the next observation spans the *whole* window rather than
        // a shorter one.
        if (now_us - anchor_us < kWindowUs) return false;

        const std::int64_t span_us = now_us - anchor_us;
        const std::uint64_t delta =
            (bytes >= anchor_bytes) ? (bytes - anchor_bytes) : 0;

        // The anchor moves forward either way: the window that just
        // closed is over, and the next one starts here.  A counter that
        // went backwards is reported as an unknown rate, but the
        // measurement must not wedge on a stale anchor.
        anchor_bytes = bytes;
        anchor_us    = now_us;

        if (span_us <= 0) return false;

        // bytes * 8 bits/byte * 1e6 us/s / span_us us.
        const std::uint64_t bps =
            (static_cast<std::uint64_t>(delta) * 8ull * 1'000'000ull) /
            static_cast<std::uint64_t>(span_us);

        // A rate that does not fit in 32 bits is a rate no link on
        // earth carries; clamping is the honest answer, and the
        // controller's own budget arithmetic is 32-bit too.
        last_bps = (bps > 0xFFFFFFFFull) ? 0xFFFFFFFFu
                                          : static_cast<std::uint32_t>(bps);
        out_bps  = last_bps;
        return true;
    }
};

}  // namespace nimrtc::engine::detail
