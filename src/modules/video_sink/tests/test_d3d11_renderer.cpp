/**
 * @file modules/video_sink/tests/test_d3d11_renderer.cpp
 * @brief Unit tests for the Windows D3D11 / DXGI presentation sink.
 *
 * Remote-desktop plan E2 (Windows renderer) + E8 (back-pressure).
 *
 * ## What is tested here
 *
 *   1. **The factory is registered** under the id `"d3d11"`, so a
 *      configuration that names it resolves through the registry.
 *   2. **The presentation clock paces the presenter.**  The renderer runs
 *      its presenter thread with a *deadline* wait, so a target of
 *      N frames per second yields N presents in about a second — not
 *      N presents in a burst followed by a spin.
 *   3. **The back-pressure policy is latest-wins and bounded.**  When the
 *      producer pushes faster than the presenter consumes, the *older*
 *      frame is dropped and the drop is counted — the mailbox never grows
 *      past one frame, so the memory is bounded by construction.
 *   4. **The render call never blocks.**  `render()` hands the frame to
 *      the presenter and returns; the engine's tick thread is not parked
 *      by the GPU.
 *
 * ## The offscreen mode
 *
 * A swap chain needs a window, and a CI machine has none.  The renderer
 * therefore supports an **offscreen** mode: it renders into a plain
 * texture and *still* runs the presentation clock.  That is what makes
 * the frame-rate and drop-accounting assertions testable without a
 * display — which is exactly what plan E2's acceptance criterion needs.
 *
 * ## The no-GPU case
 *
 * `open()` reports `kErrHardwareError` on a machine with no Direct3D 11
 * device (a headless CI container, a VM without a GPU).  The renderer
 * reports that honestly rather than pretending to present, so the tests
 * that need a device are skipped in that case — they are *not* failed,
 * because "this machine has no GPU" is not a renderer bug.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/plugins/video_sink.hpp>
#include <nimrtc/video_sink/d3d11_renderer.hpp>

namespace {

namespace np = nimrtc::plugins;

/** A packed BGRA picture (the format the desktop compositors produce).
 *
 *  The pixels have to be *real*: the sink's CPU path copies them into
 *  the upload texture, and a frame with a null `plane_y` is refused
 *  (it is a programming error, not a frame). */
class BgraFrame {
public:
    BgraFrame(std::uint32_t w, std::uint32_t h, std::int64_t capture_ts_us)
        : w_(w), h_(h), pixels_(static_cast<std::size_t>(w) * h * 4u, 0x20u) {
        frame_.width  = w;
        frame_.height = h;
        frame_.format = np::VideoPixelFormat::kBGRA;
        frame_.stride_y = static_cast<std::int32_t>(w * 4u);
        frame_.capture_ts_us = static_cast<np::TimestampUs>(capture_ts_us);
        frame_.frame_seq = static_cast<std::uint32_t>(capture_ts_us / 1000);
        frame_.buffer_size = pixels_.size();
        frame_.plane_y = pixels_.data();
    }

    /** The frame descriptor the sink consumes. */
    const np::VideoSourceFrame& get() const noexcept { return frame_; }

private:
    std::uint32_t w_;
    std::uint32_t h_;
    std::vector<std::uint8_t> pixels_;
    np::VideoSourceFrame frame_;
};

/** Sleep for @p ms milliseconds. */
void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ---------------------------------------------------------------------------
// The factory / the registry
// ---------------------------------------------------------------------------

TEST(D3D11RendererFactory, HasTheExpectedId) {
    nimrtc::video_sink::D3D11RendererFactory fac;
    EXPECT_EQ(fac.id(), "d3d11");
    EXPECT_FALSE(fac.display_name().empty());
}

TEST(D3D11RendererFactory, CreatesASink) {
    nimrtc::video_sink::D3D11RendererFactory fac;
    std::unique_ptr<np::IVideoSink> sink(fac.create(np::VideoSinkConfig{}));
    ASSERT_NE(sink, nullptr);
    EXPECT_TRUE(sink->supports_cpu_render());
    EXPECT_TRUE(sink->supports_gpu_render());
    EXPECT_TRUE(sink->is_hardware_accelerated());
    EXPECT_EQ(sink->hardware_backend(), "d3d11");
}

TEST(D3D11RendererRegistration, IsRegistered) {
    nimrtc::video_sink::register_default_plugins();
    const auto* f = nimrtc::core::PluginRegistry::instance().get_video_sink("d3d11");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->id(), "d3d11");
}

// ---------------------------------------------------------------------------
// The presentation clock
// ---------------------------------------------------------------------------

// The presenter paces itself to `target_fps`.  At 20 fps a window of
// ~600 ms must yield roughly 12 presents — the tolerance is wide
// because the measurement includes the presenter's wake-up jitter and
// the machine's scheduling.
TEST(D3D11Renderer, PresentationClockPacesThePresenter) {
    np::VideoSinkConfig cfg{};
    cfg.offscreen = true;
    cfg.width  = 64;
    cfg.height = 64;
    cfg.target_fps = 20;

    nimrtc::video_sink::D3D11Renderer renderer(cfg);
    const np::Status rc = renderer.open();
    if (rc == np::kErrHardwareError || rc == np::kErrUnsupported) {
        GTEST_SKIP() << "no Direct3D 11 device on this machine";
    }
    ASSERT_EQ(rc, np::kOk);

    const auto start = std::chrono::steady_clock::now();
    const auto window = std::chrono::milliseconds(600);

    // Push frames until the window closes.  The presenter consumes them
    // at its own rate, so the *accepted* count grows faster than the
    // *presented* count — the difference is the mailbox's back-pressure
    // drops, and it is exactly what the "bounded memory" property is
    // made of.
    std::uint64_t pushed = 0;
    while (std::chrono::steady_clock::now() - start < window) {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count();
        const BgraFrame f(64, 64, elapsed);
        renderer.render(f.get());
        ++pushed;
        sleep_ms(1);
    }

    // Give the presenter a moment to drain what it already holds.
    sleep_ms(120);

    const np::VideoSinkStats st = renderer.stats();
    renderer.close();

    // The presenter must have presented *something* — the clock works.
    EXPECT_GT(st.frames_presented, 0u);
    // ... and it must not have presented *more* than the clock allows.
    // (A spin-wait presenter would present every pushed frame.)
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    const std::uint64_t budget = static_cast<std::uint64_t>(elapsed_ms) * 20u / 1000u + 4u;
    EXPECT_LE(st.frames_presented, budget);
}

