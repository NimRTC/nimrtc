/**
 * @file nimrtc/video_sink/displayed_frame.hpp
 * @brief The "what the operator saw" record (remote-desktop plan E9).
 *
 * ## What this is
 *
 * Plan E9: "the receive side records, per displayed frame, the `ref_frame`
 * id and the capture timestamp, so a host can answer 'the user clicked on
 * what they were looking at'."
 *
 * The engine already owns a `ref_frame` id -> `capture_ts_us` table (the
 * v1.1 T1 timeline).  What was missing is the **displayed-frame** table:
 * the mapping from the id of the frame that was actually put on the
 * screen back to its capture instant.
 *
 * ## Why a module
 *
 * The tracker is a **bounded ring** of the last N displayed frames with
 * a reverse lookup.  That is pure bookkeeping — no GPU handle, no
 * window-system type — so it is unit-tested on every platform, and it is
 * the same code the Windows D3D11, the macOS Metal and the Linux EGL
 * renderers can all share.
 *
 * ## The reverse lookup
 *
 * The interesting operation is the **reverse** one: given a `ref_frame`
 * id that arrived in an input event, find the frame that was on screen
 * when the operator generated it.  The id is the peer's `frame_seq`, so
 * the receiver resolves it against **its own** displayed-frame table —
 * the ids are in the same space because the sender stamps the frames it
 * sends and the receiver stamps the frames it shows.
 *
 * @note Remote-desktop plan E9 (module half).  The engine's public
 *       accessor for the last displayed frame is the engine's.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace nimrtc::video_sink {

// ---------------------------------------------------------------------------
// DisplayedFrameInfo
// ---------------------------------------------------------------------------

/**
 * @brief One displayed frame's identity.
 *
 * The three fields answer the three questions a host asks:
 *
 *   - *which* frame was shown — `ref_frame_id`;
 *   - *when* it was captured — `capture_ts_us`;
 *   - *when* it reached the screen — `present_ts_us`.
 *
 * The difference between the last two is the presentation latency that
 * the frame was actually subject to.
 */
struct NIMRTC_API_STABLE DisplayedFrameInfo {
    /** The `ref_frame` id of the displayed frame.  It is the peer's
     *  `frame_seq` — the same id space the input events carry. */
    std::uint64_t ref_frame_id = 0;

    /** The frame's capture timestamp, in microseconds on the *sender's*
     *  monotonic clock. */
    std::int64_t capture_ts_us = 0;

    /** The local monotonic instant at which the frame was presented, in
     *  microseconds. */
    std::int64_t present_ts_us = 0;

    /** The frame's sequence number within the receive stream. */
    std::uint32_t frame_seq = 0;
};

// ---------------------------------------------------------------------------
// DisplayedFrameTracker
// ---------------------------------------------------------------------------

/**
 * @brief A bounded history of the frames that were actually displayed.
 *
 * ## The bound
 *
 * The tracker keeps at most `capacity` entries.  When it is full the
 * **oldest** entry falls out — the history is a *recent* window, not a
 * complete log.  A remote-desktop session can run for hours, and an
 * unbounded log of every displayed frame would be a slow memory leak.
 *
 * ## Threading
 *
 * Not thread-safe.  One tracker belongs to one receive stream, and the
 * presenter thread is the only writer.
 */
class NIMRTC_API_STABLE DisplayedFrameTracker {
public:
    /**
     * @brief Construct a tracker.
     *
     * @param capacity the maximum number of entries.  Zero is treated as
     *                 one — a tracker that remembers nothing cannot
     *                 answer a question.
     */
    explicit DisplayedFrameTracker(std::size_t capacity = 256);

    /**
     * @brief Record a displayed frame.
     *
     * @param info the frame's identity.
     */
    void record(DisplayedFrameInfo info) noexcept;

    /**
     * @brief The most recently displayed frame, or nullopt.
     *
     * This is the engine's `last_displayed_frame()` answer: "what was on
     * the screen right now?".
     */
    std::optional<DisplayedFrameInfo> last() const noexcept;

    /**
     * @brief Find a displayed frame by its `ref_frame` id.
     *
     * @return the frame, or nullopt when the id is not in the window.
     */
    std::optional<DisplayedFrameInfo> find(std::uint64_t ref_frame_id) const noexcept;

    /**
     * @brief Find the capture timestamp of a displayed frame by its id.
     *
     * The convenience form of `find()` — this is what the input↔frame
     * binding needs.
     */
    std::optional<std::int64_t> capture_ts_for(std::uint64_t ref_frame_id) const noexcept;

    /** The number of entries currently held. */
    std::size_t size() const noexcept;

    /** The maximum number of entries this tracker holds. */
    std::size_t capacity() const noexcept;

    /** The total number of frames recorded since the last reset. */
    std::uint64_t recorded_total() const noexcept;

    /** The number of entries that fell out of the window. */
    std::uint64_t evicted_total() const noexcept;

    /** Forget everything. */
    void clear() noexcept;

private:
    std::size_t capacity_;
    std::vector<DisplayedFrameInfo> entries_;
    std::uint64_t recorded_ = 0;
    std::uint64_t evicted_ = 0;
};

} // namespace nimrtc::video_sink
