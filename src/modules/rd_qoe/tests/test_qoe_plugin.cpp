// src/modules/rd_qoe/tests/test_qoe_plugin.cpp
// ----------------------------------------------------------------------------
// The `plugins::IQoeCollector` adapter's unit tests (plan Track J, J3 / J6).
//
// The adapter is the thin translation layer between the seam
// (`plugins::IQoeCollector`, in src/plugins/) and the module's metrics
// (`rd_qoe::QoeCollector`).  The *metrics* are tested in
// `test_qoe_metrics.cpp`; what is tested here is that the translation is
// **lossless** — that every field of the seam's snapshot carries the
// module's value, and that the registry publishes the factory under the
// id the engine resolves.
//
// The suites:
//
//   qoe_plugin_*        the seam's snapshot mirrors the module's
//   qoe_plugin_registry_* the factory is registered under "rd-qoe"
// ----------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/plugins/qoe_collector.hpp>
#include <nimrtc/rd_qoe/qoe_metrics.hpp>

#include <algorithm>
#include <cstdint>
#include <string>

namespace {

using nimrtc::plugins::FrameObservation;
using nimrtc::plugins::IQoeCollector;
using nimrtc::plugins::QoeConfig;
using nimrtc::plugins::QoeSnapshot;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// A presented frame on the *seam's* vocabulary, with a uniform
/// end-to-end latency.
FrameObservation make_frame(std::int64_t capture_ts_us,
                             std::int64_t render_ts_us,
                             std::uint64_t frame_id = 0) {
    FrameObservation obs;
    obs.capture_ts_us = capture_ts_us;
    obs.render_ts_us  = render_ts_us;
    obs.frame_id      = frame_id;
    obs.source_fps    = 30.0;
    return obs;
}

/// The same frame on the *module's* vocabulary.
nimrtc::rd_qoe::FrameObservation make_module_frame(
        std::int64_t capture_ts_us,
        std::int64_t render_ts_us,
        std::uint64_t frame_id = 0) {
    nimrtc::rd_qoe::FrameObservation obs;
    obs.capture_ts_us = capture_ts_us;
    obs.render_ts_us  = render_ts_us;
    obs.frame_id      = frame_id;
    obs.ref_frame_id  = frame_id;
    obs.source_fps    = 30.0;
    return obs;
}

/// The registered factory, registering the module's entry point first.
///
/// The registration is idempotent, so every test may call it.  It is
/// *not* done in a fixture's SetUp because the registry's content is
/// process-wide and the other suites in this binary may look the
/// factory up first.
const nimrtc::plugins::IQoeCollectorFactory* TheFactory() {
    nimrtc::rd_qoe::register_default_plugins();
    const auto* factory = nimrtc::core::PluginRegistry::instance()
                              .get_qoe_collector("rd-qoe");
    EXPECT_NE(factory, nullptr) << "the rd-qoe factory must be registered";
    return factory;
}

// ---------------------------------------------------------------------------
// The snapshot translation
// ---------------------------------------------------------------------------

TEST(QoePlugin, TheSnapshotCarriesThePresentedFrames) {
    nimrtc::rd_qoe::QoeCollector module;
    module.on_frame_presented(make_module_frame(1000, 51000, 7));

    const auto snap = module.snapshot();
    EXPECT_EQ(snap.frames_presented, 1u);
    // The 50 ms end-to-end latency.
    EXPECT_EQ(snap.latency_p50_us, 50000);
    EXPECT_EQ(snap.latency_max_us, 50000);
}

TEST(QoePlugin, TheAdapterPreservesTheModulesMetrics) {
    // Build a module-level snapshot with every field set to a
    // distinguishable value, then check that the seam's snapshot carries
    // the same values.  The adapter is a translation layer, so a
    // mismatch here is a lost field.
    nimrtc::rd_qoe::QoeCollector module;
    for (int i = 0; i < 5; ++i) {
        module.on_frame_presented(make_module_frame(
            1000 * i, 1000 * i + 40000, static_cast<std::uint64_t>(i)));
    }
    module.on_frame_dropped(3);
    module.on_freeze_detected(250);
    module.on_input_event_sent(1000, 42);
    module.on_frame_presented_for_input(9000, 42);
    module.on_sharpness_observed(12.5);

    const auto local = module.snapshot();

    // Now the same thing through the *adapter*, which is what the engine
    // sees.
    const auto* factory = TheFactory();
    ASSERT_NE(factory, nullptr);

    auto collector = factory->create(QoeConfig{});
    ASSERT_NE(collector, nullptr);
    ASSERT_EQ(collector->open(), nimrtc::plugins::kOk);

    for (int i = 0; i < 5; ++i) {
        collector->on_frame_presented(
            make_frame(1000 * i, 1000 * i + 40000, static_cast<std::uint64_t>(i)));
    }
    collector->on_frame_dropped(3);
    collector->on_freeze_detected(250);
    collector->on_input_event_sent(1000, 42);
    collector->on_frame_presented_for_input(9000, 42);
    collector->on_sharpness_observed(12.5);

    const QoeSnapshot out = collector->snapshot();

    EXPECT_EQ(out.frames_presented, local.frames_presented);
    EXPECT_EQ(out.frames_dropped, local.frames_dropped);
    EXPECT_EQ(out.freeze_count, local.freeze_count);
    EXPECT_EQ(out.freeze_total_ms, local.freeze_total_ms);
    EXPECT_EQ(out.freeze_longest_ms, local.freeze_longest_ms);
    EXPECT_EQ(out.latency_p50_us, local.latency_p50_us);
    EXPECT_EQ(out.latency_p95_us, local.latency_p95_us);
    EXPECT_EQ(out.latency_p99_us, local.latency_p99_us);
    EXPECT_EQ(out.latency_max_us, local.latency_max_us);
    EXPECT_EQ(out.input_to_photon_p50_us, local.input_to_photon_p50_us);
    EXPECT_EQ(out.input_to_photon_p95_us, local.input_to_photon_p95_us);
    EXPECT_EQ(out.input_to_photon_max_us, local.input_to_photon_max_us);
    EXPECT_DOUBLE_EQ(out.sharpness_proxy, local.sharpness_proxy);
    EXPECT_DOUBLE_EQ(out.freeze_ratio, local.freeze_ratio);
    EXPECT_EQ(out.session_duration_ms, local.session_duration_ms);
    EXPECT_DOUBLE_EQ(out.source_fps, local.source_fps);

    // And the values are *sane*, not just equal.
    EXPECT_EQ(out.frames_presented, 5u);
    EXPECT_EQ(out.frames_dropped, 3u);
    EXPECT_EQ(out.freeze_count, 1u);
    EXPECT_EQ(out.freeze_total_ms, 250);
    EXPECT_EQ(out.latency_p50_us, 40000);
    EXPECT_EQ(out.input_to_photon_p50_us, 8000);
}

TEST(QoePlugin, TheAdapterForwardsTheSharpnessProxy) {
    const auto* factory = TheFactory();
    ASSERT_NE(factory, nullptr);
    auto collector = factory->create(QoeConfig{});
    ASSERT_NE(collector, nullptr);

    // A 8x8 plane with a hard edge in the middle.
    std::uint8_t plane[64] = {};
    for (int y = 0; y < 8; ++y) {
        for (int x = 4; x < 8; ++x) {
            plane[y * 8 + x] = 255;
        }
    }

    const double value = collector->observe_sharpness(plane, 8, 8, 8);
    EXPECT_GT(value, 0.0);

    const QoeSnapshot out = collector->snapshot();
    EXPECT_DOUBLE_EQ(out.sharpness_proxy, value);
}

TEST(QoePlugin, TheAdapterResets) {
    const auto* factory = TheFactory();
    ASSERT_NE(factory, nullptr);
    auto collector = factory->create(QoeConfig{});
    ASSERT_NE(collector, nullptr);

    collector->on_frame_presented(make_frame(0, 10000, 1));
    EXPECT_EQ(collector->snapshot().frames_presented, 1u);

    collector->reset();
    EXPECT_EQ(collector->snapshot().frames_presented, 0u);
}

// ---------------------------------------------------------------------------
// The registration
// ---------------------------------------------------------------------------

TEST(QoePluginRegistry, TheFactoryIsRegisteredUnderItsId) {
    nimrtc::rd_qoe::register_default_plugins();
    // Idempotent.
    nimrtc::rd_qoe::register_default_plugins();

    const auto* factory =
        nimrtc::core::PluginRegistry::instance().get_qoe_collector("rd-qoe");
    ASSERT_NE(factory, nullptr);
    EXPECT_EQ(factory->id(), "rd-qoe");
    EXPECT_FALSE(factory->display_name().empty());

    const auto ids = nimrtc::core::PluginRegistry::instance().list_qoe_collectors();
    EXPECT_NE(std::find(ids.begin(), ids.end(), std::string("rd-qoe")), ids.end());
}

TEST(QoePluginRegistry, TheFactoryCreatesUsableCollectors) {
    nimrtc::rd_qoe::register_default_plugins();
    const auto* factory =
        nimrtc::core::PluginRegistry::instance().get_qoe_collector("rd-qoe");
    ASSERT_NE(factory, nullptr);

    auto collector = factory->create(QoeConfig{});
    ASSERT_NE(collector, nullptr);
    EXPECT_NE(collector->name(), nullptr);
    EXPECT_EQ(collector->open(), nimrtc::plugins::kOk);
    collector->close();
}

}  // namespace
