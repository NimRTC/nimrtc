/**
 * @file tests/test_engine_capture_pump.cpp
 * @brief Remote-desktop plan A10 / A11 — the capture -> engine pump.
 *
 * The remote-desktop plan's A10 row asks for exactly this:
 *
 *   "A capture plug-in + a real encoder produce an outbound RTP stream
 *    *without the host calling `send_video()`*; the codec is only ever
 *    touched from the tick thread (asserted by a test that records the
 *    thread id at `encode()` time)."
 *
 * and A11:
 *
 *   "The stats snapshot reports the counters and they move during a live
 *    session."
 *
 * So this file has four jobs:
 *
 *   1. **The pump works.**  A capture device is registered, the engine
 *      is configured to use it, and the test drives `tick()` — never
 *      `send_video()` — and observes RTP packets going out.
 *
 *   2. **The codec is only touched from the tick thread.**  The test
 *      registers its own codec plug-in, records the thread id at
 *      `encode()` time, and asserts it is the thread that called
 *      `tick()`.
 *
 *   3. **The counters move.**  The stats snapshot reports the frames
 *      captured, the frames pumped and the drops.
 *
 *   4. **The failure paths are loud.**  A missing capture plug-in is
 *      reported through `on_error` (not silently ignored), and a device
 *      that is lost is reopened.
 *
 * The capture device under test is a *fake* defined in this file — not
 * the synthetic one from the capture module — because the fake has to
 * record the thread it produced its frames on, and it has to be able to
 * inject a "device lost" error.  The synthetic device's own behaviour
 * is covered by the module's tests; what matters here is the *engine's*
 * half of the contract.
 *
 * The engine is driven through the "shell" transport strategy, so no
 * ICE/DTLS handshake is involved.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nimrtc/core/engine_errors.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/video_capture.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/video_capture/capture_source_adapter.hpp>

namespace {

using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
namespace np = nimrtc::plugins;

// ---------------------------------------------------------------------------
// The fake capture device
// ---------------------------------------------------------------------------

/** A capture device the test drives by hand.
 *
 *  It behaves like a real device in the ways the engine cares about:
 *
 *   - it produces frames on *its own* thread (here: the thread that
 *     calls `produce()`), so the engine's tick thread is a different
 *     thread from the producer's — which is what makes the
 *     "the codec is only touched from the tick thread" assertion
 *     meaningful;
 *   - it fills a bounded queue with an explicit drop policy;
 *   - it can report a device error, so the pump's recovery path is
 *     reachable.
 */
class FakeCapture final : public np::IVideoCapture {
public:
    explicit FakeCapture(np::CaptureConfig cfg) : cfg_(std::move(cfg)) {}

    // ---- IPlugin --------------------------------------------------------
    const char* name() const noexcept override { return "fake-capture"; }

    np::Status open() noexcept override {
        std::lock_guard<std::mutex> lk(mu_);
        if (open_) return np::kOk;
        if (cfg_.format.width == 0) cfg_.format.width = 64;
        if (cfg_.format.height == 0) cfg_.format.height = 48;
        if (cfg_.format.fps == 0) cfg_.format.fps = 30;
        if (cfg_.format.tag.empty()) cfg_.format.tag = "bgra";
        open_ = true;
        ++open_count_;
        return np::kOk;
    }

    void close() noexcept override {
        std::lock_guard<std::mutex> lk(mu_);
        open_ = false;
    }

    // ---- IVideoCapture ---------------------------------------------------
    std::vector<np::CaptureDevice> enumerate_devices() const noexcept override {
        np::CaptureDevice d;
        d.id = "fake:0";
        d.name = "Fake display";
        d.kind = np::CaptureDeviceKind::kScreen;
        d.max_width = 64;
        d.max_height = 48;
        d.max_fps = 30;
        return {d};
    }

    std::vector<np::CaptureFormat>
    supported_formats(const std::string&) const noexcept override {
        np::CaptureFormat f;
        f.width = 64;
        f.height = 48;
        f.fps = 30;
        f.tag = "bgra";
        f.zero_copy = false;
        f.has_dirty_rects = true;
        return {f};
    }

    np::CaptureCapabilities capabilities() const noexcept override {
        np::CaptureCapabilities c;
        c.zero_copy = false;
        c.dirty_rects = true;
        c.cursor_composited = false;
        c.region_selection = true;
        c.enumeration = true;
        c.max_pixels = 64ull * 48ull;
        c.backend = "fake";
        return c;
    }

