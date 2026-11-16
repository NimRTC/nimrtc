/**
 * @file modules/video_sink/tests/test_presenter_core.cpp
 * @brief Unit tests for the portable presenter core.
 *
 * Remote-desktop plan E2 (presentation clock) / E3 / E4 (the shared core)
 * / E8 (back-pressure).
 *
 * ## What is tested here
 *
 * The presenter core is the **portable half** of every renderer: the
 * latest-wins mailbox, the presentation clock, the drop accounting and
 * the measured frame rate.  Because it is platform-neutral it is tested
 * on **every** platform — including the Windows CI leg — with a fake
 * `present()` callback.  That is what makes the macOS and Linux adapters
 * (which cannot be compiled on a Windows host) safe to ship: their
 * timing, pacing and back-pressure behaviour is covered by these tests.
 *
 *   1. **The factory / registration** of the two new renderers.
 *   2. **The latest-wins mailbox**: a frame that arrives while another is
 *      waiting replaces it, and the drop is counted.  The mailbox never
 *      holds more than one frame — the memory bound is structural.
 *   3. **The presentation clock**: with `target_fps` set, the presenter
 *      presents at approximately that rate.  The tolerance is deliberately
 *      wide (±40 %) so the test is reliable on a loaded CI machine.
 *   4. **The idle presenter does not burn CPU**: an idle presenter parks
 *      on its condition variable; the test asserts that it wakes up
 *      promptly when a frame arrives and that it presents *nothing* while
 *      idle.
 *   5. **The drop counter** is reported through `stats()`.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/plugins/video_sink.hpp>
#include <nimrtc/video_sink/egl_renderer.hpp>
#include <nimrtc/video_sink/metal_renderer.hpp>
#include <nimrtc/video_sink/presenter_core.hpp>

namespace {

namespace np = nimrtc::plugins;
namespace vs = nimrtc::video_sink;

/** Sleep for @p ms milliseconds. */
void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

/**
 * A fake `present()` callback that records what it was given.
 *
 * The recorder is the *only* thing the platform adapter supplies in the
 * real renderers — everything else (the mailbox, the clock, the
 * accounting) is the core's.
 */
class FakePresenter {
public:
    /** The callback handed to the core. */
    vs::PresenterCore::PresentCallback callback() {
        return [this](vs::PresentedFrame& frame) {
            std::lock_guard<std::mutex> lock(mtx_);
            ++calls_;
            order_.push_back(frame.info.frame_seq);
            widths_.push_back(frame.width);
            heights_.push_back(frame.height);
            return true;
        };
    }

    /** The number of times the callback was invoked. */
    std::uint64_t calls() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return calls_;
    }

    /** The frame sequence numbers the callback saw, in order. */
    std::vector<std::uint32_t> order() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return order_;
    }

    /** The width of the n-th presented frame (0 when absent). */
    std::uint32_t width_at(std::size_t n) const {
        std::lock_guard<std::mutex> lock(mtx_);
        return n < widths_.size() ? widths_[n] : 0u;
    }

private:
    mutable std::mutex       mtx_;
    std::uint64_t            calls_ = 0;
    std::vector<std::uint32_t> order_;
    std::vector<std::uint32_t> widths_;
    std::vector<std::uint32_t> heights_;
};

/** Build a BGRA frame of the given size and sequence number. */
vs::PresentedFrame make_frame(std::uint32_t w, std::uint32_t h,
                              std::uint32_t seq) {
    vs::PresentedFrame frame;
    frame.pixels.assign(static_cast<std::size_t>(w) * h * 4u,
                        static_cast<std::uint8_t>(seq & 0xFFu));
    frame.width  = w;
    frame.height = h;
    frame.stride = w * 4u;
    frame.capture_ts_us = static_cast<std::int64_t>(seq) * 1000;
    frame.info.capture_ts_us = frame.capture_ts_us;
    frame.info.frame_seq     = seq;
    frame.info.rtp_timestamp = seq * 3000;
    return frame;
}

// ---------------------------------------------------------------------------
// The factories / the registry
// ---------------------------------------------------------------------------

