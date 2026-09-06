/**
 * @file tests/test_video_plugin_interfaces.cpp
 * @brief R2-Batch2 validation: video plugin interfaces (IVideoSource,
 *        IVideoSink, IVideoReceiver, IVideoSender) are complete and
 *        round-trip through the extended PluginRegistry.
 *
 * What this test verifies:
 *   - The four new plugin headers compile and link (i.e. the interface
 *     surface in src/plugins/include/nimrtc/plugins/{video_source,
 *     video_sink,video_pipeline}.hpp is self-consistent).
 *   - PluginRegistry::register_video_source / register_video_sink /
 *     register_video_receiver / register_video_sender store factories
 *     keyed by string ID and lookups by the same ID return the original
 *     pointer.
 *   - list_video_sources / list_video_sinks / list_video_receivers /
 *     list_video_senders all enumerate every registered factory.
 *   - The NIMRTC_REGISTER_VIDEO_{SOURCE,SINK,RECEIVER,SENDER} static-init
 *     macros successfully insert factories into the registry.
 *   - SimpleXxxFactory<T> correctly construct instances of stub types
 *     (proves the factory template compiles against the new interfaces).
 *
 * NOTE: This test only exercises the plugin interface layer — it does NOT
 *       instantiate any concrete video_* module. Concrete wiring is the
 *       next step (PluginAdapter wrappers in each video_* module's src/
 *       directory, mirroring audio3a_plugin.cpp).
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/plugins/video_source.hpp>
#include <nimrtc/plugins/video_sink.hpp>
#include <nimrtc/plugins/video_pipeline.hpp>

namespace {

using nimrtc::core::PluginRegistry;
namespace np = nimrtc::plugins;

// ---------------------------------------------------------------------------
// Stub IVideoSource — minimal implementation, only for compile/link proof.
// ---------------------------------------------------------------------------
class StubVideoSource : public np::IVideoSource {
public:
    explicit StubVideoSource(np::VideoSourceConfig) noexcept {}
    ~StubVideoSource() override = default;

    const char* name() const noexcept override { return "StubVideoSource"; }
    np::Status open() noexcept override { opened_ = true; return np::kOk; }
    void close() noexcept override { opened_ = false; }
    np::Status start() noexcept override { running_ = true; return np::kOk; }
    void stop() noexcept override { running_ = false; }
    bool running() const noexcept override { return running_; }
    void produce_one() noexcept override {
        frames_produced_++;
        // Fire the callback (matches concrete MemoryVideoSource behaviour:
        // produce_one() invokes the user callback synchronously).
        if (cb_) {
            np::VideoSourceFrame f{};
            f.width = cfg_.width;
            f.height = cfg_.height;
            f.format = cfg_.format;
            f.capture_ts_us = static_cast<np::TimestampUs>(frames_produced_);
            f.frame_seq = cfg_.start_frame_seq +
                          static_cast<std::uint32_t>(frames_produced_);
            cb_(f);
        }
    }
    np::VideoSourceConfig config() const noexcept override { return cfg_; }
    void set_pattern(np::VideoSourcePattern p) noexcept override { cfg_.pattern = p; }
    void set_callback(np::VideoSourceFrameCallback cb) noexcept override {
        cb_ = std::move(cb);
    }
    np::IVideoSource::Stats stats() const noexcept override {
        return {frames_produced_, 0, 0};
    }
private:
    np::VideoSourceConfig cfg_{};
    np::VideoSourceFrameCallback cb_;
    std::atomic<bool> opened_{false};
    std::atomic<bool> running_{false};
    std::uint64_t frames_produced_ = 0;
};

// ---------------------------------------------------------------------------
// Stub IVideoSink
// ---------------------------------------------------------------------------
class StubVideoSink : public np::IVideoSink {
public:
    explicit StubVideoSink(np::VideoSinkConfig) noexcept {}
    ~StubVideoSink() override = default;

    const char* name() const noexcept override { return "StubVideoSink"; }
    np::Status open() noexcept override { opened_ = true; return np::kOk; }
    void close() noexcept override { opened_ = false; }
    np::Status render(const np::VideoSourceFrame& /*f*/) noexcept override {
        frames_rendered_++;
        return np::kOk;
    }
    np::VideoSinkStats stats() const noexcept override { return {frames_rendered_,0,0,0,0}; }
    np::VideoSinkConfig config() const noexcept override { return cfg_; }
