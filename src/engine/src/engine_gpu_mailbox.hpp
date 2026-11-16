/**
 * @file src/engine/src/engine_gpu_mailbox.hpp
 * @brief PRIVATE engine collaborator: the capture → engine GPU-frame
 *        mailbox (remote-desktop plan B10).
 *
 * ## What this is
 *
 * The zero-copy capture seam delivers a GPU surface on the **capture
 * device's own thread** (`plugins::IVideoCapture::
 * set_gpu_frame_callback`).  The codec, on the other hand, may only be
 * touched from the **engine's tick thread** — that is RD-6, and the
 * test that asserts it is the acceptance criterion of plan item A10.
 *
 * So the surface has to cross a thread boundary.  This mailbox is that
 * boundary: a small, bounded, single-slot hand-off.
 *
 * ## Why a *single* slot and not a queue
 *
 * A queue that copies the pixels is exactly what the zero-copy path
 * exists to avoid, and a queue that *does not* copy them would have to
 * hold several surfaces — which means holding several textures, which
 * means the producer has to keep them alive.  The screen-content
 * semantics say something simpler and stronger: **the newest frame is
 * the truth, and a stale frame is worth nothing** (the same reason
 * `CaptureDropPolicy::kDropOldest` is the default).
 *
 * So the mailbox holds **one** pending frame.  If a new frame arrives
 * while one is pending, the pending one is dropped and the new one
 * takes its place.  The producer never blocks, and the consumer (the
 * tick thread) always sees the most recent picture.
 *
 * ## Why it is not a copy
 *
 * The frame carries a `shared_ptr<GpuBuffer>` — a *reference*, not a
 * copy of the pixels.  The producer's texture pool knows that the
 * consumer may still hold the buffer, so the texture is not recycled
 * underneath it (see `GpuTexturePool::mark_busy`).
 *
 * ## Thread safety
 *
 * `push()` runs on the capture thread; `pop()` runs on the tick thread.
 * The mutex is the only synchronisation, and it is held for the
 * duration of a pointer swap — never across an encode, and never
 * across a callback into the host.
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * @see engine_video_pump.cpp — the pump that drains the mailbox.
 * @see docs/plan/remote-desktop-plan.md §Track B (B10).
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

#include <nimrtc/plugins/video_capture.hpp>

namespace nimrtc::engine::detail {

/**
 * @brief A bounded, single-slot hand-off for GPU-captured frames.
 *
 * The producer (the capture device's thread) pushes; the consumer (the
 * engine's tick thread) pops.
 */
class GpuFrameMailbox {
public:
    GpuFrameMailbox() = default;

    GpuFrameMailbox(const GpuFrameMailbox&)            = delete;
    GpuFrameMailbox& operator=(const GpuFrameMailbox&) = delete;

    /**
     * @brief Hand a frame over to the tick thread.
     *
     * Never blocks.  If a frame is already pending, it is **dropped**
     * (the newest frame is the truth — see the header comment) and
     * the counter moves.
     *
     * @return true iff the frame was stored; false iff it replaced a
     *         pending frame (which was then dropped).
     */
    bool push(plugins::GpuCaptureFrame&& frame) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool replaced = pending_.has_value();
        if (replaced) {
            ++dropped_;
        }
        pending_.emplace(std::move(frame));
        ++pushed_;
        return !replaced;
    }

    /**
     * @brief Take the pending frame, if there is one.
     *
     * @return the frame, or `std::nullopt` when nothing is pending.
     */
    std::optional<plugins::GpuCaptureFrame> pop() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_) return std::nullopt;
        plugins::GpuCaptureFrame out = std::move(*pending_);
        pending_.reset();
        ++taken_;
        return out;
    }

    /** True iff a frame is waiting. */
    [[nodiscard]] bool pending() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_.has_value();
    }

    /** Drop the pending frame (if any).  Used on teardown. */
    void clear() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.reset();
    }

    /** Frames handed to the mailbox. */
    [[nodiscard]] std::uint64_t pushed() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return pushed_;
    }

    /** Frames dropped because another one was already pending. */
    [[nodiscard]] std::uint64_t dropped() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

    /** Frames the tick thread took out of the mailbox. */
    [[nodiscard]] std::uint64_t taken() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return taken_;
    }

private:
    mutable std::mutex mutex_;
    std::optional<plugins::GpuCaptureFrame> pending_;
    std::uint64_t pushed_ = 0;
    std::uint64_t dropped_ = 0;
    std::uint64_t taken_ = 0;
};

}  // namespace nimrtc::engine::detail