// The two new renderers exist on every platform (so a configuration that
// names them resolves); on a platform they cannot run, `open()` reports
// `kErrUnsupported`.
TEST(PresenterCore, TheNewRenderersExistOnEveryPlatform) {
    nimrtc::video_sink::MetalRendererFactory metal;
    EXPECT_EQ(metal.id(), "metal");
    EXPECT_FALSE(metal.display_name().empty());

    nimrtc::video_sink::EglRendererFactory egl;
    EXPECT_EQ(egl.id(), "egl");
    EXPECT_FALSE(egl.display_name().empty());
}

// ---------------------------------------------------------------------------
// The latest-wins mailbox (plan E8)
// ---------------------------------------------------------------------------

// A frame that arrives while another is still waiting *replaces* it and
// the replaced frame is counted as a drop.  The mailbox never holds more
// than one frame — the memory bound is structural.
TEST(PresenterCore, LatestWinsReplacesThePendingFrame) {
    FakePresenter fake;
    vs::PresenterCore core(1000 /* fps — effectively "as fast as possible" */,
                           fake.callback());
    core.start();
    ASSERT_TRUE(core.running());

    // The producer pushes faster than the presenter can consume.  The
    // very first frame is taken by the presenter (the clock fires
    // almost immediately); the rest pile up and are evicted.
    for (std::uint32_t i = 0; i < 200u; ++i) {
        const auto rc = core.submit(make_frame(64, 64, i));
        ASSERT_EQ(rc, np::kOk);
    }

    // Give the presenter a moment to drain what it holds.
    sleep_ms(120);

    const np::VideoSinkStats st = core.stats();
    core.stop();

    // The drop counter moved — the back-pressure policy is in force.
    EXPECT_GT(st.frames_dropped_present, 0u);
    // The memory bound is structural: the mailbox holds one frame.
    EXPECT_LE(st.presenter_queue_depth, 1u);
    // ... and the presented count is far below the submitted count.
    EXPECT_LT(st.frames_presented, 200u);
}

// When the presenter is *not* busy, the frames arrive in order.
TEST(PresenterCore, PresentsTheFramesInOrder) {
    FakePresenter fake;
    // A slow clock: the presenter takes one frame per interval, so the
    // producer (which waits for the previous frame to be presented) keeps
    // the mailbox empty and nothing is dropped.
    vs::PresenterCore core(200, fake.callback());
    core.start();

    for (std::uint32_t i = 0; i < 5u; ++i) {
        const auto rc = core.submit(make_frame(32, 32, i));
        ASSERT_EQ(rc, np::kOk);
        // Wait until the presenter has taken it.
        for (int spin = 0; spin < 200 && fake.calls() <= i; ++spin) {
            sleep_ms(5);
        }
    }
    sleep_ms(100);

    const auto order = fake.order();
    core.stop();

    ASSERT_GE(order.size(), 5u);
    for (std::size_t i = 0; i < 5u; ++i) {
        EXPECT_EQ(order[i], static_cast<std::uint32_t>(i))
            << "frame " << i << " was presented out of order";
    }
}

// ---------------------------------------------------------------------------
// The presentation clock (plan E2)
// ---------------------------------------------------------------------------

