// modules/video_capture/tests/test_cursor_wiring.cpp
// ----------------------------------------------------------------------------
// The cursor layer's *wiring* (remote-desktop plan A6).
//
// The plan's A6 acceptance criterion is:
//
//   "On a Windows host the composited frame differs from the raw frame
//    in the cursor's bounding box and is identical elsewhere
//    (pixel-exact comparison outside the box)."
//
// The compositing maths is already covered by test_cursor_compositor.cpp.
// What this file covers is the part that makes the criterion *reachable
// at all*: the pointer has to be **fetched from the platform** and
// **drawn into the frame the engine actually publishes**.  A cursor
// layer that nothing installs is a layer that never runs.
//
// The tests are written so that they pass on every CI leg:
//
//   - the *decision* (install the layer only when the device does not
//     composite the pointer itself) is tested with a fake device, and
//     is therefore platform-independent;
//   - the *platform fetch* is tested against the real platform source,
//     and skips cleanly where the platform has no pointer to read.
//
// See include/nimrtc/video_capture/cursor_compositor.hpp for the seam.
// ----------------------------------------------------------------------------

#include <nimrtc/video_capture/capture_source_adapter.hpp>
#include <nimrtc/video_capture/cursor_compositor.hpp>
#include <nimrtc/video_capture/synthetic_capture.hpp>
#include <nimrtc/video_capture/win_cursor_source.hpp>
#include <nimrtc/video_capture/x11_cursor_source.hpp>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

using nimrtc::plugins::CaptureConfig;
using nimrtc::plugins::CaptureDeviceKind;
using nimrtc::plugins::CaptureFormat;
using nimrtc::plugins::VideoPixelFormat;
using nimrtc::plugins::VideoSourceConfig;
using nimrtc::plugins::VideoSourceFrame;
using nimrtc::video_capture::AlphaCursorCompositor;
using nimrtc::video_capture::CaptureSourceAdapter;
using nimrtc::video_capture::CursorState;
using nimrtc::video_capture::ICursorCompositor;
using nimrtc::video_capture::ICursorSource;
using nimrtc::video_capture::ManualCursorSource;
using nimrtc::video_capture::make_platform_cursor_source;

namespace {

// ---------------------------------------------------------------------------
// A fake device that lets a test decide what the platform does
// ---------------------------------------------------------------------------

/**
 * @brief A capture device whose capabilities the test chooses.
 *
 * The point of the fake is the *decision* the adapter makes at
 * `start()` time: "does the device composite the pointer itself?"
 * That decision is what the plan's A6 row turns on, and it has to be
 * testable without a display.
 */
class FakeCapture final : public nimrtc::plugins::IVideoCapture {
public:
    FakeCapture(bool composites_cursor,
                std::int32_t origin_x = 0,
                std::int32_t origin_y = 0) noexcept
        : composites_(composites_cursor),
          origin_x_(origin_x),
          origin_y_(origin_y) {}

    std::vector<nimrtc::plugins::CaptureDevice> enumerate_devices()
        const noexcept override {
        nimrtc::plugins::CaptureDevice d;
        d.id = "fake:0";
        d.name = "Fake";
        d.kind = CaptureDeviceKind::kScreen;
        d.max_width = 32;
        d.max_height = 32;
        return {d};
    }

    std::vector<CaptureFormat> supported_formats(
        const std::string&) const noexcept override {
        CaptureFormat f;
        f.width = 32;
        f.height = 32;
        f.fps = 30;
        f.tag = "bgra";
        return {f};
    }

    nimrtc::plugins::CaptureCapabilities capabilities() const noexcept override {
        nimrtc::plugins::CaptureCapabilities c;
        c.backend = "fake";
        c.cursor_composited = composites_;
        c.enumeration = true;
        c.region_selection = true;
        return c;
    }

    nimrtc::plugins::Status open() noexcept override {
        cfg_.format.width = 32;
        cfg_.format.height = 32;
        cfg_.format.fps = 30;
        cfg_.format.tag = "bgra";
        cfg_.surface_origin_x = origin_x_;
        cfg_.surface_origin_y = origin_y_;
        opened_ = true;
        return nimrtc::plugins::kOk;
    }