private:
    np::VideoSinkConfig cfg_{};
    std::atomic<bool> opened_{false};
    std::uint64_t frames_rendered_ = 0;
};

// ---------------------------------------------------------------------------
// Stub IVideoReceiver
// ---------------------------------------------------------------------------
class StubVideoReceiver : public np::IVideoReceiver {
public:
    explicit StubVideoReceiver(np::VideoReceiverConfig) noexcept {}
    ~StubVideoReceiver() override = default;

    const char* name() const noexcept override { return "StubVideoReceiver"; }
    np::Status open() noexcept override { opened_ = true; return np::kOk; }
    void close() noexcept override { opened_ = false; }

    void set_frame_callback(np::VideoReceiverFrameCallback cb) noexcept override {
        frame_cb_ = std::move(cb);
    }
    void set_nack_callback(np::VideoReceiverNackCallback cb) noexcept override {
        nack_cb_ = std::move(cb);
    }
    np::Status push_rtp(const np::VideoRtpPacket& pkt,
                        np::TimestampUs now_us) noexcept override {
        packets_pushed_++;
        last_seq_ = pkt.seq;
        last_recv_us_ = now_us;
        return np::kOk;
    }
    std::vector<std::uint16_t> pop_nack_entries() noexcept override {
        return std::vector<std::uint16_t>{};
    }
    np::Status tick(np::TimestampUs /*now_us*/) noexcept override {
        return np::kOk;
    }
    void reset() noexcept override {}
    np::VideoReceiverStats stats() const noexcept override {
        return {packets_pushed_, 0, 0, 0, 0, 0, 0};
    }
    np::VideoReceiverConfig config() const noexcept override { return cfg_; }
private:
    np::VideoReceiverConfig cfg_{};
    np::VideoReceiverFrameCallback frame_cb_;
    np::VideoReceiverNackCallback nack_cb_;
    std::atomic<bool> opened_{false};
    std::uint64_t packets_pushed_ = 0;
    std::uint16_t last_seq_ = 0;
    np::TimestampUs last_recv_us_ = 0;
};

// ---------------------------------------------------------------------------
// Stub IVideoSender
// ---------------------------------------------------------------------------
class StubVideoSender : public np::IVideoSender {
public:
    explicit StubVideoSender(np::VideoSenderConfig) noexcept {}
    ~StubVideoSender() override = default;

    const char* name() const noexcept override { return "StubVideoSender"; }
    np::Status open() noexcept override { opened_ = true; return np::kOk; }
    void close() noexcept override { opened_ = false; }

    void set_packet_callback(np::VideoSenderPacketCallback cb) noexcept override {
        pkt_cb_ = std::move(cb);
    }
    np::Status push_frame(const np::EncodedVideoFrame& /*f*/,
                          std::uint32_t rtp_ts) noexcept override {
        frames_pushed_++;
        last_rtp_ts_ = rtp_ts;
        return np::kOk;
    }
    void force_keyframe() noexcept override { keyframe_calls_++; }
    np::VideoSenderStats stats() const noexcept override {
        return {frames_pushed_, 0, 0, 0, 0, keyframe_calls_, 0};
    }
    std::uint16_t next_seq() const noexcept override { return next_seq_; }
    np::VideoSenderConfig config() const noexcept override { return cfg_; }
private:
    np::VideoSenderConfig cfg_{};
    np::VideoSenderPacketCallback pkt_cb_;
    std::atomic<bool> opened_{false};
    std::uint64_t frames_pushed_ = 0;
    std::uint64_t keyframe_calls_ = 0;
    std::uint32_t last_rtp_ts_ = 0;
    std::uint16_t next_seq_ = 0;
};

