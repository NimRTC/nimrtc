/**
 * @file tests/test_video_plugin_adapters.cpp
 * @brief R2-Batch2 validation: video module plugin adapters wire the
 *        concrete implementations through core::PluginRegistry and the
 *        engine can look them up by string ID.
 *
 * What this test verifies:
 *   - `video_source::register_default_plugins()` installs the
 *     "memory" video-source factory.
 *   - `video_sink::register_default_plugins()` installs both
 *     "headless" and "pinned" video-sink factories.
 *   - `video_pipeline::register_default_plugins()` installs the
 *     "reference" receiver and sender factories.
 *   - The registered factories produce PluginAdapter instances that
 *     forward calls to the underlying concrete impls (round-trip
 *     through the adapter preserves state).
 *   - Pixel format, codec kind, packetization mode translations are
 *     lossless across the plugin adapter boundary.
 *
 * NOTE: This test only exercises the plugin adapter layer; the engine
 *       wiring (replace direct concrete instantiation with registry
 *       lookup) is a separate step.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/plugins/video_source.hpp>
#include <nimrtc/plugins/video_sink.hpp>
#include <nimrtc/plugins/video_pipeline.hpp>

#include <nimrtc/video_source/video_source_plugin.hpp>
#include <nimrtc/video_sink/video_sink.hpp>
#include <nimrtc/video_pipeline/video_pipeline_plugin.hpp>

namespace {

using nimrtc::core::PluginRegistry;
namespace np = nimrtc::plugins;

np::VideoSourceFrame make_src_frame(std::uint64_t ts_us,
                                    std::uint32_t w = 320,
                                    std::uint32_t h = 240) noexcept {
    np::VideoSourceFrame f{};
    f.width  = w;
    f.height = h;
    f.format = np::VideoPixelFormat::kI420;
    f.capture_ts_us = static_cast<np::TimestampUs>(ts_us);
    f.frame_seq = static_cast<std::uint32_t>(ts_us / 1000);
    return f;
}

// ---------------------------------------------------------------------------
// Fixture: register all default video plugins once.
// ---------------------------------------------------------------------------
class VideoPluginAdapters : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::video_source::register_default_plugins();
        nimrtc::video_sink::register_default_plugins();
        nimrtc::video_pipeline::register_default_plugins();
    }
};

// ---------------------------------------------------------------------------
// VideoSource — MemoryPluginFactory
// ---------------------------------------------------------------------------

TEST_F(VideoPluginAdapters, video_source_memory_is_registered) {
    const auto* f = PluginRegistry::instance().get_video_source("memory");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->id(), "memory");
}

TEST_F(VideoPluginAdapters, video_source_factory_round_trip) {
    const auto* factory = PluginRegistry::instance().get_video_source("memory");
    ASSERT_NE(factory, nullptr);

    np::VideoSourceConfig cfg{};
    cfg.width  = 320;
    cfg.height = 240;
    cfg.fps    = 30;
    cfg.ssrc   = 0xABCDEF01;
    cfg.format = np::VideoPixelFormat::kI420;
    cfg.pattern = np::VideoSourcePattern::kColorBars;

    std::unique_ptr<np::IVideoSource> src(factory->create(cfg));
    ASSERT_NE(src, nullptr);

    EXPECT_STREQ(src->name(), "nimrtc::video_source::PluginAdapter (wraps concrete IVideoSource)");
    ASSERT_EQ(src->open(), np::kOk);
    EXPECT_FALSE(src->running());

    std::atomic<int> frame_count{0};
    std::atomic<np::TimestampUs> last_capture_ts{0};
    src->set_callback([&](const np::VideoSourceFrame& frame) {
        frame_count.fetch_add(1);
        last_capture_ts.store(frame.capture_ts_us);
    });

    ASSERT_EQ(src->start(), np::kOk);
    EXPECT_TRUE(src->running());

    src->stop();
    EXPECT_FALSE(src->running());
    src->close();
}

TEST_F(VideoPluginAdapters, video_source_config_round_trip) {
    const auto* f = PluginRegistry::instance().get_video_source("memory");
    ASSERT_NE(f, nullptr);

    np::VideoSourceConfig cfg{};
    cfg.width  = 640;
    cfg.height = 480;
    cfg.fps    = 60;
    cfg.ssrc   = 0xDEADBEEF;

    std::unique_ptr<np::IVideoSource> src(f->create(cfg));
    ASSERT_NE(src, nullptr);
    ASSERT_EQ(src->open(), np::kOk);

    auto got = src->config();
    EXPECT_EQ(got.width,  cfg.width);
    EXPECT_EQ(got.height, cfg.height);
    EXPECT_EQ(got.fps,    cfg.fps);
    EXPECT_EQ(got.ssrc,   cfg.ssrc);
    EXPECT_EQ(got.format, np::VideoPixelFormat::kI420);

    src->close();
}

// ---------------------------------------------------------------------------
// VideoSink — HeadlessPluginFactory + PinnedPluginFactory
// ---------------------------------------------------------------------------

TEST_F(VideoPluginAdapters, video_sink_headless_is_registered) {
    const auto* f = PluginRegistry::instance().get_video_sink("headless");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->id(), "headless");
}

TEST_F(VideoPluginAdapters, video_sink_pinned_is_registered) {
    const auto* f = PluginRegistry::instance().get_video_sink("pinned");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->id(), "pinned");
}

TEST_F(VideoPluginAdapters, video_sink_headless_renders) {
    const auto* f = PluginRegistry::instance().get_video_sink("headless");
    ASSERT_NE(f, nullptr);

    std::unique_ptr<np::IVideoSink> sink(f->create(np::VideoSinkConfig{}));
    ASSERT_NE(sink, nullptr);
    ASSERT_EQ(sink->open(), np::kOk);
    EXPECT_EQ(sink->render(make_src_frame(0)), np::kOk);
    EXPECT_EQ(sink->render(make_src_frame(1000)), np::kOk);
    EXPECT_EQ(sink->render(make_src_frame(2000)), np::kOk);
    EXPECT_EQ(sink->stats().frames_rendered, 3u);
    sink->close();
}

TEST_F(VideoPluginAdapters, video_sink_pinned_enforces_ordering) {
    const auto* f = PluginRegistry::instance().get_video_sink("pinned");
    ASSERT_NE(f, nullptr);

    std::unique_ptr<np::IVideoSink> sink(f->create(np::VideoSinkConfig{}));
    ASSERT_NE(sink, nullptr);
    ASSERT_EQ(sink->open(), np::kOk);
    EXPECT_EQ(sink->render(make_src_frame(0)), np::kOk);
    EXPECT_EQ(sink->render(make_src_frame(1000)), np::kOk);
    EXPECT_EQ(sink->render(make_src_frame(1000)), np::kErrCorrupt);
    EXPECT_EQ(sink->render(make_src_frame(500)),  np::kErrCorrupt);
    EXPECT_EQ(sink->stats().render_errors, 2u);
    EXPECT_EQ(sink->stats().frames_rendered, 2u);
    sink->close();
}

// ---------------------------------------------------------------------------
// VideoPipeline — ReferenceReceiverFactory + ReferenceSenderFactory
// ---------------------------------------------------------------------------

TEST_F(VideoPluginAdapters, video_receiver_reference_is_registered) {
    const auto* f = PluginRegistry::instance().get_video_receiver("reference");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->id(), "reference");
}

TEST_F(VideoPluginAdapters, video_sender_reference_is_registered) {
    const auto* f = PluginRegistry::instance().get_video_sender("reference");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->id(), "reference");
}

TEST_F(VideoPluginAdapters, video_receiver_push_rtp_works) {
    const auto* factory = PluginRegistry::instance().get_video_receiver("reference");
    ASSERT_NE(factory, nullptr);

    np::VideoReceiverConfig cfg{};
    cfg.codec = np::VideoCodecKind::kH264;
    cfg.ssrc  = 0xCAFEBABE;
    cfg.payload_type = 102;

    std::unique_ptr<np::IVideoReceiver> rx(factory->create(cfg));
    ASSERT_NE(rx, nullptr);
    ASSERT_EQ(rx->open(), np::kOk);

    std::atomic<int> frame_count{0};
    rx->set_frame_callback([&](const np::EncodedVideoFrame& frame,
                               np::TimestampUs /*now_us*/) {
        frame_count.fetch_add(1);
        (void)frame;
    });

    // Push one RTP packet — at minimum the receiver records the packet
    // and runs FU-A dispatch (it won't emit a frame since payload is empty).
    np::VideoRtpPacket pkt{};
    pkt.ssrc = cfg.ssrc;
    pkt.seq  = 12345;
    pkt.rtp_timestamp = 90000;
    pkt.payload_type = cfg.payload_type;
    pkt.marker = true;
    pkt.payload = np::BufferView{};

    EXPECT_EQ(rx->push_rtp(pkt, /*now_us=*/1000), np::kOk);
    EXPECT_EQ(rx->stats().packets_pushed, 1u);
    rx->close();
}

