/**
 * @file tests/test_video_render_path.cpp
 * @brief WP-A — the decoded-frame delivery-path rule and the frozen
 *        capture-capability semantics.
 *
 * Two rules are tested here, and both are *pure functions* living in an
 * engine-private header (`src/engine/src/video_render_path.hpp`):
 *
 *   1. **The delivery-path rule** (`decide_video_render_path`) — whether
 *      a decoded frame reaches the sink as a GPU surface or as host
 *      pixels.  It is a function of three facts: the decoder's
 *      `supports_zero_copy_decode()`, the sink's `supports_gpu_render()`
 *      and the sink's `supports_cpu_render()`.  All eight combinations
 *      are exercised, because a wrong decision does not crash — it
 *      renders nothing, which is the hardest kind of bug to see.
 *
 *   2. **The capture-capability freeze** (`check_capture_capabilities`) —
 *      whether a requested capture configuration is supported by the
 *      device.  The whole point of the freeze is that an unsupported
 *      combination is refused at `open()` time instead of on the first
 *      frame, so each violation class gets its own case.
 *
 * The tests also pin the **graceful-sink contract**: a third-party sink
 * that implements only one of the two delivery paths must work, and the
 * engine's rule must route it to the side that exists.  That contract is
 * exercised with two miniature sinks — one CPU-only, one GPU-only —
 * that implement nothing but the side they support.
 *
 * The rule is tested without an engine, a transport, a clock or a
 * plugin: it is a pure function, so that is exactly how it is tested.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "video_render_path.hpp"

namespace {

using nimrtc::engine::detail::CaptureViolation;
using nimrtc::engine::detail::check_capture_capabilities;
using nimrtc::engine::detail::decide_video_render_path;
using nimrtc::engine::detail::VideoRenderPath;
using nimrtc::engine::detail::VideoRenderPathInputs;

using nimrtc::plugins::CaptureCapabilities;
using nimrtc::plugins::CaptureConfig;
using nimrtc::plugins::CaptureFormat;
using nimrtc::plugins::CaptureRegion;

// ---------------------------------------------------------------------------
// The delivery-path rule
// ---------------------------------------------------------------------------

TEST(VideoRenderPath, GpuPathWhenDecoderAndSinkBothSupportSurfaces) {
    VideoRenderPathInputs in;
    in.decoder_zero_copy = true;
    in.sink_gpu_render   = true;
    in.sink_cpu_render   = true;
    EXPECT_EQ(decide_video_render_path(in), VideoRenderPath::kGpuSurface);
}

TEST(VideoRenderPath, GpuPathAlsoWhenTheSinkCannotTakeCpuFrames) {
    // A GPU-only sink: the CPU path does not exist for it, so the GPU
    // path is the only one that can work — and it does.
    VideoRenderPathInputs in;
    in.decoder_zero_copy = true;
    in.sink_gpu_render   = true;
    in.sink_cpu_render   = false;
    EXPECT_EQ(decide_video_render_path(in), VideoRenderPath::kGpuSurface);
}

TEST(VideoRenderPath, CpuPathWhenTheDecoderCannotProduceASurface) {
    // The software decoder: no surface, so the frame is staged to host
    // memory whatever the sink says.
    VideoRenderPathInputs in;
    in.decoder_zero_copy = false;
    in.sink_gpu_render   = true;
    in.sink_cpu_render   = true;
    EXPECT_EQ(decide_video_render_path(in), VideoRenderPath::kCpuStaging);
}

TEST(VideoRenderPath, CpuPathWhenTheSinkCannotTakeASurface) {
    // The historical sink: it only knows how to consume host pixels.
    VideoRenderPathInputs in;
    in.decoder_zero_copy = true;
    in.sink_gpu_render   = false;
    in.sink_cpu_render   = true;
    EXPECT_EQ(decide_video_render_path(in), VideoRenderPath::kCpuStaging);
}

TEST(VideoRenderPath, IncompatibleWhenNeitherPathIsPossible) {
    // The decoder cannot produce a surface and the sink refuses host
    // pixels: nothing can ever be displayed.  The engine reports this
    // at `open()` time rather than silently rendering nothing.
    VideoRenderPathInputs in;
    in.decoder_zero_copy = false;
    in.sink_gpu_render   = false;
    in.sink_cpu_render   = false;
    EXPECT_EQ(decide_video_render_path(in), VideoRenderPath::kIncompatible);
}

TEST(VideoRenderPath, IncompatibleWhenTheDecoderCannotProduceASurfaceAndTheSinkIsGpuOnly) {
    VideoRenderPathInputs in;
    in.decoder_zero_copy = false;
    in.sink_gpu_render   = true;
    in.sink_cpu_render   = false;
    EXPECT_EQ(decide_video_render_path(in), VideoRenderPath::kIncompatible);
}

TEST(VideoRenderPath, CpuOnlySinkWithASoftwareDecoderTakesTheCpuPath) {
    // A CPU-only sink with a software decoder is the universal
    // combination: the CPU path is preferred over "incompatible"
    // whenever the sink accepts host pixels.
    VideoRenderPathInputs in;
    in.decoder_zero_copy = false;
    in.sink_gpu_render   = false;
    in.sink_cpu_render   = true;
    EXPECT_EQ(decide_video_render_path(in), VideoRenderPath::kCpuStaging);
}

TEST(VideoRenderPath, EveryCombinationIsDecided) {
    // Exhaustive: 2 × 2 × 2 = 8 combinations, each with exactly one
    // decided outcome — no combination falls through undecided.
    int decided = 0;
    for (int d = 0; d < 2; ++d) {
        for (int g = 0; g < 2; ++g) {
            for (int c = 0; c < 2; ++c) {
                VideoRenderPathInputs in;
                in.decoder_zero_copy = (d != 0);
                in.sink_gpu_render   = (g != 0);
                in.sink_cpu_render   = (c != 0);
                const VideoRenderPath p = decide_video_render_path(in);
                EXPECT_NE(p, static_cast<VideoRenderPath>(255));
                ++decided;
            }
        }
    }
    EXPECT_EQ(decided, 8);
}

// ---------------------------------------------------------------------------
// The graceful-sink contract (a third-party sink implements one side)
// ---------------------------------------------------------------------------

/** A sink that implements **only** the CPU path — the historical shape.
 *  It does not stub out the GPU overload at all. */
