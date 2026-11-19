// src/modules/video_sync/tests/test_video_sync_plugin.cpp
// ----------------------------------------------------------------------------
// The `plugins::IVideoSyncController` adapter over the render clock.
//
// The adapter is a translation layer: it must add no policy of its own.
// These cases pin that -- the adapter's answers are the module's
// answers, and the registry slot resolves the built-in clock.
// ----------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/plugins/video_sync.hpp>

#include "nimrtc/video_sync/video_sync.hpp"

namespace {

namespace np = nimrtc::plugins;

class VideoSyncPlugin : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        // The module's own registrar, not `core::register_all_default_plugins()`:
        // the latter walks the whole default-registrar table, which
        // references every built-in module.
        nimrtc::video_sync::register_default_plugins();
    }
};

TEST_F(VideoSyncPlugin, TheBuiltInClockIsRegistered) {
    const auto* factory =
        nimrtc::core::PluginRegistry::instance().get_video_sync_controller(
            "render-clock");
    ASSERT_NE(factory, nullptr)
        << "the built-in render clock must be registered under "
           "\"render-clock\"";
    EXPECT_EQ(factory->id(), "render-clock");
}

TEST_F(VideoSyncPlugin, TheFactoryProducesAController) {
    const auto* factory =
        nimrtc::core::PluginRegistry::instance().get_video_sync_controller(
            "render-clock");
    ASSERT_NE(factory, nullptr);

    np::VideoSyncConfig cfg;
    auto controller = factory->create(cfg);
    ASSERT_NE(controller, nullptr);
    EXPECT_STREQ(controller->name(), "video_sync::RenderClock");
    EXPECT_EQ(controller->open(), np::kOk);
    controller->close();
}

TEST_F(VideoSyncPlugin, TheAdapterIsATranslationAndAddsNoPolicy) {
    const auto* factory =
        nimrtc::core::PluginRegistry::instance().get_video_sync_controller(
            "render-clock");
    ASSERT_NE(factory, nullptr);

    np::VideoSyncConfig cfg;
    cfg.tolerance_us = 40'000;
    cfg.max_delay_us  = 200'000;

    auto controller = factory->create(cfg);
    ASSERT_NE(controller, nullptr);
    ASSERT_EQ(controller->open(), np::kOk);

    // Inside the band: present.
    EXPECT_EQ(controller->decide(0, 1).action, np::VideoSyncAction::kPresent);
    // Picture behind: drop.
    EXPECT_EQ(controller->decide(-100'000, 2).action, np::VideoSyncAction::kDrop);
    // Picture ahead: hold, for the skew minus the band.
    const auto held = controller->decide(100'000, 3);
    EXPECT_EQ(held.action, np::VideoSyncAction::kDelay);
    EXPECT_EQ(held.delay_us, 60'000);

    const auto counters = controller->counters();
    EXPECT_EQ(counters.frames_presented, 1u);
    EXPECT_EQ(counters.frames_dropped, 1u);
    EXPECT_EQ(counters.frames_delayed, 1u);
    EXPECT_EQ(counters.total_delay_us, 60'000);

    controller->close();
}

TEST_F(VideoSyncPlugin, TheAdapterHonoursTheConfiguration) {
    const auto* factory =
        nimrtc::core::PluginRegistry::instance().get_video_sync_controller(
            "render-clock");
    ASSERT_NE(factory, nullptr);

    np::VideoSyncConfig cfg;
    cfg.tolerance_us = 5'000;
    cfg.max_delay_us  = 200'000;

    auto controller = factory->create(cfg);
    ASSERT_NE(controller, nullptr);
    ASSERT_EQ(controller->open(), np::kOk);

    EXPECT_EQ(controller->tolerance_us(), 5'000);
    EXPECT_EQ(controller->max_delay_us(), 200'000);
    EXPECT_TRUE(controller->enabled());

    controller->set_enabled(false);
    EXPECT_FALSE(controller->enabled());
    EXPECT_EQ(controller->decide(5'000'000, 1).action, np::VideoSyncAction::kPresent);

    controller->close();
}

TEST_F(VideoSyncPlugin, TheAdapterForwardsTheHoldAccounting) {
    const auto* factory =
        nimrtc::core::PluginRegistry::instance().get_video_sync_controller(
            "render-clock");
    ASSERT_NE(factory, nullptr);

    np::VideoSyncConfig cfg;
    cfg.tolerance_us = 40'000;
    cfg.max_delay_us  = 2'000'000;

    auto controller = factory->create(cfg);
    ASSERT_NE(controller, nullptr);
    ASSERT_EQ(controller->open(), np::kOk);

    (void)controller->decide(1'000'000, 1);   // hold: 960 ms promised
    controller->note_delay_applied(960'000);  // the hold happened

    // The reported total is the realised hold.
    EXPECT_EQ(controller->counters().total_delay_us, 960'000);

    // A hold that never happened is reported as zero: the second hold is
    // promised (960 ms) and then reported as *not* waited for, so the
    // total falls back to what really happened.
    (void)controller->decide(1'000'000, 2);
    EXPECT_EQ(controller->counters().total_delay_us, 960'000 + 960'000);
    controller->note_delay_applied(0);
    EXPECT_EQ(controller->counters().total_delay_us, 960'000);

    controller->close();
}

TEST_F(VideoSyncPlugin, TheActionNamesAreReadable) {
    EXPECT_STREQ(np::video_sync_action_name(np::VideoSyncAction::kPresent),
                 "present");
    EXPECT_STREQ(np::video_sync_action_name(np::VideoSyncAction::kDrop),
                 "drop");
    EXPECT_STREQ(np::video_sync_action_name(np::VideoSyncAction::kDelay),
                 "delay");
}

}  // namespace