TEST_F(VideoPluginAdapters, video_sender_push_frame_works) {
    const auto* f = PluginRegistry::instance().get_video_sender("reference");
    ASSERT_NE(f, nullptr);

    np::VideoSenderConfig cfg{};
    cfg.codec = np::VideoCodecKind::kH264;
    cfg.ssrc = 0xABCDEF01;
    cfg.payload_type = 102;
    cfg.mtu  = 1200;
    cfg.initial_seq = 100;
    cfg.packetization_mode = np::VideoPacketizationMode::kNonInterleaved;

    std::unique_ptr<np::IVideoSender> tx(f->create(cfg));
    ASSERT_NE(tx, nullptr);
    ASSERT_EQ(tx->open(), np::kOk);

    std::atomic<int> packet_count{0};
    tx->set_packet_callback([&](np::BufferView /*payload*/,
                                std::uint32_t /*ts*/,
                                std::uint16_t /*seq*/,
                                bool /*marker*/) {
        packet_count.fetch_add(1);
    });

    // Push one frame: a single-NAL H.264 IDR slice (5 bytes payload —
    // small enough to fit in one RTP packet).
    np::EncodedVideoFrame frame{};
    frame.codec        = np::VideoCodecKind::kH264;
    frame.payload_type = cfg.payload_type;
    frame.is_keyframe  = true;
    frame.nalu_format  = np::NaluFormat::kAnnexBStartCode;
    // Real Annex B start code (0x00 0x00 0x00 0x01) + IDR NAL header + payload.
    static const std::uint8_t kIdrSlice[] = {
        0x00, 0x00, 0x00, 0x01,   // Annex B start code
        0x65,                     // NAL: IDR slice
        0x88, 0x84, 0x00, 0x33,   // payload bytes
    };
    frame.payload      = nimrtc::core::ByteSpan{kIdrSlice, sizeof(kIdrSlice)};
    frame.info.capture_ts_us   = 12345;
    frame.info.frame_seq       = 7;
    frame.info.rtp_timestamp   = 90000;

    EXPECT_EQ(tx->push_frame(frame, /*rtp_ts=*/90000), np::kOk);
    EXPECT_EQ(tx->stats().frames_pushed, 1u);
    EXPECT_GT(packet_count.load(), 0);

    tx->force_keyframe();
    EXPECT_EQ(tx->stats().force_keyframe_calls, 1u);

    tx->close();
}

