/**
 * @file nimrtc/video_capture/capture_queue.hpp
 * @brief The bounded frame queue — the shared heart of the capture layer.
 *
 * Every capture backend (DXGI Desktop Duplication, ScreenCaptureKit,
 * PipeWire, X11, and the synthetic device the tests use) delivers frames
 * on *its own* thread and the engine consumes them on *its* tick thread.
 * Between those two there has to be a buffer, and that buffer has to be
 * bounded — an unbounded one turns a slow encoder into unbounded latency
 * instead of a dropped frame.
 *
 * So the queue is the piece that must be right, and it is deliberately
 * *portable*: it is the mechanism, and each platform backend is a thin
 * adapter over it (remote-desktop plan A2b — "establish what the portable
 * part of capture actually is ... before any platform backend is written").
 *
 * ## The drop policy is the interesting part
 *
 * When the queue is full, one frame has to go, and *which* one depends on
 * the content:
 *
 *   - **Screen content** → drop the **oldest**.  The newest frame is the
 *     truth; a stale frame of a screen that has since changed is worth
 *     nothing, and sending it wastes the user's bandwidth.
 *   - **Camera** → drop the **newest**.  A slightly stale frame beats a
 *     frame that arrives too late to be displayed — for a camera, latency
 *     is more visible than staleness.
 *
 * The policy is a configuration, not a branch in the code that picks it.
 *
 * ## Thread safety
 *
 * `push()` is called from the capture thread; `pop()` from the engine's
 * tick thread.  They may be concurrent.  `pop()` never blocks: it
 * returns "not ready" when the queue is empty, because the engine's tick
 * loop must not be parked by a capture device that went quiet.
 *
 * ## Frame ownership
 *
 * The queue *copies* the pixel bytes into a pooled buffer.  That is one
 * memcpy per frame, and it is deliberate: the capture device's own
 * buffer is recycled the moment the callback returns (that is how DXGI
 * Desktop Duplication and PipeWire work — you must copy or map before
 * returning), and the engine needs the pixels to outlive the callback.
 *
 * A zero-copy backend does *not* use this queue: it hands its surface
 * straight to the hardware encoder within the callback, and never
 * populates a CPU-visible buffer at all.  The queue exists for the CPU
 * path and for the zero-copy path's *fallback*.
 *
 * @note Remote-desktop plan A2 / A2b.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include <nimrtc/plugins/video_capture.hpp>

namespace nimrtc::video_capture {

// ---------------------------------------------------------------------------
// CaptureQueue
// ---------------------------------------------------------------------------

/**
 * @brief A bounded, thread-safe queue of captured frames.
 *
 * @tparam Depth the maximum number of frames held at once.  A depth of 1
 *         means "no queueing": the producer's newest frame replaces the
 *         one waiting.
 */
class NIMRTC_API_STABLE CaptureQueue {
public:
    /** Construct a queue holding at most @p depth frames. */
    explicit CaptureQueue(std::uint32_t depth = 2) noexcept
        : depth_(depth == 0 ? 1u : depth) {}

    // ---- Producer side (the capture thread) -------------------------------

    /** What happened to a pushed frame. */
    enum class PushResult : std::uint8_t {
        kQueued,        ///< the frame is in the queue
        kDroppedOldest, ///< the frame is in the queue, and the oldest one
                        ///< was evicted to make room
        kRejected,      ///< the frame was refused (the queue is unchanged)
    };

