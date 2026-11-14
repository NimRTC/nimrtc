/**
 * @file src/modules/video_sink/src/presenter_core.cpp
 * @brief The portable presentation core (remote-desktop plan E2/E3/E4/E8).
 *
 * See include/nimrtc/video_sink/presenter_core.hpp for the contract.
 *
 * ## The structure of this file
 *
 *   - `PresenterCore::Impl` — the mailbox, the clock, the counters and the
 *     presenter thread.
 *   - `PresenterCore` — the public face.
 *
 * This file is deliberately free of platform code: it is compiled on
 * every platform and unit-tested on every platform.  The platform adapters
 * (`d3d11_renderer.cpp`, `metal_renderer.cpp`, `egl_renderer.cpp`) supply
 * the `present()` callback and nothing else.
 */

#include <nimrtc/video_sink/presenter_core.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace nimrtc::video_sink {

namespace {

using Clock = std::chrono::steady_clock;

/** The default presentation rate when the configuration names none. */
constexpr std::uint32_t kDefaultPresentFps = 30;

/** The frame-rate measurement window (one second). */
constexpr std::int64_t kFpsWindowUs = 1'000'000;

} // namespace

// ===========================================================================
// PresenterCore::Impl
// ===========================================================================

struct PresenterCore::Impl {
    Impl(std::uint32_t fps, PresentCallback cb) noexcept
        : target_fps_(fps > 0 ? fps : kDefaultPresentFps), present_(std::move(cb)) {}

    ~Impl() { stop(); }

    // ---- The configuration -----------------------------------------------
    const std::uint32_t target_fps_;
    const PresentCallback present_;

    // ---- The counters (atomic — `stats()` may be read from any thread) --
    std::atomic<std::uint64_t> frames_presented_{0};
    std::atomic<std::uint64_t> frames_dropped_present_{0};
    std::atomic<std::uint64_t> render_errors_{0};
    std::atomic<std::uint32_t> present_fps_{0};

    // ---- The mailbox (single slot, latest-wins) ------------------------
    //
    // `slot_` holds the frame that is *waiting* to be presented.  A new
    // frame replaces it; the replaced one is counted as a drop.  The
    // producer never blocks on the presenter.
    std::mutex              mtx_;
    std::condition_variable cv_;
    PresentedFrame          slot_;
    bool                    slot_valid_ = false;
    bool                    stop_       = false;

    // ---- The presenter thread -------------------------------------------
    std::thread        presenter_;
    std::atomic<bool>  running_{false};

