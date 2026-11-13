// modules/video_capture/tests/test_video_capture.cpp
// ----------------------------------------------------------------------------
// The portable capture layer's tests.
//
// These run on every CI leg, on every platform, with no display and no
// camera: the synthetic device is the reference implementation of the
// mechanism the platform backends are written against, so a test that
// passes here proves the *mechanism* is right on every platform.
//
// The properties pinned down here are the ones the remote-desktop plan
// names explicitly:
//
//   - A2: "queue overflow drops the **oldest** frame and increments a
//     counter; the drop policy is configurable."
//   - A2b: "the portable layer is exercised by a synthetic capture
//     device on all CI legs."
//   - A7: "a static desktop produces frames whose dirty rect is empty."
//   - A8: "a capture configured for an 800x600 region produces frames of
//     exactly that size."
// ----------------------------------------------------------------------------

#include <nimrtc/video_capture/capture_queue.hpp>
#include <nimrtc/video_capture/synthetic_capture.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using nimrtc::plugins::CaptureConfig;
using nimrtc::plugins::CaptureDeviceKind;
using nimrtc::plugins::CaptureDropPolicy;
using nimrtc::plugins::CaptureFrame;
using nimrtc::video_capture::CaptureQueue;
using nimrtc::video_capture::SyntheticCapture;

// ---------------------------------------------------------------------------
// The queue
// ---------------------------------------------------------------------------

TEST(CaptureQueue, StartsEmpty) {
    CaptureQueue q(2);
    EXPECT_EQ(q.size(), 0u);
    EXPECT_EQ(q.pushed(), 0u);
    EXPECT_EQ(q.dropped(), 0u);
}

TEST(CaptureQueue, PopOnEmptyReportsNotReady) {
    CaptureQueue q(2);
    CaptureFrame f{};
    EXPECT_FALSE(q.pop(f));
}

TEST(CaptureQueue, RoundTripsThePixels) {
    CaptureQueue q(2);
    const std::uint8_t pixels[4] = {1, 2, 3, 4};
    ASSERT_EQ(q.push(pixels, sizeof(pixels), 4, 1, 1, 123, 7, {}),
              CaptureQueue::PushResult::kQueued);

    CaptureFrame f{};
    ASSERT_TRUE(q.pop(f));
    ASSERT_NE(f.pixels, nullptr);
    EXPECT_EQ(f.size, 4u);
    EXPECT_EQ(f.pixels[0], 1);
    EXPECT_EQ(f.pixels[3], 4);
    EXPECT_EQ(f.width, 1u);
    EXPECT_EQ(f.height, 1u);
    EXPECT_EQ(f.capture_ts_us, 123);
    EXPECT_EQ(f.frame_seq, 7u);
}

// Regression: `pop()` used to hand out a pointer into a *local* frame
// that died with the call, so the consumer read freed memory.  The
// `CaptureFrame` contract says the pixels stay valid until the next
// `pop_frame()`.  Under ASan this test fails with a heap-use-after-free
// if the popped buffer is not kept alive inside the queue.
TEST(CaptureQueue, ThePoppedPixelsOutliveThePop) {
    CaptureQueue q(2);
    const std::uint8_t pixels[4] = {10, 20, 30, 40};
    ASSERT_EQ(q.push(pixels, sizeof(pixels), 4, 1, 1, 5, 1, {}),
              CaptureQueue::PushResult::kQueued);

    CaptureFrame f{};
    ASSERT_TRUE(q.pop(f));
    ASSERT_NE(f.pixels, nullptr);

    // Read the pixels *after* pop() returned — this is the use-after-free.
    EXPECT_EQ(f.pixels[0], 10);
    EXPECT_EQ(f.pixels[1], 20);
    EXPECT_EQ(f.pixels[2], 30);
    EXPECT_EQ(f.pixels[3], 40);

    // And they survive a subsequent (failing) pop, which does not
    // invalidate the previous frame.
    CaptureFrame g{};
    EXPECT_FALSE(q.pop(g));
    EXPECT_EQ(f.pixels[0], 10);
}