    void close() noexcept override { opened_ = false; }

    [[nodiscard]] bool is_open() const noexcept override { return opened_; }

    [[nodiscard]] CaptureConfig config() const noexcept override {
        return cfg_;
    }

    void set_frame_callback(
        nimrtc::plugins::CaptureFrameCallback cb) noexcept override {
        cb_ = std::move(cb);
    }

    [[nodiscard]] nimrtc::plugins::CaptureStats stats() const noexcept override {
        return nimrtc::plugins::CaptureStats{};
    }

    const char* name() const noexcept override { return "fake-capture"; }

    nimrtc::plugins::Status pop_frame(nimrtc::plugins::CaptureFrame& out) noexcept
        override {
        if (!opened_) return nimrtc::plugins::kErrNotReady;
        if (!pending_) return nimrtc::plugins::kErrNotReady;

        pending_ = false;
        out = {};
        out.width = 32;
        out.height = 32;
        out.stride = 32 * 4;
        out.size = 32u * 32u * 4u;
        out.pixels = pixels_.data();
        out.capture_ts_us = 1;
        out.frame_seq = 0;
        return nimrtc::plugins::kOk;
    }

    /** Make one frame available to `pop_frame()`. */
    void publish() { pending_ = true; }

    /** The device's pixels. */
    std::vector<std::uint8_t>& pixels() { return pixels_; }

private:
    bool composites_;
    std::int32_t origin_x_;
    std::int32_t origin_y_;
    bool opened_ = false;
    bool pending_ = false;
    CaptureConfig cfg_{};
    std::vector<std::uint8_t> pixels_ =
        std::vector<std::uint8_t>(32u * 32u * 4u, 0x40u);
    nimrtc::plugins::CaptureFrameCallback cb_;
};

// ---------------------------------------------------------------------------
// A cursor source the test drives, but that records the origin
// ---------------------------------------------------------------------------

/**
 * @brief A manual source that remembers the surface origin it was told.
 *
 * The origin is what makes a multi-monitor capture work: the pointer
 * is reported in the platform's *global* space, so a capture of the
 * second monitor has to shift it.
 *
 * The shift is applied here rather than in the adapter, because that is
 * the seam's contract: a source reports *surface-relative* coordinates
 * (the Windows source subtracts the display's origin, the X11 source
 * the screen's), and the adapter subtracts the *region's* origin on
 * top of that.
 */
class RecordingSource final : public ICursorSource {
public:
    void set_state(CursorState s) noexcept { state_ = std::move(s); }

    CursorState query() noexcept override {
        CursorState out = state_;
        out.x -= origin_x_;
        out.y -= origin_y_;
        return out;
    }

    [[nodiscard]] std::string_view backend() const noexcept override {
        return "recording";
    }

    [[nodiscard]] bool has_live_cursor() const noexcept override { return true; }

    void set_surface_origin(std::int32_t x, std::int32_t y) noexcept override {
        origin_x_ = x;
        origin_y_ = y;
    }