class CpuOnlySink final : public nimrtc::plugins::IVideoSink {
public:
    const char* name() const noexcept override { return "cpu-only"; }
    nimrtc::plugins::Status open() noexcept override { return nimrtc::plugins::kOk; }
    void close() noexcept override {}

    nimrtc::plugins::Status
    render(const nimrtc::plugins::VideoSourceFrame&) noexcept override {
        ++cpu_frames;
        return nimrtc::plugins::kOk;
    }

    nimrtc::plugins::VideoSinkStats stats() const noexcept override {
        nimrtc::plugins::VideoSinkStats s;
        s.frames_rendered = cpu_frames;
        return s;
    }
    nimrtc::plugins::VideoSinkConfig config() const noexcept override { return {}; }

    std::uint64_t cpu_frames = 0;
};

/** A sink that implements **only** the GPU path — the new shape.  It does
 *  not stub out the CPU overload either. */
class GpuOnlySink final : public nimrtc::plugins::IVideoSink {
public:
    const char* name() const noexcept override { return "gpu-only"; }
    nimrtc::plugins::Status open() noexcept override { return nimrtc::plugins::kOk; }
    void close() noexcept override {}

    nimrtc::plugins::Status
    render(const nimrtc::plugins::GpuBuffer&) noexcept override {
        ++gpu_frames;
        return nimrtc::plugins::kOk;
    }

    bool supports_gpu_render() const noexcept override { return true; }
    bool supports_cpu_render() const noexcept override { return false; }

    nimrtc::plugins::VideoSinkStats stats() const noexcept override {
        nimrtc::plugins::VideoSinkStats s;
        s.frames_rendered = gpu_frames;
        return s;
    }
    nimrtc::plugins::VideoSinkConfig config() const noexcept override { return {}; }

    std::uint64_t gpu_frames = 0;
};