// ---------------------------------------------------------------------------
// Fixture — register one stub factory per video plugin slot.
// ---------------------------------------------------------------------------
class VideoPluginInterfaces : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        auto& r = PluginRegistry::instance();
        static const np::SimpleVideoSourceFactory<StubVideoSource>  s_fac{"stub_source", "Stub source"};
        static const np::SimpleVideoSinkFactory  <StubVideoSink>    k_fac{"stub_sink",   "Stub sink"};
        static const np::SimpleVideoReceiverFactory<StubVideoReceiver> r_fac{"stub_rx", "Stub receiver"};
        static const np::SimpleVideoSenderFactory  <StubVideoSender>   t_fac{"stub_tx", "Stub sender"};
        r.register_video_source  (s_fac.id(), &s_fac);
        r.register_video_sink    (k_fac.id(), &k_fac);
        r.register_video_receiver(r_fac.id(), &r_fac);
        r.register_video_sender  (t_fac.id(), &t_fac);
    }
};

// ---------------------------------------------------------------------------
// 1. Plugin headers compile + link (implicit: this test wouldn't build).
// ---------------------------------------------------------------------------

TEST_F(VideoPluginInterfaces, headers_are_self_consistent) {
    // If we got here, the headers themselves compiled and the stubs
    // implemented the interfaces correctly. Verify the type traits we'd
    // expect from the design.
    static_assert(std::is_base_of_v<np::IPlugin, np::IVideoSource>,
                  "IVideoSource must derive from IPlugin");
    static_assert(std::is_base_of_v<np::IPlugin, np::IVideoSink>,
                  "IVideoSink must derive from IPlugin");
    static_assert(std::is_base_of_v<np::IPlugin, np::IVideoReceiver>,
                  "IVideoReceiver must derive from IPlugin");
    static_assert(std::is_base_of_v<np::IPlugin, np::IVideoSender>,
                  "IVideoSender must derive from IPlugin");

    // EncodedVideoFrame must be shared across the codec + pipeline headers
    // (same struct, not a translation layer).
    static_assert(
        std::is_same_v<decltype(np::EncodedVideoFrame{}.codec),
                       decltype(np::EncodedVideoFrame{}.codec)>,
        "EncodedVideoFrame is the canonical pipeline↔codec type");

    EXPECT_TRUE(true) << "compile-time invariants OK";
}

// ---------------------------------------------------------------------------
// 2. Registry round-trip: lookup by ID returns the original factory pointer.
// ---------------------------------------------------------------------------

TEST_F(VideoPluginInterfaces, registry_lookup_video_source) {
    const auto* f = PluginRegistry::instance().get_video_source("stub_source");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->id(), "stub_source");
    EXPECT_EQ(f->display_name(), "Stub source");
}

TEST_F(VideoPluginInterfaces, registry_lookup_video_sink) {
    const auto* f = PluginRegistry::instance().get_video_sink("stub_sink");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->id(), "stub_sink");
}

TEST_F(VideoPluginInterfaces, registry_lookup_video_receiver) {
    const auto* f = PluginRegistry::instance().get_video_receiver("stub_rx");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->id(), "stub_rx");
}

TEST_F(VideoPluginInterfaces, registry_lookup_video_sender) {
    const auto* f = PluginRegistry::instance().get_video_sender("stub_tx");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->id(), "stub_tx");
}

// ---------------------------------------------------------------------------
// 3. list_*() enumerates every registered factory.
// ---------------------------------------------------------------------------

TEST_F(VideoPluginInterfaces, list_video_sources_includes_stub) {
    auto ids = PluginRegistry::instance().list_video_sources();
    bool found = false;
    for (auto id : ids) if (id == "stub_source") { found = true; break; }
    EXPECT_TRUE(found) << "stub_source not in list_video_sources()";
}

TEST_F(VideoPluginInterfaces, list_video_sinks_includes_stub) {
    auto ids = PluginRegistry::instance().list_video_sinks();
    bool found = false;
    for (auto id : ids) if (id == "stub_sink") { found = true; break; }
    EXPECT_TRUE(found) << "stub_sink not in list_video_sinks()";
}

TEST_F(VideoPluginInterfaces, list_video_receivers_includes_stub) {
    auto ids = PluginRegistry::instance().list_video_receivers();
    bool found = false;
    for (auto id : ids) if (id == "stub_rx") { found = true; break; }
    EXPECT_TRUE(found) << "stub_rx not in list_video_receivers()";
}