    /** Start the presenter thread. */
    void start() noexcept {
        if (running_.exchange(true)) return;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stop_ = false;
        }
        presenter_ = std::thread([this] { present_loop(); });
    }

    /** Stop the presenter thread and join it. */
    void stop() noexcept {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        if (presenter_.joinable()) presenter_.join();
        running_.store(false);
    }

    /** The presenter thread's body. */
    void present_loop() {
        // The presentation clock: a deadline that advances by one frame
        // interval per presented frame.  The wait is a *timed* wait — the
        // thread sleeps until the deadline, it does not spin.
        const auto interval = std::chrono::nanoseconds{
            1'000'000'000LL / static_cast<long long>(target_fps_)};
        auto next_deadline = Clock::now() + interval;

        // The frame-rate window: the presenter recomputes the rate over a
        // one-second window so the reported figure reflects what the
        // display actually got.
        std::uint64_t window_frames = 0;
        auto          window_start  = Clock::now();

        for (;;) {
            PresentedFrame frame;
            {
                std::unique_lock<std::mutex> lock(mtx_);

                // ---- The presentation clock ----------------------------
                //
                // The presenter sleeps until the deadline.  It does *not*
                // wake early when a frame arrives: waking early is what a
                // spin-wait presenter does, and it makes the frame rate
                // follow the producer instead of the clock.  The frame
                // waits in the mailbox (which is latest-wins, so the frame
                // the presenter eventually shows is the *newest* one) until
                // the deadline arrives.
                cv_.wait_until(lock, next_deadline, [this] { return stop_; });
                if (stop_) return;

                if (!slot_valid_) {
                    // The deadline arrived with nothing to show: the clock
                    // keeps running, but there is no frame to present.  The
                    // next deadline is advanced so the loop cannot spin on
                    // an expired deadline.
                    next_deadline = Clock::now() + interval;
                    continue;
                }
                // Take the frame out of the slot.  The slot becomes free,
                // so the producer can post the next one immediately.
                frame = std::move(slot_);
                slot_ = PresentedFrame{};
                slot_valid_ = false;
            }

            const bool ok = present_ ? present_(frame) : false;
            if (ok) {
                frames_presented_.fetch_add(1, std::memory_order_relaxed);
                ++window_frames;

                const auto now = Clock::now();
                const auto window_us =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        now - window_start).count();
                if (window_us >= kFpsWindowUs) {
                    const auto fps = (static_cast<double>(window_frames) * 1e6) /
                                     static_cast<double>(window_us);
                    present_fps_.store(static_cast<std::uint32_t>(fps + 0.5),
                                       std::memory_order_relaxed);
                    window_frames = 0;
                    window_start  = now;
                }
            } else {
                render_errors_.fetch_add(1, std::memory_order_relaxed);
            }

            // Advance the presentation clock.  If the present took longer
            // than the interval (a slow GPU, a vsync wait), the deadline is
            // reset to *now* rather than accumulating a debt — a renderer
            // that falls behind must not try to "catch up" by presenting a
            // burst of frames.
            const auto after = Clock::now();
            next_deadline += interval;
            if (next_deadline < after) next_deadline = after + interval;
        }
    }
};

// ===========================================================================
// PresenterCore
// ===========================================================================

PresenterCore::PresenterCore(std::uint32_t target_fps,
                             PresentCallback present)
    : impl_(std::make_unique<Impl>(target_fps, std::move(present))) {}

PresenterCore::~PresenterCore() = default;

void PresenterCore::start() noexcept {
    impl_->start();
}

void PresenterCore::stop() noexcept {
    impl_->stop();
}

plugins::Status PresenterCore::submit(PresentedFrame frame) noexcept {
    if (!impl_->running_.load(std::memory_order_acquire)) {
        return plugins::kErrNotReady;
    }
    if (frame.pixels.empty() || frame.width == 0 || frame.height == 0) {
        impl_->render_errors_.fetch_add(1, std::memory_order_relaxed);
        return plugins::kErrInvalidParam;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mtx_);
        if (impl_->slot_valid_) {
            // The previous frame has not been presented yet: it is
            // dropped in favour of the newer one.  The count is what
            // proves the back-pressure policy works.
            impl_->frames_dropped_present_.fetch_add(1, std::memory_order_relaxed);
        }
        impl_->slot_       = std::move(frame);
        impl_->slot_valid_ = true;
    }
    impl_->cv_.notify_all();
    return plugins::kOk;
}

plugins::VideoSinkStats PresenterCore::stats() const noexcept {
    plugins::VideoSinkStats s;
    s.frames_dropped_present =
        impl_->frames_dropped_present_.load(std::memory_order_relaxed);
    s.frames_presented =
        impl_->frames_presented_.load(std::memory_order_relaxed);
    s.render_errors  = impl_->render_errors_.load(std::memory_order_relaxed);
    s.present_fps    = impl_->present_fps_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(impl_->mtx_);
        s.presenter_queue_depth = impl_->slot_valid_ ? 1u : 0u;
    }
    return s;
}

std::uint32_t PresenterCore::target_fps() const noexcept {
    return impl_->target_fps_;
}

bool PresenterCore::running() const noexcept {
    return impl_->running_.load(std::memory_order_acquire);
}

} // namespace nimrtc::video_sink