// ---------------------------------------------------------------------------
// The back-pressure policy (plan E8)
// ---------------------------------------------------------------------------

// The mailbox is a *single slot* with a latest-wins policy: a frame
// that arrives while another is still waiting replaces it.  The replaced
// frame is counted as a drop.
//
// The test drives the producer *much* faster than the presenter (a
// 1 fps target, so the presenter consumes one frame per second) and
// asserts that (a) the drop counter moved and (b) the mailbox never
// held more than one frame.
TEST(D3D11Renderer, LatestWinsBackPressureCountsDrops) {
    np::VideoSinkConfig cfg{};
    cfg.offscreen = true;
    cfg.width  = 64;
    cfg.height = 64;
    cfg.target_fps = 1;   // deliberately far slower than the producer

    nimrtc::video_sink::D3D11Renderer renderer(cfg);
    const np::Status rc = renderer.open();
    if (rc == np::kErrHardwareError || rc == np::kErrUnsupported) {
        GTEST_SKIP() << "no Direct3D 11 device on this machine";
    }
    ASSERT_EQ(rc, np::kOk);

    // 40 frames pushed back-to-back: the presenter (at 1 fps) cannot
    // keep up, so the mailbox replaces its occupant over and over.
    for (std::uint64_t i = 0; i < 40u; ++i) {
        const BgraFrame f(64, 64, static_cast<std::int64_t>(i) * 1000);
        renderer.render(f.get());
    }

    const np::VideoSinkStats st = renderer.stats();
    renderer.close();

    EXPECT_EQ(st.frames_rendered, 40u);
    EXPECT_GT(st.frames_dropped_present, 0u);
    // The memory bound is *structural*: the mailbox holds one frame.
    EXPECT_LE(st.presenter_queue_depth, 1u);
}

// ---------------------------------------------------------------------------
// The render call never blocks
// ---------------------------------------------------------------------------

// The engine's tick thread hands the frame over and returns.  The
// presenter's GPU work happens on the sink's own thread (RD-6: a device
// plug-in owns its threads; the engine stays a single-threaded
// orchestrator).
TEST(D3D11Renderer, RenderDoesNotBlock) {
    np::VideoSinkConfig cfg{};
    cfg.offscreen = true;
    cfg.width  = 64;
    cfg.height = 64;
    cfg.target_fps = 30;

    nimrtc::video_sink::D3D11Renderer renderer(cfg);
    const np::Status rc = renderer.open();
    if (rc == np::kErrHardwareError || rc == np::kErrUnsupported) {
        GTEST_SKIP() << "no Direct3D 11 device on this machine";
    }
    ASSERT_EQ(rc, np::kOk);

    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < 20u; ++i) {
        const BgraFrame f(64, 64, static_cast<std::int64_t>(i) * 1000);
        renderer.render(f.get());
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    renderer.close();

    // 20 hand-offs must not take anywhere near a frame interval of GPU
    // time each; a blocking render would take at least 20 / 30 s.
    EXPECT_LT(elapsed, 1000);
}

// ---------------------------------------------------------------------------
// The GPU path
// ---------------------------------------------------------------------------

// A GPU surface that is not a D3D11 texture is refused rather than
// misinterpreted.
TEST(D3D11Renderer, RejectsAForeignGpuBackend) {
    np::VideoSinkConfig cfg{};
    cfg.offscreen = true;

    nimrtc::video_sink::D3D11Renderer renderer(cfg);
    const np::Status rc = renderer.open();
    if (rc == np::kErrHardwareError || rc == np::kErrUnsupported) {
        GTEST_SKIP() << "no Direct3D 11 device on this machine";
    }
    ASSERT_EQ(rc, np::kOk);

    np::GpuHandle h{};
    h.backend = np::GpuBackend::kAppleCFPixelBuffer;
    h.handle  = reinterpret_cast<void*>(0x1234);
    np::GpuBuffer surface(h, 64, 64, np::VideoPixelFormat::kBGRA);

    EXPECT_EQ(renderer.render(surface), np::kErrUnsupported);
    renderer.close();
}

// A frame with no dimensions is a programming error, not a frame.
TEST(D3D11Renderer, RejectsAnEmptyFrame) {
    np::VideoSinkConfig cfg{};
    cfg.offscreen = true;

    nimrtc::video_sink::D3D11Renderer renderer(cfg);
    const np::Status rc = renderer.open();
    if (rc == np::kErrHardwareError || rc == np::kErrUnsupported) {
        GTEST_SKIP() << "no Direct3D 11 device on this machine";
    }
    ASSERT_EQ(rc, np::kOk);

    np::VideoSourceFrame empty{};
    EXPECT_EQ(renderer.render(empty), np::kErrInvalidParam);
    renderer.close();
}

} // namespace
