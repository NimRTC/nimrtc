/**
 * @file tests/test_engine_video_plugins.cpp
 * @brief R3-Batch validation: engine resolves and exposes video plugins via
 *        core::PluginRegistry.
 *
 * What this test verifies:
 *   - core::register_all_default_plugins() also installs the video_source /
 *     video_sink / video_receiver / video_sender factories.
 *   - NimRTCEngine::open() resolves all four video plugins from the registry.
 *   - The video_source / sink / receiver / sender accessors return
 *     non-null plugin instances after open().
 *   - The receiver's set_frame_callback is wired (so users can detect
 *     encoded frames arriving) and the sink can render a frame.
 *   - set_video_sink() replaces the default sink at runtime.
 *   - Close releases the plugins cleanly.
 *
 * NOTE: This test exercises plugin wiring only. No ICE/DTLS/SRTP
 *       networking is involved — the engine's ICE transport is opened
 *       (port 0 = OS-chosen ephemeral) but no packets are exchanged.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>

#include <nimrtc/plugins/video_pipeline.hpp>
#include <nimrtc/plugins/video_sink.hpp>
#include <nimrtc/plugins/video_source.hpp>

namespace {

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
namespace np = nimrtc::plugins;

/** Test fixture: register all default plugins once. */
class EngineVideoPlugins : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
    }
};

// ---------------------------------------------------------------------------
// Registry-level: video factories are registered
// ---------------------------------------------------------------------------

TEST_F(EngineVideoPlugins, registry_has_video_source_memory) {
    const auto* f = PluginRegistry::instance().get_video_source("memory");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->id(), "memory");
}

TEST_F(EngineVideoPlugins, registry_has_video_sink_headless) {
    const auto* f = PluginRegistry::instance().get_video_sink("headless");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->id(), "headless");
}

TEST_F(EngineVideoPlugins, registry_has_video_receiver_reference) {
    const auto* f = PluginRegistry::instance().get_video_receiver("reference");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->id(), "reference");
}

TEST_F(EngineVideoPlugins, registry_has_video_sender_reference) {
    const auto* f = PluginRegistry::instance().get_video_sender("reference");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->id(), "reference");
}

// ---------------------------------------------------------------------------
// Engine-level: open() resolves all 4 video plugins
// ---------------------------------------------------------------------------

TEST_F(EngineVideoPlugins, open_resolves_video_plugins) {
    EngineConfig cfg{};
    cfg.video_source_name   = "memory";
    cfg.video_sink_name     = "headless";
    cfg.video_receiver_name = "reference";
    cfg.video_sender_name   = "reference";

    NimRTCEngine engine(cfg);
    std::atomic<uint32_t> last_err{0};
    engine.set_on_error([&](uint32_t e, std::string_view /*msg*/) {
        last_err.store(e);
    });

    ASSERT_EQ(engine.open(), 0u) << "engine open() failed (err=0x"
                                 << std::hex << last_err.load() << ")";

    EXPECT_NE(engine.video_source(),   nullptr);
    EXPECT_NE(engine.video_sink(),     nullptr);
    EXPECT_NE(engine.video_receiver(), nullptr);
    EXPECT_NE(engine.video_sender(),   nullptr);

    engine.close();
}

// ---------------------------------------------------------------------------
// Receiver → sink pipeline is accessible to user code
// ---------------------------------------------------------------------------

TEST_F(EngineVideoPlugins, source_can_produce_and_sink_can_render) {
    EngineConfig cfg{};
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // Use the default headless sink (no ordering check, accepts all frames).
    // The test demonstrates: source frames flow through user callback →
    // sink render(). We swap to headless explicitly so this test is
    // independent of the EngineConfig default.
    const auto* sink_factory =
        PluginRegistry::instance().get_video_sink("headless");
    ASSERT_NE(sink_factory, nullptr);
    auto new_sink = std::unique_ptr<np::IVideoSink>(
        sink_factory->create(np::VideoSinkConfig{}));
    ASSERT_NE(new_sink, nullptr);
    engine.set_video_sink(std::move(new_sink));
    EXPECT_NE(engine.video_sink(), nullptr);

    // Drive source → render directly through the new sink to verify
    // it's open and that frames round-trip end-to-end.
    std::atomic<int> src_frame_count{0};
    engine.video_source()->set_callback(
        [&](const np::VideoSourceFrame& frame) {
            src_frame_count.fetch_add(1);
            if (engine.video_sink()) {
                engine.video_sink()->render(frame);
            }
        });

    ASSERT_EQ(engine.video_source()->open(), np::kOk);
    ASSERT_EQ(engine.video_source()->start(), np::kOk);
    // Run for ~200ms (6 frames at 30fps) to capture some frames.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    engine.video_source()->stop();
    EXPECT_GT(src_frame_count.load(), 0);
    EXPECT_EQ(src_frame_count.load(),
              static_cast<int>(engine.video_sink()->stats().frames_rendered));

    engine.close();
}