TEST(CaptureQueue, KeepsTheOrder) {
    CaptureQueue q(4);
    const std::uint8_t a[1] = {1};
    const std::uint8_t b[1] = {2};
    const std::uint8_t c[1] = {3};
    ASSERT_EQ(q.push(a, 1, 1, 1, 1, 0, 1, {}), CaptureQueue::PushResult::kQueued);
    ASSERT_EQ(q.push(b, 1, 1, 1, 1, 0, 2, {}), CaptureQueue::PushResult::kQueued);
    ASSERT_EQ(q.push(c, 1, 1, 1, 1, 0, 3, {}), CaptureQueue::PushResult::kQueued);

    CaptureFrame f{};
    ASSERT_TRUE(q.pop(f));
    EXPECT_EQ(f.frame_seq, 1u);
    ASSERT_TRUE(q.pop(f));
    EXPECT_EQ(f.frame_seq, 2u);
    ASSERT_TRUE(q.pop(f));
    EXPECT_EQ(f.frame_seq, 3u);
    EXPECT_FALSE(q.pop(f));
}

// ---------------------------------------------------------------------------
// The drop policy — the heart of the bounded queue
// ---------------------------------------------------------------------------

TEST(CaptureQueue, OverflowDropsTheOldestByDefault) {
    CaptureQueue q(2);
    EXPECT_EQ(q.drop_policy(), CaptureDropPolicy::kDropOldest);

    const std::uint8_t a[1] = {1};
    const std::uint8_t b[1] = {2};
    const std::uint8_t c[1] = {3};
    ASSERT_EQ(q.push(a, 1, 1, 1, 1, 0, 1, {}), CaptureQueue::PushResult::kQueued);
    ASSERT_EQ(q.push(b, 1, 1, 1, 1, 0, 2, {}), CaptureQueue::PushResult::kQueued);
    // The queue is full: the oldest goes, the newest stays.
    ASSERT_EQ(q.push(c, 1, 1, 1, 1, 0, 3, {}),
              CaptureQueue::PushResult::kDroppedOldest);

    EXPECT_EQ(q.size(), 2u);
    EXPECT_EQ(q.dropped(), 1u);

    CaptureFrame f{};
    ASSERT_TRUE(q.pop(f));
    EXPECT_EQ(f.frame_seq, 2u);
    ASSERT_TRUE(q.pop(f));
    EXPECT_EQ(f.frame_seq, 3u);
}

TEST(CaptureQueue, DropNewestKeepsWhatIsAlreadyQueued) {
    CaptureQueue q(2);
    q.set_drop_policy(CaptureDropPolicy::kDropNewest);

    const std::uint8_t a[1] = {1};
    const std::uint8_t b[1] = {2};
    const std::uint8_t c[1] = {3};
    ASSERT_EQ(q.push(a, 1, 1, 1, 1, 0, 1, {}), CaptureQueue::PushResult::kQueued);
    ASSERT_EQ(q.push(b, 1, 1, 1, 1, 0, 2, {}), CaptureQueue::PushResult::kQueued);
    // The arrival is refused; the queue keeps what it had.
    EXPECT_EQ(q.push(c, 1, 1, 1, 1, 0, 3, {}), CaptureQueue::PushResult::kRejected);

    EXPECT_EQ(q.size(), 2u);
    EXPECT_EQ(q.dropped(), 1u);

    CaptureFrame f{};
    ASSERT_TRUE(q.pop(f));
    EXPECT_EQ(f.frame_seq, 1u);
    ASSERT_TRUE(q.pop(f));
    EXPECT_EQ(f.frame_seq, 2u);
}

TEST(CaptureQueue, DropIncomingRefusesTheArrival) {
    CaptureQueue q(1);
    q.set_drop_policy(CaptureDropPolicy::kDropIncoming);

    const std::uint8_t a[1] = {1};
    const std::uint8_t b[1] = {2};
    ASSERT_EQ(q.push(a, 1, 1, 1, 1, 0, 1, {}), CaptureQueue::PushResult::kQueued);
    EXPECT_EQ(q.push(b, 1, 1, 1, 1, 0, 2, {}), CaptureQueue::PushResult::kRejected);

    CaptureFrame f{};
    ASSERT_TRUE(q.pop(f));
    EXPECT_EQ(f.frame_seq, 1u);
}