    [[nodiscard]] std::int32_t origin_x() const noexcept { return origin_x_; }
    [[nodiscard]] std::int32_t origin_y() const noexcept { return origin_y_; }

private:
    CursorState state_{};
    std::int32_t origin_x_ = 0;
    std::int32_t origin_y_ = 0;
};

/** A 4x4 opaque red cursor bitmap at (x, y). */
CursorState make_cursor(std::int32_t x, std::int32_t y) {
    CursorState s;
    s.visible = true;
    s.live = true;
    s.x = x;
    s.y = y;
    s.bitmap_width = 4;
    s.bitmap_height = 4;
    s.bitmap_stride = 4 * 4;
    s.bgra.assign(4u * 4u * 4u, 0u);
    for (std::size_t i = 0; i < s.bgra.size(); i += 4u) {
        s.bgra[i + 0u] = 0x00u;   // B
        s.bgra[i + 1u] = 0x00u;   // G
        s.bgra[i + 2u] = 0xFFu;   // R
        s.bgra[i + 3u] = 0xFFu;   // A
    }
    return s;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// The decision: when is the cursor layer installed?
// ---------------------------------------------------------------------------

TEST(CursorWiring, TheLayerIsInstalledWhenTheDeviceDoesNotComposite) {
    // DXGI Desktop Duplication hands over a raw desktop texture with no
    // pointer in it, so the layer has to run.
    FakeCapture cap{/*composites_cursor=*/false};
    CaptureSourceAdapter adapter{&cap, VideoSourceConfig{}};

    ASSERT_EQ(adapter.start(), nimrtc::plugins::kOk);
    EXPECT_TRUE(adapter.cursor_layer_installed());
    EXPECT_NE(adapter.cursor_source(), nullptr);
    EXPECT_NE(adapter.cursor_compositor(), nullptr);
}

TEST(CursorWiring, TheLayerIsSkippedWhenTheDeviceCompositesItself) {
    // Windows Graphics Capture and macOS ScreenCaptureKit draw the
    // pointer into the frame themselves; drawing it again would
    // double-draw it.
    FakeCapture cap{/*composites_cursor=*/true};
    CaptureSourceAdapter adapter{&cap, VideoSourceConfig{}};

    ASSERT_EQ(adapter.start(), nimrtc::plugins::kOk);
    EXPECT_FALSE(adapter.cursor_layer_installed());
    EXPECT_EQ(adapter.cursor_source(), nullptr);
    EXPECT_EQ(adapter.cursor_compositor(), nullptr);
}

TEST(CursorWiring, AHostInstalledCursorWinsOverTheAutomaticOne) {
    // A host that already tracks the pointer (a remote agent that
    // knows where the *remote* pointer is) keeps its own.
    FakeCapture cap{/*composites_cursor=*/false};
    CaptureSourceAdapter adapter{&cap, VideoSourceConfig{}};

    ManualCursorSource src;
    AlphaCursorCompositor comp;
    adapter.set_cursor(&src, &comp);

    ASSERT_EQ(adapter.start(), nimrtc::plugins::kOk);
    // The host's objects are the ones in place.
    EXPECT_EQ(adapter.cursor_source(), &src);
    EXPECT_EQ(adapter.cursor_compositor(), &comp);
    // And the adapter did not install a layer of its own.
    EXPECT_FALSE(adapter.cursor_layer_installed());
}

TEST(CursorWiring, TheLayerSurvivesAStopAndStart) {
    FakeCapture cap{/*composites_cursor=*/false};
    CaptureSourceAdapter adapter{&cap, VideoSourceConfig{}};

    ASSERT_EQ(adapter.start(), nimrtc::plugins::kOk);
    EXPECT_TRUE(adapter.cursor_layer_installed());

    adapter.stop();
    // A closed device has no surface, so the layer goes away with it.
    EXPECT_FALSE(adapter.cursor_layer_installed());

    ASSERT_EQ(adapter.start(), nimrtc::plugins::kOk);
    EXPECT_TRUE(adapter.cursor_layer_installed());
}

// ---------------------------------------------------------------------------
// The surface origin
// ---------------------------------------------------------------------------

TEST(CursorWiring, TheSurfaceOriginReachesTheCursorSource) {
    // The pointer is reported in the platform's global space; a capture
    // of the *second* monitor has to shift it.  The device reports the
    // surface's origin, and the adapter hands it to the source.
    FakeCapture cap{/*composites_cursor=*/false,
                    /*origin_x=*/1920, /*origin_y=*/-200};
    CaptureSourceAdapter adapter{&cap, VideoSourceConfig{}};

    RecordingSource src;
    AlphaCursorCompositor comp;
    adapter.set_cursor(&src, &comp);

    ASSERT_EQ(adapter.start(), nimrtc::plugins::kOk);

    // The adapter forwards the device's origin to the source.
    // (The host-installed source is the one in place, so this is the
    // path a host's own source sees.)
    EXPECT_EQ(src.origin_x(), 1920);
    EXPECT_EQ(src.origin_y(), -200);
}

TEST(CursorWiring, TheSurfaceOriginShiftsThePointerIntoTheFrame) {
    // The end-to-end property: a pointer at global (1928, -196) on a
    // surface that starts at (1920, -200) lands at frame (8, 4).
    FakeCapture cap{/*composites_cursor=*/false,
                    /*origin_x=*/1920, /*origin_y=*/-200};

    // The cursor is alpha-blended, which needs a *packed* output: a
    // planar YUV frame has no alpha channel to blend into.
    VideoSourceConfig scfg{};
    scfg.format = VideoPixelFormat::kBGRA;
    CaptureSourceAdapter adapter{&cap, scfg};

    RecordingSource src;
    AlphaCursorCompositor comp;
    // The source reports the *global* position; the surface origin is
    // what turns it into a surface-relative one — which is exactly
    // what `set_surface_origin` is for.
    src.set_state(make_cursor(1928, -196));
    adapter.set_cursor(&src, &comp);

    VideoSourceFrame seen{};
    bool called = false;
    adapter.set_callback([&](const VideoSourceFrame& f) {
        called = true;
        seen = f;
    });

    ASSERT_EQ(adapter.start(), nimrtc::plugins::kOk);
    cap.publish();
    adapter.produce_one();

    ASSERT_TRUE(called);
    // The pointer was drawn at frame (8, 4): the pixel there is red.
    ASSERT_NE(seen.plane_y, nullptr);
    const std::size_t idx = (4u * 32u + 8u) * 4u;
    EXPECT_EQ(seen.plane_y[idx + 2u], 0xFFu);
    // And the frame is pixel-exact outside the pointer's box — the
    // plan's acceptance criterion.
    for (std::uint32_t y = 0; y < 32u; ++y) {
        for (std::uint32_t x = 0; x < 32u; ++x) {
            const bool in_box = x >= 8u && x < 12u && y >= 4u && y < 8u;
            if (in_box) continue;
            const std::size_t i = (static_cast<std::size_t>(y) * 32u + x) * 4u;
            EXPECT_EQ(seen.plane_y[i + 2u], 0x40u)
                << "pixel (" << x << "," << y << ") changed outside the box";
        }
    }
}

// ---------------------------------------------------------------------------
// The engine's own path: a planar I420 output
// ---------------------------------------------------------------------------

TEST(CursorWiring, ThePointerIsDrawnOnThePlanarOutputToo) {
    // The engine's capture pump asks for **I420** (the encoders want
    // YUV 4:2:0), not BGRA.  A cursor layer that only worked on a
    // packed output would therefore never draw on the path that
    // matters — so the pointer is composited into the *device's*
    // pixels, before the conversion, and survives it.
    FakeCapture cap{/*composites_cursor=*/false};
    VideoSourceConfig scfg{};
    scfg.format = VideoPixelFormat::kI420;
    CaptureSourceAdapter adapter{&cap, scfg};

    RecordingSource src;
    AlphaCursorCompositor comp;
    src.set_state(make_cursor(8, 4));
    adapter.set_cursor(&src, &comp);

    VideoSourceFrame seen{};
    bool called = false;
    adapter.set_callback([&](const VideoSourceFrame& f) {
        called = true;
        seen = f;
    });

    ASSERT_EQ(adapter.start(), nimrtc::plugins::kOk);
    cap.publish();
    adapter.produce_one();

    ASSERT_TRUE(called);
    EXPECT_EQ(seen.format, VideoPixelFormat::kI420);
    // The pointer was drawn.
    EXPECT_EQ(adapter.cursor_frames_composited(), 1u);

    // And it is in the published picture: the pointer's box is a red
    // patch on a grey desktop, so the luma under it differs from the
    // luma far away from it.
    ASSERT_NE(seen.plane_y, nullptr);
    const std::size_t inside = (4u * 32u + 8u);        // row 4, column 8
    const std::size_t outside = (30u * 32u + 30u);     // row 30, column 30
    EXPECT_NE(seen.plane_y[inside], seen.plane_y[outside])
        << "the pointer is not visible in the published I420 frame";
}

TEST(CursorWiring, TheDevicePixelsAreNotWrittenInto) {
    // The device owns its pixels and may recycle them the moment the
    // callback returns, so the adapter must not draw into the
    // device's buffer: it copies first.
    FakeCapture cap{/*composites_cursor=*/false};
    VideoSourceConfig scfg{};
    scfg.format = VideoPixelFormat::kI420;
    CaptureSourceAdapter adapter{&cap, scfg};

    RecordingSource src;
    AlphaCursorCompositor comp;
    src.set_state(make_cursor(8, 4));
    adapter.set_cursor(&src, &comp);

    ASSERT_EQ(adapter.start(), nimrtc::plugins::kOk);
    cap.publish();
    adapter.produce_one();

    // The device's own buffer is untouched: the pixel under the
    // pointer is still the desktop's grey.
    const std::size_t idx = (4u * 32u + 8u) * 4u;
    EXPECT_EQ(cap.pixels()[idx + 2u], 0x40u)
        << "the adapter wrote into the device's own pixels";
}

// ---------------------------------------------------------------------------
// The platform source
// ---------------------------------------------------------------------------
TEST(CursorWiring, ThePlatformSourceIsCreatedOnEveryPlatform) {
    // On a host without a pointer to read (macOS, a build without the
    // X11 libraries) the factory returns nullptr — there is no pointer,
    // and a null source says exactly that.  Elsewhere it returns a
    // source.
    auto source = make_platform_cursor_source();
#if defined(_WIN32)
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->backend(), "win-cursor");
#elif defined(__linux__) && defined(NIMRTC_HAVE_X11)
    // On Linux the X11 source is created when the host has a display;
    // without one there is no pointer to read and the factory says so.
    if (source != nullptr) {
        EXPECT_EQ(source->backend(), "x11-cursor");
    }
#else
    EXPECT_EQ(source, nullptr);
#endif
}

TEST(CursorWiring, TheWindowsSourceTranslatesTheVirtualDesktopOrigin) {
    // The Windows source's own contract: the hotspot is reported in
    // virtual-desktop coordinates, and the display's origin is
    // subtracted.  A headless CI agent has no *visible* pointer, so
    // the test skips honestly rather than lying about it.
    auto source = nimrtc::video_capture::make_win_cursor_source();
#if defined(_WIN32)
    ASSERT_NE(source, nullptr);

    const auto at_origin = source->query();
    if (!at_origin.visible) {
        GTEST_SKIP() << "no visible pointer on this host";
    }

    // Shift the surface's origin to the right by a whole screen: the
    // reported position moves by the same amount.
    source->set_surface_origin(1920, 0);
    const auto shifted = source->query();

    EXPECT_EQ(at_origin.x - 1920, shifted.x);
    EXPECT_EQ(at_origin.y, shifted.y);
#else
    EXPECT_EQ(source, nullptr);
#endif
}

TEST(CursorWiring, TheX11SourceTranslatesTheRootWindowOrigin) {
    // The X11 source's own contract: the hotspot is reported in
    // root-window coordinates, and the screen's origin is subtracted.
    auto source = nimrtc::video_capture::make_x11_cursor_source();
#if defined(__linux__) && defined(NIMRTC_HAVE_X11)
    ASSERT_NE(source, nullptr);

    const auto at_origin = source->query();
    if (!at_origin.visible) {
        GTEST_SKIP() << "no visible pointer on this host";
    }

    source->set_surface_origin(1920, 0);
    const auto shifted = source->query();

    EXPECT_EQ(at_origin.x - 1920, shifted.x);
    EXPECT_EQ(at_origin.y, shifted.y);
#else
    EXPECT_EQ(source, nullptr);
#endif
}