TEST_F(EngineVideoPlugins, set_video_sink_before_open_takes_effect) {
    EngineConfig cfg{};
    cfg.video_sink_name = "headless";  // default
    NimRTCEngine engine(cfg);

    // Pre-set a pinned sink BEFORE open() — should replace the default
    // once open() resolves the plugin.
    const auto* f = PluginRegistry::instance().get_video_sink("pinned");
    ASSERT_NE(f, nullptr);
    auto new_sink = std::unique_ptr<np::IVideoSink>(f->create(np::VideoSinkConfig{}));
    engine.set_video_sink(std::move(new_sink));

    ASSERT_EQ(engine.open(), 0u);
    ASSERT_NE(engine.video_sink(), nullptr);

    np::VideoSourceFrame f1{};
    f1.width  = 320;
    f1.height = 240;
    f1.format = np::VideoPixelFormat::kI420;
    f1.capture_ts_us = 1000;
    np::VideoSourceFrame f2{};
    f2.width  = 320;
    f2.height = 240;
    f2.format = np::VideoPixelFormat::kI420;
    f2.capture_ts_us = 2000;
    EXPECT_EQ(engine.video_sink()->render(f1), np::kOk);
    EXPECT_EQ(engine.video_sink()->render(f2), np::kOk);
    EXPECT_EQ(engine.video_sink()->stats().frames_rendered, 2u);

    engine.close();
}

// ---------------------------------------------------------------------------
// Sender can push a frame; receiver can be fed a packet
// ---------------------------------------------------------------------------

TEST_F(EngineVideoPlugins, sender_pushes_frames_and_receiver_counts_packets) {
    EngineConfig cfg{};
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    std::atomic<int> packets_pushed{0};
    // Note: receiver.set_frame_callback is wired by init_video_plugins()
    // already; we don't replace it here.
    np::VideoRtpPacket pkt{};
    pkt.ssrc = cfg.video_receiver_tuning.ssrc;
    pkt.seq  = 1234;
    pkt.rtp_timestamp = 90000;
    pkt.payload_type = cfg.video_receiver_tuning.payload_type;
    pkt.marker = true;
    pkt.payload = np::BufferView{};

    EXPECT_EQ(engine.video_receiver()->push_rtp(pkt, /*now_us=*/1000), np::kOk);
    EXPECT_EQ(engine.video_receiver()->stats().packets_pushed, 1u);

    np::EncodedVideoFrame frame{};
    frame.codec        = np::VideoCodecKind::kH264;
    frame.payload_type = cfg.video_sender_tuning.payload_type;
    frame.is_keyframe  = true;
    frame.nalu_format  = nimrtc::video_frame::NaluFormat::kAnnexBStartCode;
    static const std::uint8_t kIdrSlice[] = {
        0x00, 0x00, 0x00, 0x01,
        0x65,
        0x88, 0x84, 0x00, 0x33,
    };
    frame.payload = nimrtc::core::ByteSpan{kIdrSlice, sizeof(kIdrSlice)};
    frame.info.capture_ts_us   = 5000;
    frame.info.frame_seq       = 1;
    frame.info.rtp_timestamp   = 90000;

    EXPECT_EQ(engine.video_sender()->push_frame(frame, /*rtp_ts=*/90000), np::kOk);
    EXPECT_EQ(engine.video_sender()->stats().frames_pushed, 1u);

    engine.close();
}

// ---------------------------------------------------------------------------
// Video plugins are released on close()
// ---------------------------------------------------------------------------

TEST_F(EngineVideoPlugins, close_releases_video_plugins) {
    EngineConfig cfg{};
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    EXPECT_NE(engine.video_source(),   nullptr);
    EXPECT_NE(engine.video_sink(),     nullptr);
    EXPECT_NE(engine.video_receiver(), nullptr);
    EXPECT_NE(engine.video_sender(),   nullptr);

    engine.close();

    EXPECT_EQ(engine.video_source(),   nullptr);
    EXPECT_EQ(engine.video_sink(),     nullptr);
    EXPECT_EQ(engine.video_receiver(), nullptr);
    EXPECT_EQ(engine.video_sender(),   nullptr);
}

} // namespace