    /**
     * Push a frame.  Copies the pixel bytes into the queue's own storage.
     *
     * @param pixels  The pixel bytes.  May be null for a metadata-only
     *                frame (a dirty-rect notification).
     * @param size    Size of @p pixels in bytes.
     * @param stride  Row stride in bytes.
     * @param width   Frame width in pixels.
     * @param height  Frame height in pixels.
     * @param ts      Capture timestamp, microseconds.
     * @param seq     Frame sequence number.
     * @param dirty   The changed region(s) of this frame.
     * @return What happened to the frame.  `kDroppedOldest` means the
     *         frame itself is queued but an older one was lost — the
     *         caller must count that as a drop.
     */
    PushResult push(const std::uint8_t* pixels,
                    std::size_t size,
                    std::int32_t stride,
                    std::uint32_t width,
                    std::uint32_t height,
                    std::int64_t ts,
                    std::uint32_t seq,
                    const plugins::CaptureDirtyRects& dirty) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);

        PushResult result = PushResult::kQueued;

        if (frames_.size() >= depth_) {
            // The queue is full -- apply the drop policy.
            switch (policy_) {
                case plugins::CaptureDropPolicy::kDropOldest:
                    // The oldest frame is the least useful: for screen
                    // content it is out of date, and for a camera it is
                    // merely stale.  Either way the newest frame is the
                    // one worth keeping.
                    frames_.pop_front();
                    ++dropped_;
                    result = PushResult::kDroppedOldest;
                    break;

                case plugins::CaptureDropPolicy::kDropNewest:
                    // Keep what is already queued and refuse the arrival.
                    ++dropped_;
                    return PushResult::kRejected;

                case plugins::CaptureDropPolicy::kDropIncoming:
                    // Same as kDropNewest in effect, but the distinction
                    // matters to the caller: kDropIncoming means "never
                    // replace a queued frame", kDropNewest means "the
                    // queue contents are the newest ones we have".
                    ++dropped_;
                    return PushResult::kRejected;
            }
        }

        Frame f;
        f.width   = width;
        f.height  = height;
        f.stride  = stride;
        f.ts      = ts;
        f.seq     = seq;
        f.dirty   = dirty;
        if (pixels != nullptr && size > 0) {
            f.pixels.resize(size);
            std::memcpy(f.pixels.data(), pixels, size);
        }
        frames_.push_back(std::move(f));
        ++pushed_;
        return result;
    }

    /** The policy applied when the queue is full. */
    void set_drop_policy(plugins::CaptureDropPolicy p) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        policy_ = p;
    }

    /** Resize the queue. Shrinking discards the frames that no longer
     *  fit, keeping the *newest* ones (the same rule the overflow
     *  policy applies). */
    void set_depth(std::uint32_t depth) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        depth_ = depth == 0 ? 1u : depth;
        while (frames_.size() > depth_) {
            frames_.pop_front();
            ++dropped_;
        }
    }

    [[nodiscard]] plugins::CaptureDropPolicy drop_policy() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return policy_;
    }

    // ---- Consumer side (the engine tick thread) ---------------------------

    /**
     * Pop the oldest queued frame.
     *
     * The returned frame's `pixels` stay valid until the *next* call to
     * `pop()` — the popped frame is kept alive inside the queue (in
     * `popped_`) rather than in a local that dies with this call, so the
     * consumer can read the pixels after `pop()` returns without copying.
     *
     * @return true when a frame was popped.
     */
    bool pop(plugins::CaptureFrame& out) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frames_.empty()) return false;

        // Move the frame *out* of the deque but *into* the queue's own
        // storage, so the pixel buffer outlives this call.  Assigning
        // over the previous `popped_` releases the previous frame's
        // buffer, which is exactly when the contract says the previous
        // pointer stops being valid.
        popped_ = std::move(frames_.front());
        frames_.pop_front();

        out.pixels = popped_.pixels.empty() ? nullptr : popped_.pixels.data();
        out.size   = popped_.pixels.size();
        out.stride = popped_.stride;
        out.width  = popped_.width;
        out.height = popped_.height;
        out.capture_ts_us = popped_.ts;
        out.frame_seq     = popped_.seq;
        out.dirty         = std::move(popped_.dirty);
        out.has_gpu_handle = false;
        return true;
    }

    /** Discard every queued frame. */
    void clear() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        frames_.clear();
    }

    /**
     * Look at the oldest queued frame *without* removing it.
     *
     * Used by the delivery callback: the callback must see exactly the
     * bytes the consumer will later pop, not a different copy of the
     * same frame.
     *
     * @return true when a frame was available.  The pixels are valid
     *         until the next `push()` or `pop()`.
     */
    bool peek(plugins::CaptureFrame& out) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frames_.empty()) return false;
        const Frame& f = frames_.front();
        out.pixels = f.pixels.empty() ? nullptr : f.pixels.data();
        out.size   = f.pixels.size();
        out.stride = f.stride;
        out.width  = f.width;
        out.height = f.height;
        out.capture_ts_us = f.ts;
        out.frame_seq     = f.seq;
        out.dirty         = f.dirty;
        out.has_gpu_handle = false;
        return true;
    }

    // ---- Counters ----------------------------------------------------------

    [[nodiscard]] std::uint64_t pushed() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return pushed_;
    }

    [[nodiscard]] std::uint64_t dropped() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return frames_.size();
    }

    [[nodiscard]] std::uint32_t depth() const noexcept { return depth_; }

private:
    struct Frame {
        std::vector<std::uint8_t> pixels;
        std::int32_t stride = 0;
        std::uint32_t width  = 0;
        std::uint32_t height = 0;
        std::int64_t  ts     = 0;
        std::uint32_t seq    = 0;
        plugins::CaptureDirtyRects dirty{};
    };

    mutable std::mutex mutex_;
    std::deque<Frame> frames_;
    /** The most recently popped frame.  It keeps the popped pixel buffer
     *  alive so the pointer handed to the consumer stays valid until the
     *  next `pop()`. */
    Frame popped_;
    std::uint32_t depth_ = 2;
    plugins::CaptureDropPolicy policy_ = plugins::CaptureDropPolicy::kDropOldest;
    std::uint64_t pushed_ = 0;
    std::uint64_t dropped_ = 0;
};

} // namespace nimrtc::video_capture