    bool is_open() const noexcept override {
        std::lock_guard<std::mutex> lk(mu_);
        return open_;
    }

    np::CaptureConfig config() const noexcept override {
        std::lock_guard<std::mutex> lk(mu_);
        return cfg_;
    }

    np::Status pop_frame(np::CaptureFrame& out) noexcept override {
        std::lock_guard<std::mutex> lk(mu_);
        if (queue_.empty()) return np::kErrNotReady;

        // The capture seam's contract: the pixels stay valid until the
        // *next* call to `pop_frame()`.  So the popped frame's bytes are
        // copied into a buffer that lives until then — not into a
        // per-slot buffer that dies with the queue entry.
        const Slot& slot = queue_.front();
        current_pixels_.assign(slot.pixels.begin(), slot.pixels.end());

        out = slot.frame;
        out.pixels = current_pixels_.data();
        out.size = current_pixels_.size();

        queue_.pop_front();
        ++stats_.frames_captured;
        return np::kOk;
    }

    void set_frame_callback(np::CaptureFrameCallback cb) noexcept override {
        std::lock_guard<std::mutex> lk(mu_);
        cb_ = std::move(cb);
    }

    np::CaptureStats stats() const noexcept override {
        std::lock_guard<std::mutex> lk(mu_);
        return stats_;
    }

    // ---- Test controls ---------------------------------------------------

    /** Produce one BGRA frame into the queue, on the calling thread. */
    void produce() {
        std::lock_guard<std::mutex> lk(mu_);
        Slot slot;
        slot.frame.width  = cfg_.format.width;
        slot.frame.height = cfg_.format.height;
        slot.frame.stride = static_cast<std::int32_t>(slot.frame.width * 4u);
        slot.frame.size   = static_cast<std::size_t>(slot.frame.width)
                          * slot.frame.height * 4u;
        slot.frame.capture_ts_us = 1000 + static_cast<std::int64_t>(produced_++);
        slot.frame.frame_seq     = static_cast<std::uint32_t>(produced_);
        // A flat mid-grey: the conversion maths is exercised (the YUV
        // planes are not all 0x80) but the values are deterministic.
        slot.pixels.assign(slot.frame.size, 0x80u);
        slot.frame.pixels = slot.pixels.data();

        queue_.push_back(std::move(slot));

        // The queue is bounded: the oldest frame goes (the screen-content
        // policy — the newest frame is the truth).
        while (queue_.size() > depth_) {
            queue_.pop_front();
            ++stats_.frames_dropped;
        }
    }

    /** Report a device error, as a lost display would. */
    void fail_device() {
        std::lock_guard<std::mutex> lk(mu_);
        ++stats_.device_errors;
    }

    /** How many times the device was opened. */
    int open_count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return open_count_;
    }

    /** The thread the last frame was produced on. */
    std::thread::id producer_thread() const {
        std::lock_guard<std::mutex> lk(mu_);
        return producer_thread_;
    }

    void note_producer_thread() {
        std::lock_guard<std::mutex> lk(mu_);
        producer_thread_ = std::this_thread::get_id();
    }

private:
    struct Slot {
        np::CaptureFrame frame{};
        std::vector<std::uint8_t> pixels;
    };

    mutable std::mutex mu_;
    np::CaptureConfig cfg_;
    std::deque<Slot> queue_;
    /** The bytes of the most recently popped frame.  Valid until the
     *  next `pop_frame()` — the seam's lifetime contract. */
    std::vector<std::uint8_t> current_pixels_;
    np::CaptureFrameCallback cb_;
    np::CaptureStats stats_{};
    std::thread::id producer_thread_{};
    std::uint32_t produced_ = 0;
    std::size_t depth_ = 2;
    int open_count_ = 0;
    bool open_ = false;
};

class FakeCaptureFactory final : public np::IVideoCaptureFactory {
public:
    std::string_view id() const noexcept override { return "fake-capture"; }
    std::string_view display_name() const noexcept override {
        return "Fake capture device (engine pump test)";
    }
    np::IVideoCapture* create(np::CaptureConfig cfg) const override {
        auto* c = new FakeCapture(std::move(cfg));
        std::lock_guard<std::mutex> lk(mu_);
        last_ = c;
        return c;
    }

    np::IVideoSource* create_source(np::IVideoCapture* device,
                                    np::VideoSourceConfig cfg) const override {
        return nimrtc::video_capture::make_capture_source(device,
                                                           std::move(cfg));
    }