TEST(CaptureQueue, DepthOneKeepsOnlyTheNewest) {
    CaptureQueue q(1);
    const std::uint8_t a[1] = {1};
    const std::uint8_t b[1] = {2};
    ASSERT_EQ(q.push(a, 1, 1, 1, 1, 0, 1, {}), CaptureQueue::PushResult::kQueued);
    ASSERT_EQ(q.push(b, 1, 1, 1, 1, 0, 2, {}),
              CaptureQueue::PushResult::kDroppedOldest);

    CaptureFrame f{};
    ASSERT_TRUE(q.pop(f));
    EXPECT_EQ(f.frame_seq, 2u);
}

TEST(CaptureQueue, PeekDoesNotConsume) {
    CaptureQueue q(2);
    const std::uint8_t a[1] = {7};
    ASSERT_EQ(q.push(a, 1, 1, 1, 1, 0, 1, {}), CaptureQueue::PushResult::kQueued);

    CaptureFrame peeked{};
    ASSERT_TRUE(q.peek(peeked));
    EXPECT_EQ(peeked.frame_seq, 1u);
    EXPECT_EQ(q.size(), 1u);

    CaptureFrame popped{};
    ASSERT_TRUE(q.pop(popped));
    EXPECT_EQ(popped.frame_seq, 1u);
}

TEST(CaptureQueue, ClearDiscardsEverything) {
    CaptureQueue q(2);
    const std::uint8_t a[1] = {1};
    ASSERT_EQ(q.push(a, 1, 1, 1, 1, 0, 1, {}), CaptureQueue::PushResult::kQueued);
    q.clear();
    EXPECT_EQ(q.size(), 0u);
}

// ---------------------------------------------------------------------------
// The synthetic device
// ---------------------------------------------------------------------------

TEST(SyntheticCapture, EnumeratesOneScreen) {
    SyntheticCapture cap{CaptureConfig{}};
    const auto devices = cap.enumerate_devices();
    ASSERT_EQ(devices.size(), 1u);
    EXPECT_EQ(devices[0].kind, CaptureDeviceKind::kScreen);
    EXPECT_FALSE(devices[0].id.empty());
}

TEST(SyntheticCapture, OffersSeveralFormats) {
    SyntheticCapture cap{CaptureConfig{}};
    const auto formats = cap.supported_formats("synthetic:0");
    ASSERT_FALSE(formats.empty());
    for (const auto& f : formats) {
        EXPECT_GT(f.width, 0u);
        EXPECT_GT(f.height, 0u);
        EXPECT_GT(f.fps, 0u);
    }
}

TEST(SyntheticCapture, ReportsItsCapabilities) {
    SyntheticCapture cap{CaptureConfig{}};
    const auto caps = cap.capabilities();
    EXPECT_TRUE(caps.dirty_rects);
    EXPECT_TRUE(caps.region_selection);
    EXPECT_TRUE(caps.enumeration);
    EXPECT_FALSE(caps.zero_copy);
    EXPECT_EQ(caps.backend, "synthetic");
}

TEST(SyntheticCapture, PopBeforeOpenReportsNotReady) {
    SyntheticCapture cap{CaptureConfig{}};
    CaptureFrame f{};
    EXPECT_EQ(cap.pop_frame(f), nimrtc::plugins::kErrNotReady);
}

TEST(SyntheticCapture, RejectsAnUnknownDevice) {
    CaptureConfig cfg{};
    cfg.device_id = "no-such-device";
    SyntheticCapture cap{cfg};
    EXPECT_EQ(cap.open(), nimrtc::plugins::kErrInvalidParam);
    EXPECT_FALSE(cap.is_open());
}

TEST(SyntheticCapture, NegotiatesADefaultFormat) {
    SyntheticCapture cap{CaptureConfig{}};
    ASSERT_EQ(cap.open(), nimrtc::plugins::kOk);
    EXPECT_EQ(cap.config().format.width, 1280u);
    EXPECT_EQ(cap.config().format.height, 720u);
    EXPECT_GT(cap.config().format.fps, 0u);
}