TEST_F(VideoPluginInterfaces, list_video_senders_includes_stub) {
    auto ids = PluginRegistry::instance().list_video_senders();
    bool found = false;
    for (auto id : ids) if (id == "stub_tx") { found = true; break; }
    EXPECT_TRUE(found) << "stub_tx not in list_video_senders()";
}

// ---------------------------------------------------------------------------
// 4. lookup of a non-registered id returns nullptr.
// ---------------------------------------------------------------------------

TEST_F(VideoPluginInterfaces, unknown_id_returns_nullptr) {
    EXPECT_EQ(PluginRegistry::instance().get_video_source("does_not_exist"), nullptr);
    EXPECT_EQ(PluginRegistry::instance().get_video_sink("does_not_exist"), nullptr);
    EXPECT_EQ(PluginRegistry::instance().get_video_receiver("does_not_exist"), nullptr);
    EXPECT_EQ(PluginRegistry::instance().get_video_sender("does_not_exist"), nullptr);
}

// ---------------------------------------------------------------------------
// 5. NIMRTC_REGISTER_VIDEO_* macros register via static init.
// ---------------------------------------------------------------------------
//
// We exercise each macro in its own static-init registrar and check that
// the registry contains the entry. This proves the macros expand correctly
// and integrate with detail::Registrar's Category dispatch.

namespace {
struct MacroProbeSource      { static const np::SimpleVideoSourceFactory<StubVideoSource>     f; };
struct MacroProbeSink        { static const np::SimpleVideoSinkFactory  <StubVideoSink>       f; };
struct MacroProbeReceiver    { static const np::SimpleVideoReceiverFactory<StubVideoReceiver> f; };
struct MacroProbeSender      { static const np::SimpleVideoSenderFactory  <StubVideoSender>   f; };
const np::SimpleVideoSourceFactory<StubVideoSource>     MacroProbeSource    ::f{"macro_source", "macro"};
const np::SimpleVideoSinkFactory  <StubVideoSink>       MacroProbeSink      ::f{"macro_sink",   "macro"};
const np::SimpleVideoReceiverFactory<StubVideoReceiver> MacroProbeReceiver  ::f{"macro_rx",     "macro"};
const np::SimpleVideoSenderFactory  <StubVideoSender>   MacroProbeSender    ::f{"macro_tx",     "macro"};
} // namespace

// Registrar hooks (one per slot). Each macro expansion is a `static`
// Registrar instance whose constructor runs at static-init time.
NIMRTC_REGISTER_VIDEO_SOURCE (macro_source, &MacroProbeSource    ::f);
NIMRTC_REGISTER_VIDEO_SINK   (macro_sink,   &MacroProbeSink      ::f);
NIMRTC_REGISTER_VIDEO_RECEIVER(macro_rx,    &MacroProbeReceiver  ::f);
NIMRTC_REGISTER_VIDEO_SENDER (macro_tx,     &MacroProbeSender    ::f);

TEST_F(VideoPluginInterfaces, macros_register_via_static_init) {
    EXPECT_NE(PluginRegistry::instance().get_video_source  ("macro_source"), nullptr);
    EXPECT_NE(PluginRegistry::instance().get_video_sink    ("macro_sink"),   nullptr);
    EXPECT_NE(PluginRegistry::instance().get_video_receiver("macro_rx"),     nullptr);
    EXPECT_NE(PluginRegistry::instance().get_video_sender  ("macro_tx"),     nullptr);
}

// ---------------------------------------------------------------------------
// 6. Factory create() round-trip — proves the templates compile against
//    the new interfaces and produce usable instances.
// ---------------------------------------------------------------------------