    /** The device the engine created (the last one). */
    static FakeCapture* last() {
        std::lock_guard<std::mutex> lk(mu_);
        return last_;
    }

private:
    static std::mutex mu_;
    static FakeCapture* last_;
};

std::mutex FakeCaptureFactory::mu_;
FakeCapture* FakeCaptureFactory::last_ = nullptr;

// ---------------------------------------------------------------------------
// The fake codec — it records the thread it was called on
// ---------------------------------------------------------------------------

/** A codec that records the thread id at every `encode()` call.
 *
 *  The engine's video codec is chosen by plugin id, so the test
 *  registers this one under its own id and points
 *  `EngineConfig::video_codec_name` at it.  It emits a tiny Annex-B
 *  NAL unit, which is enough for the sender to packetise.
 */
class ThreadRecordingCodec final : public np::IVideoCodec {
public:
    explicit ThreadRecordingCodec(np::VideoCodecConfig cfg) : cfg_(cfg) {}

    const char* name() const noexcept override { return "thread-recording-codec"; }

    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    bool can_encode() const noexcept override { return true; }
    bool can_decode() const noexcept override { return false; }
    np::VideoCodecKind kind() const noexcept override { return np::VideoCodecKind::kH264; }
    std::string_view codec_name() const noexcept override { return "h264"; }

    np::Status encode(const np::VideoFrame& raw,
                      std::uint8_t* output,
                      std::size_t output_capacity,
                      np::EncodedVideoFrame& encoded_out) noexcept override {
        (void)raw;
        {
            std::lock_guard<std::mutex> lk(mu_);
            encode_threads_.push_back(std::this_thread::get_id());
            ++encode_count_;
        }
        if (output_capacity < 5) return np::kErrBufferTooSmall;
        // A minimal Annex-B IDR slice.
        output[0] = 0x00; output[1] = 0x00; output[2] = 0x00; output[3] = 0x01;
        output[4] = 0x65;
        encoded_out.payload = nimrtc::core::ByteSpan{output, 5};
        encoded_out.is_keyframe = true;
        encoded_out.codec = np::VideoCodecKind::kH264;
        encoded_out.nalu_format = np::NaluFormat::kAnnexBStartCode;
        encoded_out.payload_type = 102;
        encoded_out.info.frame_seq = 1;
        return np::kOk;
    }

    np::Status decode(const np::EncodedVideoFrame&, np::VideoFrame&,
                      std::uint8_t* const*) noexcept override {
        return np::kErrUnsupported;
    }

    np::Status force_keyframe() noexcept override { return np::kOk; }

    np::VideoCodecConfig config() const noexcept override { return cfg_; }
    np::Status update_config(np::VideoCodecConfig cfg) noexcept override {
        cfg_ = cfg;
        return np::kOk;
    }
    np::VideoCodecStats stats() const noexcept override { return stats_; }
    std::uint8_t payload_type() const noexcept override { return 102; }

    // ---- Test controls ---------------------------------------------------

    /** The thread ids every `encode()` ran on. */
    std::vector<std::thread::id> encode_threads() const {
        std::lock_guard<std::mutex> lk(mu_);
        return encode_threads_;
    }

    std::uint64_t encode_count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return encode_count_;
    }

private:
    mutable std::mutex mu_;
    np::VideoCodecConfig cfg_;
    np::VideoCodecStats stats_{};
    std::vector<std::thread::id> encode_threads_;
    std::uint64_t encode_count_ = 0;
};

class ThreadRecordingCodecFactory final : public np::IVideoCodecFactory {
public:
    std::string_view id() const noexcept override { return "thread-recording"; }
    std::string_view display_name() const noexcept override {
        return "Codec that records the thread it was called on";
    }
    np::IVideoCodec* create(np::VideoCodecConfig cfg) const override {
        auto* c = new ThreadRecordingCodec(cfg);
        std::lock_guard<std::mutex> lk(mu_);
        last_ = c;
        return c;
    }

    /** The codec the engine created. */
    static ThreadRecordingCodec* last() {
        std::lock_guard<std::mutex> lk(mu_);
        return last_;
    }

private:
    static std::mutex mu_;
    static ThreadRecordingCodec* last_;
};

std::mutex ThreadRecordingCodecFactory::mu_;
ThreadRecordingCodec* ThreadRecordingCodecFactory::last_ = nullptr;