TEST(SyntheticCapture, ProducesAndPopsAFrame) {
    CaptureConfig cfg{};
    cfg.format.width  = 320;
    cfg.format.height = 240;
    SyntheticCapture cap{cfg};
    ASSERT_EQ(cap.open(), nimrtc::plugins::kOk);
    ASSERT_EQ(cap.produce_one(), nimrtc::plugins::kOk);

    CaptureFrame f{};
    ASSERT_EQ(cap.pop_frame(f), nimrtc::plugins::kOk);
    EXPECT_EQ(f.width, 320u);
    EXPECT_EQ(f.height, 240u);
    EXPECT_EQ(f.stride, static_cast<std::int32_t>(320u * 4u));
    EXPECT_EQ(f.size, static_cast<std::size_t>(320u * 240u * 4u));
    ASSERT_NE(f.pixels, nullptr);
    EXPECT_EQ(cap.stats().frames_captured, 1u);
}

TEST(SyntheticCapture, AStaticDesktopHasNoDirtyRect) {
    CaptureConfig cfg{};
    cfg.format.width  = 320;
    cfg.format.height = 240;
    SyntheticCapture cap{cfg};
    ASSERT_EQ(cap.open(), nimrtc::plugins::kOk);

    cap.set_static_frame(true);
    ASSERT_EQ(cap.produce_one(), nimrtc::plugins::kOk);
    ASSERT_EQ(cap.produce_one(), nimrtc::plugins::kOk);

    CaptureFrame f{};
    ASSERT_EQ(cap.pop_frame(f), nimrtc::plugins::kOk);
    EXPECT_FALSE(f.dirty.full_frame);
    EXPECT_TRUE(f.dirty.rects.empty());
}

TEST(SyntheticCapture, AMovingDesktopReportsADirtyRect) {
    CaptureConfig cfg{};
    cfg.format.width  = 320;
    cfg.format.height = 240;
    SyntheticCapture cap{cfg};
    ASSERT_EQ(cap.open(), nimrtc::plugins::kOk);

    ASSERT_EQ(cap.produce_one(), nimrtc::plugins::kOk);
    ASSERT_EQ(cap.produce_one(), nimrtc::plugins::kOk);

    CaptureFrame f{};
    ASSERT_EQ(cap.pop_frame(f), nimrtc::plugins::kOk);
    EXPECT_FALSE(f.dirty.full_frame);
    EXPECT_FALSE(f.dirty.rects.empty());
    // The dirty rect must lie inside the frame.
    for (const auto& r : f.dirty.rects) {
        EXPECT_LT(r.x + r.width, 320u + 1u);
        EXPECT_LT(r.y + r.height, 240u + 1u);
    }
}

TEST(SyntheticCapture, TheDirtyRectFlagAgreesWithWhatTheFramesCarry) {
    // The capability flag is what a host reads to decide whether it can
    // rely on the dirty-rect metadata.  A flag that says "false" while
    // the frames carry the metadata is not merely conservative — it is
    // wrong, and it makes the *consumer* ignore metadata that is
    // present and correct.
    //
    // The synthetic device is the reference implementation of the
    // mechanism the platform backends are written against, so this test
    // states the invariant every backend has to satisfy.
    CaptureConfig cfg{};
    cfg.format.width  = 320;
    cfg.format.height = 240;
    SyntheticCapture cap{cfg};
    ASSERT_EQ(cap.open(), nimrtc::plugins::kOk);

    const auto caps = cap.capabilities();
    ASSERT_TRUE(caps.dirty_rects);

    // The negotiated format must agree with the capability.
    EXPECT_TRUE(cap.config().format.has_dirty_rects);

    // And the frames must actually carry the metadata the flag promises.
    ASSERT_EQ(cap.produce_one(), nimrtc::plugins::kOk);
    ASSERT_EQ(cap.produce_one(), nimrtc::plugins::kOk);

    CaptureFrame f{};
    ASSERT_EQ(cap.pop_frame(f), nimrtc::plugins::kOk);
    // A *moving* desktop produced a real rect, not the "everything
    // changed" fallback.
    EXPECT_FALSE(f.dirty.full_frame);
    EXPECT_FALSE(f.dirty.rects.empty());
}