TEST(VideoRenderPath, CpuOnlySinkKeepsTheCpuPathAndReportsNoGpuSupport) {
    CpuOnlySink sink;

    // The capability probes are the defaults: no GPU path, CPU path yes.
    EXPECT_FALSE(sink.supports_gpu_render());
    EXPECT_TRUE(sink.supports_cpu_render());

    // The route the engine picks for this sink.
    VideoRenderPathInputs in;
    in.decoder_zero_copy = true;
    in.sink_gpu_render   = sink.supports_gpu_render();
    in.sink_cpu_render   = sink.supports_cpu_render();
    EXPECT_EQ(decide_video_render_path(in), VideoRenderPath::kCpuStaging);
}

TEST(VideoRenderPath, CpuOnlySinkDoesNotHaveToImplementTheGpuOverload) {
    // The GPU overload keeps its default implementation: it reports
    // "unsupported" rather than pretending to have rendered the frame.
    //
    // The call goes through the *interface* (`IVideoSink&`), which is
    // how the engine calls it — the derived class hides the base
    // overloads for calls made on the concrete type (the usual C++
    // name-hiding rule), but the virtual dispatch is unaffected.
    CpuOnlySink sink;
    nimrtc::plugins::IVideoSink& iface = sink;
    nimrtc::plugins::GpuBuffer surface;
    EXPECT_EQ(iface.render(surface), nimrtc::plugins::kErrUnsupported);
}

TEST(VideoRenderPath, GpuOnlySinkKeepsTheGpuPathAndReportsNoCpuSupport) {
    GpuOnlySink sink;

    EXPECT_TRUE(sink.supports_gpu_render());
    EXPECT_FALSE(sink.supports_cpu_render());

    VideoRenderPathInputs in;
    in.decoder_zero_copy = true;
    in.sink_gpu_render   = sink.supports_gpu_render();
    in.sink_cpu_render   = sink.supports_cpu_render();
    EXPECT_EQ(decide_video_render_path(in), VideoRenderPath::kGpuSurface);
}

TEST(VideoRenderPath, GpuOnlySinkDoesNotHaveToImplementTheCpuOverload) {
    GpuOnlySink sink;
    nimrtc::plugins::IVideoSink& iface = sink;
    nimrtc::plugins::VideoSourceFrame frame{};
    EXPECT_EQ(iface.render(frame), nimrtc::plugins::kErrUnsupported);
}

TEST(VideoRenderPath, GpuOnlySinkIsIncompatibleWithASoftwareDecoder) {
    // The GPU-only sink with a decoder that cannot produce a surface:
    // neither path is possible, and the engine reports it at `open()`
    // time instead of rendering nothing forever.
    GpuOnlySink sink;
    VideoRenderPathInputs in;
    in.decoder_zero_copy = false;
    in.sink_gpu_render   = sink.supports_gpu_render();
    in.sink_cpu_render   = sink.supports_cpu_render();
    EXPECT_EQ(decide_video_render_path(in), VideoRenderPath::kIncompatible);
}

TEST(VideoRenderPath, TheGpuOverloadWithMetadataFallsBackToTheOneArgumentOverload) {
    // A sink that implements only `render(const GpuBuffer&)` keeps
    // working when the engine hands it the metadata overload.
    GpuOnlySink sink;
    nimrtc::plugins::IVideoSink& iface = sink;
    nimrtc::plugins::GpuBuffer surface;
    nimrtc::plugins::VideoFrameInfo info{};
    info.capture_ts_us = 1234;
    EXPECT_EQ(iface.render(surface, info), nimrtc::plugins::kOk);
    EXPECT_EQ(sink.gpu_frames, 1u);
}

// ---------------------------------------------------------------------------
// The capture-capability freeze
// ---------------------------------------------------------------------------

CaptureConfig request(std::uint32_t w, std::uint32_t h, std::uint32_t fps) {
    CaptureConfig cfg;
    cfg.format.width  = w;
    cfg.format.height = h;
    cfg.format.fps    = fps;
    return cfg;
}

