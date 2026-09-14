/**
 * @file modules/video_sink/tests/test_video_sink.cpp
 * @brief Unit tests for HeadlessSink and PinnedSink + PluginAdapter.
 *
 * Verifies:
 *   - HeadlessSink.render() increments frames_rendered
 *   - PinnedSink.render() enforces strict capture_ts ordering
 *   - PluginAdapter forwards calls to the underlying concrete sink
 *   - HeadlessPluginFactory / PinnedPluginFactory produce usable adapters
 *   - register_default_plugins() installs both factories into PluginRegistry
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/plugins/video_sink.hpp>
#include <nimrtc/video_sink/video_sink.hpp>

namespace {

using nimrtc::core::PluginRegistry;
namespace np = nimrtc::plugins;

np::VideoSourceFrame make_frame(std::uint64_t capture_ts_us,
                                std::uint32_t w = 320,
                                std::uint32_t h = 240) noexcept {
    np::VideoSourceFrame f{};
    f.width  = w;
    f.height = h;
    f.format = np::VideoPixelFormat::kI420;
    f.capture_ts_us = static_cast<np::TimestampUs>(capture_ts_us);
    f.frame_seq = static_cast<std::uint32_t>(capture_ts_us / 1000);
    return f;
}

// ---------------------------------------------------------------------------
// HeadlessSink
// ---------------------------------------------------------------------------

TEST(HeadlessSink, RendersAndCounts) {
    nimrtc::video_sink::HeadlessSink s({});
    ASSERT_EQ(s.open(), np::kOk);
    ASSERT_EQ(s.render(make_frame(0)),     np::kOk);
    ASSERT_EQ(s.render(make_frame(1000)),  np::kOk);
    ASSERT_EQ(s.render(make_frame(2000)),  np::kOk);
    EXPECT_EQ(s.stats().frames_rendered, 3u);
    s.close();
}

TEST(HeadlessSink, ClosedSinkRejects) {
    nimrtc::video_sink::HeadlessSink s({});
    EXPECT_EQ(s.render(make_frame(0)), np::kErrNotReady);
}

// ---------------------------------------------------------------------------
// PinnedSink
// ---------------------------------------------------------------------------

TEST(PinnedSink, AcceptsMonotonicTimestamps) {
    nimrtc::video_sink::PinnedSink s({});
    ASSERT_EQ(s.open(), np::kOk);
    EXPECT_EQ(s.render(make_frame(0)),    np::kOk);
    EXPECT_EQ(s.render(make_frame(1000)), np::kOk);
    EXPECT_EQ(s.render(make_frame(2000)), np::kOk);
    EXPECT_EQ(s.stats().frames_rendered, 3u);
    s.close();
}

TEST(PinnedSink, RejectsNonMonotonicTimestamps) {
    nimrtc::video_sink::PinnedSink s({});
    ASSERT_EQ(s.open(), np::kOk);
    ASSERT_EQ(s.render(make_frame(1000)), np::kOk);
    EXPECT_EQ(s.render(make_frame(1000)), np::kErrCorrupt); // equal
    EXPECT_EQ(s.render(make_frame(500)),  np::kErrCorrupt); // regress
    EXPECT_EQ(s.stats().render_errors, 2u);
    EXPECT_EQ(s.stats().frames_rendered, 1u);
    s.close();
}

// ---------------------------------------------------------------------------
// PluginAdapter forwarding
// ---------------------------------------------------------------------------

TEST(PluginAdapter, ForwardsAllCalls) {
    auto headless = nimrtc::video_sink::create_headless_sink({});
    nimrtc::video_sink::PluginAdapter adapter(std::move(headless));
    EXPECT_STREQ(adapter.name(), "nimrtc::video_sink::HeadlessSink");
    ASSERT_EQ(adapter.open(), np::kOk);
    EXPECT_EQ(adapter.render(make_frame(0)), np::kOk);
    EXPECT_EQ(adapter.render(make_frame(1000)), np::kOk);
    EXPECT_EQ(adapter.stats().frames_rendered, 2u);
    adapter.close();
}

// ---------------------------------------------------------------------------
// Plugin factories
// ---------------------------------------------------------------------------

TEST(VideoSinkPluginFactories, HeadlessFactoryProducesUsableSink) {
    nimrtc::video_sink::HeadlessPluginFactory fac;
    std::unique_ptr<np::IVideoSink> sink(fac.create(np::VideoSinkConfig{}));
    ASSERT_NE(sink, nullptr);
    EXPECT_EQ(sink->open(), np::kOk);
    EXPECT_EQ(sink->render(make_frame(0)), np::kOk);
    sink->close();
}

TEST(VideoSinkPluginFactories, PinnedFactoryProducesUsableSink) {
    nimrtc::video_sink::PinnedPluginFactory fac;
    std::unique_ptr<np::IVideoSink> sink(fac.create(np::VideoSinkConfig{}));
    ASSERT_NE(sink, nullptr);
    EXPECT_EQ(sink->open(), np::kOk);
    EXPECT_EQ(sink->render(make_frame(0)), np::kOk);
    EXPECT_EQ(sink->render(make_frame(0)), np::kErrCorrupt);
    sink->close();
}

// ---------------------------------------------------------------------------
// Registry integration
// ---------------------------------------------------------------------------

class VideoSinkRegistration : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::video_sink::register_default_plugins();
    }
};

TEST_F(VideoSinkRegistration, HeadlessIsRegistered) {
    const auto* f = PluginRegistry::instance().get_video_sink("headless");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->id(), "headless");
}

TEST_F(VideoSinkRegistration, PinnedIsRegistered) {
    const auto* f = PluginRegistry::instance().get_video_sink("pinned");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->id(), "pinned");
}

TEST_F(VideoSinkRegistration, CreateThroughRegistry) {
    const auto* f = PluginRegistry::instance().get_video_sink("headless");
    ASSERT_NE(f, nullptr);
    std::unique_ptr<np::IVideoSink> sink(f->create(np::VideoSinkConfig{}));
    ASSERT_NE(sink, nullptr);
    EXPECT_EQ(sink->open(), np::kOk);
    EXPECT_EQ(sink->render(make_frame(0)), np::kOk);
    sink->close();
}

} // namespace
