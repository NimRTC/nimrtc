/**
 * @file nimrtc/video_sink/presenter_core.hpp
 * @brief The portable presentation core (remote-desktop plan E2/E3/E4/E8).
 *
 * ## What this is
 *
 * The **portable half** of a presentation sink.  Every platform renderer
 * (the Windows DXGI swap chain, the macOS Metal / AVSampleBufferDisplayLayer
 * layer, the Linux EGL / Wayland / X11 surface) needs the *same* three
 * things:
 *
 *   1. a **single-slot latest-wins mailbox** so the producer never blocks
 *      and the memory bound is structural (plan E8),
 *   2. a **presentation clock** so the frames are paced to the display's
 *      cadence instead of being pushed as fast as the decoder produces
 *      them, and
 *   3. the **drop / present / fps accounting** that proves both of the
 *      above work.
 *
 * Those three things are pure C++ — no GPU handle, no window-system type,
 * no platform SDK.  So they live here, in one class, and are *unit-tested
 * on every platform* (Windows included) with a fake `present()` callback.
 *
 * The platform-specific half — uploading the pixels and putting them on
 * screen — is a **thin adapter** that implements the callback and calls
 * into this core.
 *
 * ## The clock
 *
 * The presenter owns a deadline that advances by exactly one frame
 * interval per presented frame.  The wait is a *timed* wait
 * (`std::condition_variable::wait_until`), so an idle presenter is parked
 * in the kernel and costs nothing — a spin-wait presenter would peg a
 * core for the whole session.
 *
 * When a present takes longer than the interval (a slow GPU, a compositor
 * hiccup) the deadline is reset to *now + interval* rather than
 * accumulating a debt: a renderer that fell behind must not "catch up"
 * by presenting a burst of frames.
 *
 * ## The mailbox
 *
 * The mailbox holds **at most one** frame.  A new frame replaces the
 * waiting one; the replaced frame is counted as a drop
 * (`VideoSinkStats::frames_dropped_present`).  That is the *latest-wins*
 * back-pressure policy of plan E8, and its memory bound is structural:
 * one frame, whatever the producer does.
 *
 * @note Remote-desktop plan E2 (Windows) / E3 (macOS) / E4 (Linux) /
 *       E7 (scale-to-fit) / E8 (back-pressure).
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <nimrtc/core/error.hpp>
#include <nimrtc/plugins/video_sink.hpp>

namespace nimrtc::video_sink {

// ---------------------------------------------------------------------------
// PresentedFrame
// ---------------------------------------------------------------------------

/**
 * @brief The picture the presenter hands to the platform adapter.
 *
 * The core is deliberately **platform-neutral**: it carries *pixels*, never
 * a GPU handle.  A Metal texture, a `wl_buffer`, an X11 `XImage` and a
 * DXGI texture are all "a rectangle of BGRA bytes" as far as the core is
 * concerned — the adapter is what turns the bytes into something the
 * window system accepts.
 */
struct NIMRTC_API_STABLE PresentedFrame {
    /** The packed BGRA8 pixels (4 bytes per pixel, B, G, R, A order). */
    std::vector<std::uint8_t> pixels;

    /** The frame width in pixels. */
    std::uint32_t width = 0;

    /** The frame height in pixels. */
    std::uint32_t height = 0;

    /** The row stride in bytes (`width * 4` for a packed frame). */
    std::uint32_t stride = 0;

    /** The frame's capture timestamp (microseconds, monotonic). */
    std::int64_t capture_ts_us = 0;

    /** The frame's timeline metadata (frame id, RTP timestamp, ...). */
    plugins::VideoFrameInfo info{};
};

// ---------------------------------------------------------------------------
// PresenterCore
// ---------------------------------------------------------------------------

/**
 * @brief The portable presenter: a latest-wins mailbox, a presentation
 *        clock and the drop / present / fps accounting.
 *
 * ## Threading
 *
 * The core owns **one** thread (the *presenter thread*).  The producer
 * calls `submit()` from whatever thread it likes; the presenter thread
 * calls the `present()` callback.  The callback therefore runs on a
 * thread the adapter owns — the engine's tick thread is never blocked by
 * a GPU (RD-6: a device plug-in owns its own threads).
 *
 * ## The callback contract
 *
 * The callback receives a **non-const** frame and returns `true` when the
 * frame reached the screen.  It is invoked from the presenter thread
 * only, so an adapter may touch its window-system / GPU objects without
 * further synchronisation — but it must not block for longer than one
 * frame interval, or the presenter falls behind (and then drops, which
 * is the designed behaviour).
 */
class NIMRTC_API_STABLE PresenterCore {
public:
    /**
     * @brief The platform adapter's present callback.
     *
     * @param frame  The frame to present.  The adapter may read it; it
     *               must not keep a reference to it past the call.
     * @return true when the frame was presented.
     */
    using PresentCallback = std::function<bool(PresentedFrame&)>;

    /**
     * @brief Construct a presenter.
     *
     * @param target_fps The presentation cadence.  0 selects the default
     *                   (30 fps).
     * @param present    The adapter's present callback.  Must not be empty.
     */
    PresenterCore(std::uint32_t target_fps, PresentCallback present);

    ~PresenterCore();

    PresenterCore(const PresenterCore&)            = delete;
    PresenterCore& operator=(const PresenterCore&) = delete;

    /**
     * @brief Start the presenter thread.
     *
     * Idempotent — a second call while the presenter runs does nothing.
     */
    void start() noexcept;

    /**
     * @brief Stop the presenter thread and join it.
     *
     * Idempotent.  A frame still waiting in the mailbox is dropped (it was
     * never presented).
     */
    void stop() noexcept;

    /**
     * @brief Hand a frame to the presenter.
     *
     * The producer side of the mailbox.  If a frame is already waiting it
     * is **replaced** — the older frame is dropped and the drop is
     * counted.  The call never blocks on the presenter.
     *
     * @param frame  The frame to present.
     * @return `kOk` when the frame was accepted; `kErrNotReady` when the
     *         presenter is not running; `kErrInvalidParam` when the frame
     *         has no pixels or no dimensions.
     */
    plugins::Status submit(PresentedFrame frame) noexcept;

    /** A snapshot of the counters. */
    plugins::VideoSinkStats stats() const noexcept;

    /** The presentation cadence in frames per second. */
    std::uint32_t target_fps() const noexcept;

    /** True iff the presenter thread is running. */
    bool running() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nimrtc::video_sink