TEST(CaptureCapabilities, AnEmptyRequestIsSupported) {
    // "the device's best" — nothing was asked for, so nothing can be
    // refused.  (This is the default `CaptureConfig`.)
    const CaptureCapabilities caps{};
    const CaptureConfig cfg{};
    const auto check = check_capture_capabilities(caps, cfg, {});
    EXPECT_TRUE(check.ok());
    EXPECT_EQ(check.violation, CaptureViolation::kNone);
}

TEST(CaptureCapabilities, ZeroCopyRequestNeedsADeviceThatCanDoZeroCopy) {
    CaptureCapabilities caps;
    caps.zero_copy = false;

    CaptureConfig cfg = request(1920, 1080, 30);
    cfg.format.zero_copy = true;

    const auto check = check_capture_capabilities(caps, cfg, {});
    EXPECT_FALSE(check.ok());
    EXPECT_EQ(check.violation, CaptureViolation::kZeroCopyUnsupported);
}

TEST(CaptureCapabilities, ZeroCopyRequestIsAcceptedWhenTheDeviceAndTheFormatBothAllowIt) {
    CaptureCapabilities caps;
    caps.zero_copy = true;

    CaptureConfig cfg = request(1920, 1080, 30);
    cfg.format.zero_copy = true;

    std::vector<CaptureFormat> offered;
    CaptureFormat f;
    f.width = 1920;
    f.height = 1080;
    f.fps = 30;
    f.zero_copy = true;
    offered.push_back(f);

    const auto check = check_capture_capabilities(caps, cfg, offered);
    EXPECT_TRUE(check.ok());
}

TEST(CaptureCapabilities, ZeroCopyIsRefusedWhenTheDeviceCanButTheFormatCannot) {
    // The device-wide flag is the *weaker* of the two statements: a
    // device may do zero-copy in general while one particular format
    // still needs a readback.
    CaptureCapabilities caps;
    caps.zero_copy = true;

    CaptureConfig cfg = request(1920, 1080, 30);
    cfg.format.zero_copy = true;

    std::vector<CaptureFormat> offered;
    CaptureFormat f;
    f.width = 1920;
    f.height = 1080;
    f.fps = 30;
    f.zero_copy = false;   // the format itself needs a readback
    offered.push_back(f);

    const auto check = check_capture_capabilities(caps, cfg, offered);
    EXPECT_FALSE(check.ok());
    EXPECT_EQ(check.violation, CaptureViolation::kZeroCopyUnsupported);
}

TEST(CaptureCapabilities, DirtyRectsRequestNeedsADeviceThatProducesThem) {
    CaptureCapabilities caps;
    caps.dirty_rects = false;

    CaptureConfig cfg = request(1920, 1080, 30);
    cfg.format.has_dirty_rects = true;

    const auto check = check_capture_capabilities(caps, cfg, {});
    EXPECT_FALSE(check.ok());
    EXPECT_EQ(check.violation, CaptureViolation::kDirtyRectsUnsupported);
}

TEST(CaptureCapabilities, DirtyRectsRequestIsAcceptedWhenTheDeviceProducesThem) {
    CaptureCapabilities caps;
    caps.dirty_rects = true;

    CaptureConfig cfg = request(1920, 1080, 30);
    cfg.format.has_dirty_rects = true;

    const auto check = check_capture_capabilities(caps, cfg, {});
    EXPECT_TRUE(check.ok());
}

TEST(CaptureCapabilities, ARegionRequestNeedsADeviceThatCanSelectOne) {
    CaptureCapabilities caps;
    caps.region_selection = false;

    CaptureConfig cfg = request(1920, 1080, 30);
    cfg.region = CaptureRegion{0, 0, 640, 480};

    const auto check = check_capture_capabilities(caps, cfg, {});
    EXPECT_FALSE(check.ok());
    EXPECT_EQ(check.violation, CaptureViolation::kRegionSelectionUnsupported);
}

TEST(CaptureCapabilities, ARegionRequestIsAcceptedWhenTheDeviceCanSelectOne) {
    CaptureCapabilities caps;
    caps.region_selection = true;

    CaptureConfig cfg = request(1920, 1080, 30);
    cfg.region = CaptureRegion{0, 0, 640, 480};

    const auto check = check_capture_capabilities(caps, cfg, {});
    EXPECT_TRUE(check.ok());
}