TEST(SyntheticCapture, TheRegionSelectsTheFrameSize) {
    CaptureConfig cfg{};
    cfg.format.width  = 1920;
    cfg.format.height = 1080;
    cfg.region.x      = 100;
    cfg.region.y      = 50;
    cfg.region.width  = 800;
    cfg.region.height = 600;
    SyntheticCapture cap{cfg};
    ASSERT_EQ(cap.open(), nimrtc::plugins::kOk);
    ASSERT_EQ(cap.produce_one(), nimrtc::plugins::kOk);

    CaptureFrame f{};
    ASSERT_EQ(cap.pop_frame(f), nimrtc::plugins::kOk);
    EXPECT_EQ(f.width, 800u);
    EXPECT_EQ(f.height, 600u);
    EXPECT_EQ(f.size, static_cast<std::size_t>(800u * 600u * 4u));
}

TEST(SyntheticCapture, OverflowDropsTheOldestAndCountsIt) {
    CaptureConfig cfg{};
    cfg.format.width  = 64;
    cfg.format.height = 64;
    cfg.queue_depth   = 2;
    SyntheticCapture cap{cfg};
    ASSERT_EQ(cap.open(), nimrtc::plugins::kOk);

    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(cap.produce_one(), nimrtc::plugins::kOk);
    }

    // The queue holds 2; the other 3 were dropped as the oldest.
    EXPECT_EQ(cap.stats().frames_dropped, 3u);

    CaptureFrame f{};
    ASSERT_EQ(cap.pop_frame(f), nimrtc::plugins::kOk);
    EXPECT_EQ(f.frame_seq, 3u);
    ASSERT_EQ(cap.pop_frame(f), nimrtc::plugins::kOk);
    EXPECT_EQ(f.frame_seq, 4u);
    EXPECT_EQ(cap.pop_frame(f), nimrtc::plugins::kErrNotReady);
}

TEST(SyntheticCapture, TheCallbackSeesTheQueuedFrame) {
    CaptureConfig cfg{};
    cfg.format.width  = 64;
    cfg.format.height = 64;
    SyntheticCapture cap{cfg};
    ASSERT_EQ(cap.open(), nimrtc::plugins::kOk);

    bool called = false;
    std::uint32_t seen_seq = 0;
    cap.set_frame_callback([&](const CaptureFrame& f) {
        called = true;
        seen_seq = f.frame_seq;
    });

    ASSERT_EQ(cap.produce_one(), nimrtc::plugins::kOk);
    EXPECT_TRUE(called);
    EXPECT_EQ(seen_seq, 0u);

    CaptureFrame popped{};
    ASSERT_EQ(cap.pop_frame(popped), nimrtc::plugins::kOk);
    EXPECT_EQ(popped.frame_seq, seen_seq);
}

TEST(SyntheticCapture, TheFrameBudgetModelsADeviceLost) {
    CaptureConfig cfg{};
    cfg.format.width  = 64;
    cfg.format.height = 64;
    SyntheticCapture cap{cfg};
    ASSERT_EQ(cap.open(), nimrtc::plugins::kOk);
    cap.set_frame_budget(2);

    EXPECT_EQ(cap.produce_one(), nimrtc::plugins::kOk);
    EXPECT_EQ(cap.produce_one(), nimrtc::plugins::kOk);
    EXPECT_EQ(cap.produce_one(), nimrtc::plugins::kErrHardwareError);
    EXPECT_EQ(cap.stats().device_errors, 1u);
}

TEST(SyntheticCapture, CloseStopsProduction) {
    CaptureConfig cfg{};
    SyntheticCapture cap{cfg};
    ASSERT_EQ(cap.open(), nimrtc::plugins::kOk);
    cap.close();
    EXPECT_FALSE(cap.is_open());
    EXPECT_EQ(cap.produce_one(), nimrtc::plugins::kErrNotReady);
}