// ---------------------------------------------------------------------------
// The fixture
// ---------------------------------------------------------------------------

class EngineCapturePump : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();

        // The capture device the engine will create.  The factory
        // records it so the test can drive it.
        static FakeCaptureFactory capture_factory;
        nimrtc::core::PluginRegistry::instance().register_video_capture(
            capture_factory.id(), &capture_factory);

        static ThreadRecordingCodecFactory codec_factory;
        nimrtc::core::PluginRegistry::instance().register_video_codec(
            codec_factory.id(), &codec_factory);
    }

    static EngineConfig make_test_config() {
        EngineConfig cfg;
        cfg.stun_server_host       = "";
        cfg.local_port_range_begin = 52800;
        cfg.local_port_range_end   = 52900;
        cfg.transport_strategy_name = "shell";
        cfg.video_codec_name    = "thread-recording";
        cfg.video_sender_name   = "reference";
        cfg.video_receiver_name = "reference";
        cfg.video_source_name   = "memory";
        cfg.video_sink_name     = "headless";

        // The capture device.  The format tag is left empty so the
        // device picks its own default ("bgra"), which is what a real
        // desktop compositor delivers.
        cfg.video_capture_name = "fake-capture";
        cfg.video_capture_config.format.width  = 64;
        cfg.video_capture_config.format.height = 48;
        cfg.video_capture_config.queue_depth   = 2;
        return cfg;
    }
};

// ---------------------------------------------------------------------------
// The pump
// ---------------------------------------------------------------------------

TEST_F(EngineCapturePump, TheEngineResolvesTheCaptureDevice) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The device is there, and it is open.
    ASSERT_NE(engine.video_capture(), nullptr);
    EXPECT_TRUE(engine.video_capture()->is_open());

    // The negotiated geometry is what the *device* agreed to, not what
    // the host guessed.
    EXPECT_EQ(engine.video_capture()->config().format.width, 64u);
    EXPECT_EQ(engine.video_capture()->config().format.height, 48u);

    engine.close();
}

TEST_F(EngineCapturePump, AnUnknownCapturePluginIsReportedNotIgnored) {
    EngineConfig cfg = make_test_config();
    cfg.video_capture_name = "no-such-capture-plugin";

    std::atomic<bool> saw_missing{false};
    NimRTCEngine engine(cfg);
    engine.set_on_error([&](std::uint32_t e, std::string_view) {
        if (e == nimrtc::core::kVideoCapturePluginMissing) {
            saw_missing.store(true);
        }
    });

    ASSERT_EQ(engine.open(), 0u);

    // The engine still opens (a missing capture device is not fatal for
    // the rest of the session), but the host is told.
    EXPECT_EQ(engine.video_capture(), nullptr);
    EXPECT_TRUE(saw_missing.load());

    engine.close();
}

TEST_F(EngineCapturePump, NoCaptureConfiguredMeansNoDevice) {
    EngineConfig cfg = make_test_config();
    cfg.video_capture_name.clear();

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    EXPECT_EQ(engine.video_capture(), nullptr);
    // And the counters stay at zero — a host that never configured a
    // capture device sees no change in its telemetry.
    EXPECT_EQ(engine.stats().video_capture_frames, 0u);
    EXPECT_EQ(engine.stats().video_capture_frames_pumped, 0u);

    engine.close();
}