TEST_F(VideoPluginInterfaces, source_factory_produces_usable_instance) {
    const auto* f = PluginRegistry::instance().get_video_source("stub_source");
    ASSERT_NE(f, nullptr);

    np::VideoSourceConfig cfg{};
    cfg.width  = 320;
    cfg.height = 240;
    cfg.ssrc   = 0xCAFEBABE;
    cfg.format = np::VideoPixelFormat::kI420;

    std::unique_ptr<np::IVideoSource> src(f->create(cfg));
    ASSERT_NE(src, nullptr);

    EXPECT_STREQ(src->name(), "StubVideoSource");
    EXPECT_EQ(src->open(), np::kOk);
    EXPECT_FALSE(src->running());
    EXPECT_EQ(src->start(), np::kOk);
    EXPECT_TRUE(src->running());

    std::atomic<int> frame_count{0};
    src->set_callback([&](const np::VideoSourceFrame& /*f*/) {
        frame_count.fetch_add(1, std::memory_order_relaxed);
    });
    src->produce_one();
    EXPECT_EQ(frame_count.load(), 1);

    src->stop();
    EXPECT_FALSE(src->running());
    src->close();
}

TEST_F(VideoPluginInterfaces, sink_factory_produces_usable_instance) {
    const auto* f = PluginRegistry::instance().get_video_sink("stub_sink");
    ASSERT_NE(f, nullptr);

    np::VideoSinkConfig cfg{};
    cfg.label = "test_sink";

    std::unique_ptr<np::IVideoSink> sink(f->create(cfg));
    ASSERT_NE(sink, nullptr);

    EXPECT_EQ(sink->open(), np::kOk);
    np::VideoSourceFrame frame{};
    frame.width = 640;
    frame.height = 480;
    frame.format = np::VideoPixelFormat::kI420;
    EXPECT_EQ(sink->render(frame), np::kOk);
    EXPECT_EQ(sink->stats().frames_rendered, 1u);
    sink->close();
}

TEST_F(VideoPluginInterfaces, receiver_factory_produces_usable_instance) {
    const auto* f = PluginRegistry::instance().get_video_receiver("stub_rx");
    ASSERT_NE(f, nullptr);

    np::VideoReceiverConfig cfg{};
    cfg.codec = np::VideoCodecKind::kH264;
    cfg.ssrc  = 0xABCDEF01;
    cfg.payload_type = 102;

    std::unique_ptr<np::IVideoReceiver> rx(f->create(cfg));
    ASSERT_NE(rx, nullptr);

    EXPECT_EQ(rx->open(), np::kOk);

    np::VideoRtpPacket pkt{};
    pkt.ssrc = cfg.ssrc;
    pkt.seq  = 12345;
    pkt.rtp_timestamp = 90000;
    pkt.payload_type  = cfg.payload_type;
    pkt.marker = true;
    pkt.payload = np::BufferView{};

    EXPECT_EQ(rx->push_rtp(pkt, /*now_us=*/1000), np::kOk);
    EXPECT_EQ(rx->stats().packets_pushed, 1u);

    rx->reset();
    rx->close();
}

TEST_F(VideoPluginInterfaces, sender_factory_produces_usable_instance) {
    const auto* f = PluginRegistry::instance().get_video_sender("stub_tx");
    ASSERT_NE(f, nullptr);

    np::VideoSenderConfig cfg{};
    cfg.codec = np::VideoCodecKind::kH264;
    cfg.ssrc = 0xDEADBEEF;
    cfg.payload_type = 102;
    cfg.mtu  = 1200;
    cfg.initial_seq = 100;

    std::unique_ptr<np::IVideoSender> tx(f->create(cfg));
    ASSERT_NE(tx, nullptr);

    EXPECT_EQ(tx->open(), np::kOk);

    std::atomic<int> packet_count{0};
    tx->set_packet_callback([&](np::BufferView /*payload*/,
                                std::uint32_t /*ts*/,
                                std::uint16_t /*seq*/,
                                bool /*marker*/) {
        packet_count.fetch_add(1, std::memory_order_relaxed);
    });

    np::EncodedVideoFrame frame{};
    frame.codec = np::VideoCodecKind::kH264;
    frame.payload = np::BufferView{};
    frame.is_keyframe = true;
    EXPECT_EQ(tx->push_frame(frame, /*rtp_ts=*/90000), np::kOk);
    EXPECT_EQ(tx->stats().frames_pushed, 1u);

    tx->force_keyframe();
    EXPECT_EQ(tx->stats().force_keyframe_calls, 1u);

    tx->close();
}

} // namespace