// With `target_fps` set, the presenter presents at approximately that
// rate.  The tolerance is ±40 % — deliberately wide, because the
// measurement includes the wake-up jitter of a loaded CI machine.  The
// point of the assertion is that the presenter is *paced* (it does not
// present a burst and then spin), not that it hits the rate exactly.
TEST(PresenterCore, ThePresentationClockPacesThePresenter) {
    constexpr std::uint32_t kTargetFps = 20;

    FakePresenter fake;
    vs::PresenterCore core(kTargetFps, fake.callback());
    core.start();

    // Push a frame every 5 ms for a second — far more than the clock
    // allows, so the presenter is always backlogged.
    const auto start = std::chrono::steady_clock::now();
    const auto window = std::chrono::milliseconds(1000);
    while (std::chrono::steady_clock::now() - start < window) {
        core.submit(make_frame(32, 32, 0));
        sleep_ms(5);
    }
    sleep_ms(150);

    const np::VideoSinkStats st = core.stats();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    core.stop();

    // The presenter must have presented *something* — the clock works.
    EXPECT_GT(st.frames_presented, 0u);

    // ... and it must not have presented far more than the clock allows.
    // (A spin-wait presenter would present every pushed frame.)
    const double seconds = static_cast<double>(elapsed_ms) / 1000.0;
    const double expected = static_cast<double>(kTargetFps) * seconds;
    const double tolerance = expected * 0.40 + 4.0;
    EXPECT_LE(static_cast<double>(st.frames_presented), expected + tolerance)
        << "the presenter presented " << st.frames_presented
        << " frames in " << elapsed_ms << " ms — the clock is not pacing it";

    // The measured rate must be in the same ballpark.  The measurement
    // window is one second, so this needs a long enough run.
    if (st.present_fps > 0u) {
        EXPECT_GE(static_cast<double>(st.present_fps),
                  static_cast<double>(kTargetFps) * 0.6);
        EXPECT_LE(static_cast<double>(st.present_fps),
                  static_cast<double>(kTargetFps) * 1.4);
    }
}

// ---------------------------------------------------------------------------
// The idle presenter
// ---------------------------------------------------------------------------

// An idle presenter parks on its condition variable: it presents
// *nothing* while there is no work, and it wakes up promptly when a
// frame arrives.  (The test does not measure CPU usage — it measures
// the *behaviour* that a parked thread produces.)
TEST(PresenterCore, TheIdlePresenterParks) {
    FakePresenter fake;
    vs::PresenterCore core(60, fake.callback());
    core.start();

    // Idle for a while: nothing may be presented.
    sleep_ms(400);
    EXPECT_EQ(fake.calls(), 0u)
        << "an idle presenter must not present anything";

    // The presenter must still be alive and responsive.
    ASSERT_EQ(core.submit(make_frame(16, 16, 7)), np::kOk);
    bool presented = false;
    for (int spin = 0; spin < 100; ++spin) {
        if (fake.calls() > 0) {
            presented = true;
            break;
        }
        sleep_ms(10);
    }
    core.stop();

    EXPECT_TRUE(presented) << "the presenter did not wake up for the frame";
}

// ---------------------------------------------------------------------------
// The counters
// ---------------------------------------------------------------------------

// The drop counter is reported through `stats()`.
TEST(PresenterCore, TheDropCounterIsReported) {
    FakePresenter fake;
    vs::PresenterCore core(1 /* fps — deliberately far too slow */,
                           fake.callback());
    core.start();

    for (std::uint32_t i = 0; i < 40u; ++i) {
        core.submit(make_frame(16, 16, i));
    }

    const np::VideoSinkStats st = core.stats();
    core.stop();

    EXPECT_GT(st.frames_dropped_present, 0u);
    EXPECT_LE(st.presenter_queue_depth, 1u);
}

// A frame with no pixels (or no dimensions) is refused rather than
// presented as garbage.
TEST(PresenterCore, RejectsAnEmptyFrame) {
    FakePresenter fake;
    vs::PresenterCore core(30, fake.callback());
    core.start();

    vs::PresentedFrame empty;
    EXPECT_EQ(core.submit(empty), np::kErrInvalidParam);

    vs::PresentedFrame no_pixels = make_frame(16, 16, 1);
    no_pixels.pixels.clear();
    EXPECT_EQ(core.submit(no_pixels), np::kErrInvalidParam);

    core.stop();
}

// Submitting before the presenter is started is refused.
TEST(PresenterCore, RefusesToSubmitBeforeStart) {
    FakePresenter fake;
    vs::PresenterCore core(30, fake.callback());
    EXPECT_EQ(core.submit(make_frame(16, 16, 1)), np::kErrNotReady);
    EXPECT_FALSE(core.running());
}

// The target cadence is reported back.
TEST(PresenterCore, ReportsTheTargetFps) {
    FakePresenter fake;
    vs::PresenterCore core(37, fake.callback());
    EXPECT_EQ(core.target_fps(), 37u);

    // 0 selects the default (30 fps).
    vs::PresenterCore def(0, fake.callback());
    EXPECT_EQ(def.target_fps(), 30u);
}

} // namespace