TEST_F(EngineCapturePump, ThePumpProducesRtpWithoutTheHostCallingSendVideo) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // Drive the device from *another* thread, the way a real capture
    // plug-in does (DXGI's duplication event, ScreenCaptureKit's
    // stream, PipeWire's buffer).
    std::thread producer([&engine] {
        for (int i = 0; i < 5; ++i) {
            auto* cap = static_cast<FakeCapture*>(engine.video_capture());
            if (cap == nullptr) break;
            cap->note_producer_thread();
            cap->produce();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    // The host only ticks.  It never calls send_video().
    for (int i = 0; i < 20; ++i) {
        (void)engine.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    producer.join();

    const auto stats = engine.stats();

    // The capture device produced frames...
    EXPECT_GT(stats.video_capture_frames, 0u);
    // ...and the engine pumped them into the encoder...
    EXPECT_GT(stats.video_capture_frames_pumped, 0u);
    // ...and the encoder's output became RTP packets.
    EXPECT_GT(stats.video_packets_sent, 0u);

    engine.close();
}

TEST_F(EngineCapturePump, TheCodecIsOnlyTouchedFromTheTickThread) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::thread::id tick_thread = std::this_thread::get_id();

    std::thread producer([&engine] {
        for (int i = 0; i < 5; ++i) {
            auto* cap = static_cast<FakeCapture*>(engine.video_capture());
            if (cap == nullptr) break;
            cap->note_producer_thread();
            cap->produce();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    for (int i = 0; i < 20; ++i) {
        (void)engine.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    producer.join();

    auto* codec = ThreadRecordingCodecFactory::last();
    ASSERT_NE(codec, nullptr);

    const auto threads = codec->encode_threads();
    ASSERT_FALSE(threads.empty()) << "the encoder was never called";

    // Every single encode() ran on the tick thread.  This is the
    // acceptance criterion of the plan's A10 row: the codec is only ever
    // touched from the tick thread.
    for (std::size_t i = 0; i < threads.size(); ++i) {
        EXPECT_EQ(threads[i], tick_thread)
            << "encode() #" << i << " ran on a thread other than the "
            << "engine's tick thread";
    }

    engine.close();
}

TEST_F(EngineCapturePump, TheProducerThreadIsNotTheTickThread) {
    // A guard on the test itself: if the producer happened to run on
    // the tick thread, the assertion above would be vacuous.
    EngineConfig cfg = make_test_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::thread::id tick_thread = std::this_thread::get_id();
    std::thread::id producer_thread;

    std::thread producer([&engine, &producer_thread] {
        auto* cap = static_cast<FakeCapture*>(engine.video_capture());
        if (cap == nullptr) return;
        cap->note_producer_thread();
        cap->produce();
        producer_thread = cap->producer_thread();
    });
    producer.join();

    EXPECT_NE(producer_thread, tick_thread);

    engine.close();
}

// ---------------------------------------------------------------------------
// The statistics (A11)
// ---------------------------------------------------------------------------

TEST_F(EngineCapturePump, TheCaptureCountersMoveDuringALiveSession) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto before = engine.stats();
    EXPECT_EQ(before.video_capture_frames, 0u);
    EXPECT_EQ(before.video_capture_dropped, 0u);
    EXPECT_EQ(before.video_capture_device_errors, 0u);

    // Overflow the device's bounded queue: the oldest frames go, and
    // the drops are counted.
    auto* cap = static_cast<FakeCapture*>(engine.video_capture());
    ASSERT_NE(cap, nullptr);
    for (int i = 0; i < 10; ++i) {
        cap->produce();
    }
    for (int i = 0; i < 10; ++i) {
        (void)engine.tick();
    }

    const auto after = engine.stats();
    EXPECT_GT(after.video_capture_frames, 0u);
    EXPECT_GT(after.video_capture_dropped, 0u);

    engine.close();
}

TEST_F(EngineCapturePump, ADeviceErrorIsCountedAndTheDeviceIsReopened) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    auto* cap = static_cast<FakeCapture*>(engine.video_capture());
    ASSERT_NE(cap, nullptr);
    const int opens_before = cap->open_count();

    // The device is lost.
    cap->fail_device();

    // The pump notices on the next tick and reopens the device.
    for (int i = 0; i < 5; ++i) {
        (void)engine.tick();
    }

    const auto stats = engine.stats();
    EXPECT_GE(stats.video_capture_device_errors, 1u);
    EXPECT_GE(stats.video_capture_restarts, 1u);
    EXPECT_GT(cap->open_count(), opens_before);

    engine.close();
}

// ---------------------------------------------------------------------------
// The manual path still works
// ---------------------------------------------------------------------------

TEST_F(EngineCapturePump, TheManualPathStillWorksWithACaptureConfigured) {
    // `send_video()` stays the explicit manual path (the plan's A10
    // row says so).  A host that drives the device by hand sets
    // `video_capture_auto_pump = false` and keeps using send_video().
    EngineConfig cfg = make_test_config();
    cfg.video_capture_auto_pump = false;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The device is still opened — the host drives it.
    ASSERT_NE(engine.video_capture(), nullptr);
    EXPECT_TRUE(engine.video_capture()->is_open());

    // The pump does not run, so nothing is encoded on the host's
    // behalf.
    auto* cap = static_cast<FakeCapture*>(engine.video_capture());
    cap->produce();
    for (int i = 0; i < 5; ++i) {
        (void)engine.tick();
    }
    EXPECT_EQ(engine.stats().video_capture_frames_pumped, 0u);

    engine.close();
}

} // anonymous namespace