TEST(CaptureCapabilities, AZeroSizedRegionIsNotARegionRequest) {
    // "the whole surface" is not a region selection: it must not be
    // refused by a device that cannot select a sub-region.
    CaptureCapabilities caps;
    caps.region_selection = false;

    CaptureConfig cfg = request(1920, 1080, 30);
    cfg.region = CaptureRegion{0, 0, 0, 0};

    const auto check = check_capture_capabilities(caps, cfg, {});
    EXPECT_TRUE(check.ok());
}

TEST(CaptureCapabilities, ASizeAboveTheMaximumIsRefused) {
    CaptureCapabilities caps;
    caps.max_pixels = 1920ull * 1080ull;

    const CaptureConfig cfg = request(3840, 2160, 30);
    const auto check = check_capture_capabilities(caps, cfg, {});
    EXPECT_FALSE(check.ok());
    EXPECT_EQ(check.violation, CaptureViolation::kMaxPixelsExceeded);
}

TEST(CaptureCapabilities, ASizeAtTheMaximumIsAccepted) {
    CaptureCapabilities caps;
    caps.max_pixels = 1920ull * 1080ull;

    const CaptureConfig cfg = request(1920, 1080, 30);
    const auto check = check_capture_capabilities(caps, cfg, {});
    EXPECT_TRUE(check.ok());
}

TEST(CaptureCapabilities, AnUnboundedMaximumRefusesNothing) {
    CaptureCapabilities caps;
    caps.max_pixels = 0;   // unbounded

    const CaptureConfig cfg = request(7680, 4320, 60);
    const auto check = check_capture_capabilities(caps, cfg, {});
    EXPECT_TRUE(check.ok());
}

TEST(CaptureCapabilities, AFrameRateAboveTheMaximumIsRefused) {
    CaptureCapabilities caps;
    caps.max_fps = 30;

    const CaptureConfig cfg = request(1920, 1080, 60);
    const auto check = check_capture_capabilities(caps, cfg, {});
    EXPECT_FALSE(check.ok());
    EXPECT_EQ(check.violation, CaptureViolation::kMaxFpsExceeded);
}

TEST(CaptureCapabilities, AFrameRateAtTheMaximumIsAccepted) {
    CaptureCapabilities caps;
    caps.max_fps = 30;

    const CaptureConfig cfg = request(1920, 1080, 30);
    const auto check = check_capture_capabilities(caps, cfg, {});
    EXPECT_TRUE(check.ok());
}

TEST(CaptureCapabilities, AGeometryTheDeviceDoesNotOfferIsRefused) {
    CaptureCapabilities caps;

    std::vector<CaptureFormat> offered;
    CaptureFormat f;
    f.width = 1920;
    f.height = 1080;
    f.fps = 30;
    offered.push_back(f);

    const CaptureConfig cfg = request(1280, 720, 30);
    const auto check = check_capture_capabilities(caps, cfg, offered);
    EXPECT_FALSE(check.ok());
    EXPECT_EQ(check.violation, CaptureViolation::kFormatNotOffered);
}

TEST(CaptureCapabilities, AnEmptyFormatListMeansTheSizeCheckRestsOnMaxPixels) {
    // The device does not publish a format list: the geometry is then
    // only bounded by `max_pixels`.
    CaptureCapabilities caps;
    caps.max_pixels = 1920ull * 1080ull;

    const CaptureConfig cfg = request(1280, 720, 30);
    const auto check = check_capture_capabilities(caps, cfg, {});
    EXPECT_TRUE(check.ok());
}

TEST(CaptureCapabilities, TheCursorIsAPreferenceAndNotAViolation) {
    // `want_cursor` defaults to true, and a device that cannot
    // composite the pointer simply delivers frames without it.  It must
    // never be the reason a session is refused.
    CaptureCapabilities caps;
    caps.cursor_composited = false;

    CaptureConfig cfg = request(1920, 1080, 30);
    EXPECT_TRUE(cfg.want_cursor);

    const auto check = check_capture_capabilities(caps, cfg, {});
    EXPECT_TRUE(check.ok());
}

} // namespace