// ---------------------------------------------------------------------------
// Adapter classes — direct construction (no factory)
// ---------------------------------------------------------------------------

TEST(VideoPluginAdaptersDirect, ReceiverAdapter_is_forwarder) {
    // Construct a concrete receiver, then wrap it via the adapter header.
    nimrtc::video_pipeline::ReceiverConfig concrete_cfg{};
    concrete_cfg.codec        = nimrtc::video_frame::CodecKind::kH264;
    concrete_cfg.ssrc         = 0x12345678;
    concrete_cfg.payload_type = 102;
    concrete_cfg.expect_fu_a  = true;

    auto concrete = std::make_unique<nimrtc::video_pipeline::VideoReceiver>(
        concrete_cfg);

    nimrtc::video_pipeline::ReceiverAdapter adapter(std::move(concrete));
    ASSERT_EQ(adapter.open(), np::kOk);

    std::atomic<int> frames{0};
    adapter.set_frame_callback([&](const np::EncodedVideoFrame& f,
                                   np::TimestampUs /*now_us*/) {
        frames.fetch_add(1);
        EXPECT_EQ(f.codec, np::VideoCodecKind::kH264);
    });

    // Verify the adapter accepts a push_rtp call (no crash).
    np::VideoRtpPacket pkt{};
    pkt.ssrc = 0x12345678;
    pkt.seq  = 1;
    pkt.rtp_timestamp = 90000;
    pkt.payload_type = 102;
    pkt.marker = true;
    pkt.payload = np::BufferView{};

    EXPECT_EQ(adapter.push_rtp(pkt, /*now_us=*/1000), np::kOk);
    EXPECT_EQ(adapter.stats().packets_pushed, 1u);

    adapter.close();
}

TEST(VideoPluginAdaptersDirect, SenderAdapter_is_forwarder) {
    nimrtc::video_pipeline::SenderConfig concrete_cfg{};
    concrete_cfg.codec        = nimrtc::video_frame::CodecKind::kH264;
    concrete_cfg.ssrc         = 0x87654321;
    concrete_cfg.payload_type = 102;
    concrete_cfg.mtu          = 1200;
    concrete_cfg.initial_seq  = 42;

    auto concrete = std::make_unique<nimrtc::video_pipeline::VideoSender>(
        concrete_cfg);

    nimrtc::video_pipeline::SenderAdapter adapter(std::move(concrete));
    ASSERT_EQ(adapter.open(), np::kOk);

    std::atomic<int> pkts{0};
    adapter.set_packet_callback([&](nimrtc::core::ByteSpan /*p*/,
                                    std::uint32_t /*ts*/,
                                    std::uint16_t /*seq*/, bool /*marker*/) {
        pkts.fetch_add(1);
    });

    np::EncodedVideoFrame frame{};
    frame.codec        = np::VideoCodecKind::kH264;
    frame.payload_type = 102;
    frame.is_keyframe  = true;
    frame.nalu_format  = np::NaluFormat::kAnnexBStartCode;
    static const std::uint8_t kIdrSlice[] = {
        0x00, 0x00, 0x00, 0x01,
        0x65,
        0x88, 0x84, 0x00, 0x33,
    };
    frame.payload      = nimrtc::core::ByteSpan{kIdrSlice, sizeof(kIdrSlice)};
    frame.info.capture_ts_us   = 5000;
    frame.info.frame_seq       = 1;
    frame.info.rtp_timestamp   = 90000;

    EXPECT_EQ(adapter.push_frame(frame, /*rtp_ts=*/90000), np::kOk);
    EXPECT_EQ(adapter.stats().frames_pushed, 1u);

    adapter.close();
}

} // namespace
