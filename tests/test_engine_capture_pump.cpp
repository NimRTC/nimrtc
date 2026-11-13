/**
 * @file tests/test_engine_capture_pump.cpp
 * @brief The capture → engine pump (remote-desktop plan A10).
 *
 * ## What this test proves
 *
 * Three things, each of which is an acceptance criterion of plan item
 * A10 (and of the Windows-rendering work package this landed with):
 *
 *   1. **An outbound RTP stream without `send_video()`.**  A capture
 *      plug-in plus an encoder produce packets on their own: the host
 *      never calls `send_video()`.  The sender's counters are the
 *      evidence — `frames_pushed` moves because the *pump* pushed.
 *
 *   2. **The codec is only ever touched from the tick thread.**  The
 *      capture device here pushes frames from *its own* thread (the way
 *      a real DXGI / PipeWire backend does), and the codec records the
 *      thread id at `encode()` time.  The test asserts that the
 *      recorded id is the id of the thread that called `tick()`.
 *      This is the RD-6 invariant, and it is the reason the pump
 *      *polls* the device instead of installing a frame callback.
 *
 *   3. **The telemetry reaches the engine's stats snapshot.**  The
 *      capture counters, the codec telemetry and the pump's own
 *      counters are all visible in `engine.stats()`.
 *
 * ## Why the capture device pushes from its own thread
 *
 * A capture device is *push*: the OS tells the plug-in that a new frame
 * exists, on a thread the plug-in owns (RD-6: a capture plug-in may
 * own its own thread; the engine stays a single-threaded
 * orchestrator).  A test that pushed from the test's own thread would
 * not exercise the interesting half of the contract — the one where a
 * frame arrives on a foreign thread and must still be encoded on the
 * tick thread.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/video_capture.hpp>
#include <nimrtc/plugins/video_codec.hpp>

namespace {

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
namespace np = nimrtc::plugins;
namespace nc = nimrtc::core;

// ---------------------------------------------------------------------------
// A capture device that pushes BGRA frames from its own thread.
//
// It is deliberately simple: a background thread fills a bounded queue
// with BGRA frames (the desktop-compositor format) at a fixed cadence,
// and `pop_frame()` hands them to the engine.  The queue is bounded so
// an over-loaded consumer drops frames instead of growing without
// bound — the property the acceptance criteria ask for.
// ---------------------------------------------------------------------------

/** The shared state between the capture device and its producer thread. */
struct PumpCaptureState {
    /** One queued frame: the pixels plus the metadata that belongs to
     *  *them*.  Keeping the two together is what makes the queue safe:
     *  a frame's timestamp must not change when a newer frame is
     *  queued. */
    struct Frame {
        std::vector<std::uint8_t> pixels;
        std::int64_t ts_us = 0;
        std::uint32_t seq   = 0;
    };

    std::mutex mutex;
    std::deque<Frame> frames;
    /** The most recently popped frame.  It keeps the popped pixel buffer
     *  alive so the pointer handed to the consumer stays valid until
     *  the next `pop_frame()` — the same contract the real capture
     *  queue documents. */
    Frame popped;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> produced{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint32_t> next_seq{0};
    std::atomic<std::int64_t> next_ts_us{0};
    std::uint32_t width  = 64;
    std::uint32_t height = 48;
    /** How many frames the device may produce (0 = unlimited). */
    std::atomic<std::uint32_t> budget{0};
};

class PumpCaptureDevice : public np::IVideoCapture {
public:
    explicit PumpCaptureDevice(np::CaptureConfig cfg, PumpCaptureState* state)
        : cfg_(std::move(cfg)), state_(state) {}

    ~PumpCaptureDevice() override { close(); }

    // ---- IPlugin ----------------------------------------------------------
    const char* name() const noexcept override { return "PumpCaptureDevice"; }

    np::Status open() noexcept override {
        if (open_) return np::kOk;
        open_ = true;
        // The producer thread is the device's own thread (RD-6: a capture
        // plug-in may own one; the engine stays single-threaded).
        producer_ = std::thread([this] { produce_loop(); });
        return np::kOk;
    }

    void close() noexcept override {
        if (!open_) return;
        open_ = false;
        if (state_) state_->stop.store(true);
        if (producer_.joinable()) producer_.join();
    }

    // ---- Enumeration ------------------------------------------------------
    std::vector<np::CaptureDevice> enumerate_devices() const noexcept override {
        return {};
    }
    std::vector<np::CaptureFormat> supported_formats(
        const std::string&) const noexcept override {
        return {};
    }
    np::CaptureCapabilities capabilities() const noexcept override {
        np::CaptureCapabilities c;
        c.backend = "pump_test";
        return c;
    }

    // ---- Lifecycle --------------------------------------------------------
    bool is_open() const noexcept override { return open_; }
    np::CaptureConfig config() const noexcept override { return cfg_; }

    // ---- Frame delivery ---------------------------------------------------
    np::Status pop_frame(np::CaptureFrame& out) noexcept override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->frames.empty()) return np::kErrNotReady;

        // The popped frame is moved into the queue's own storage so its
        // pixels stay alive until the next pop() — the contract the real
        // capture queue documents.
        state_->popped = std::move(state_->frames.front());
        state_->frames.pop_front();

        out.pixels      = state_->popped.pixels.data();
        out.size        = state_->popped.pixels.size();
        out.stride      = static_cast<std::int32_t>(state_->width * 4u);
        out.width       = state_->width;
        out.height      = state_->height;
        out.format      = np::VideoPixelFormat::kBGRA;
        out.capture_ts_us = state_->popped.ts_us;
        out.frame_seq     = state_->popped.seq;
        return np::kOk;
    }

    void set_frame_callback(np::CaptureFrameCallback) noexcept override {}

    np::CaptureStats stats() const noexcept override {
        np::CaptureStats s;
        s.frames_captured = state_->produced.load();
        s.frames_dropped  = state_->dropped.load();
        // A CPU capture device: no GPU surface, so every frame is a
        // CPU-staged one.
        s.frames_cpu_staged = state_->produced.load();
        return s;
    }

private:
    void produce_loop() {
        const std::size_t bytes =
            static_cast<std::size_t>(state_->width) * state_->height * 4u;
        while (!state_->stop.load()) {
            const std::uint32_t budget = state_->budget.load();
            if (budget != 0 && state_->produced.load() >= budget) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            PumpCaptureState::Frame f;
            f.pixels.assign(bytes, 0x40u);
            f.ts_us = state_->next_ts_us.fetch_add(33'333);   // 30 fps
            f.seq   = state_->next_seq.fetch_add(1);

            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                // The bounded queue: at most two frames are held, so an
                // over-loaded consumer drops instead of accumulating.
                if (state_->frames.size() >= 2) {
                    state_->frames.pop_front();
                    ++state_->dropped;
                }
                state_->frames.push_back(std::move(f));
            }
            ++state_->produced;

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    np::CaptureConfig cfg_{};
    PumpCaptureState* state_ = nullptr;
    std::thread producer_;
    bool open_ = false;
};

class PumpCaptureFactory : public np::IVideoCaptureFactory {
public:
    explicit PumpCaptureFactory(PumpCaptureState* state) : state_(state) {}

    std::string_view id() const noexcept override { return "pump_test_capture"; }
    std::string_view display_name() const noexcept override {
        return "Pump test capture device";
    }
    np::IVideoCapture* create(np::CaptureConfig cfg) const override {
        return new PumpCaptureDevice(std::move(cfg), state_);
    }

private:
    PumpCaptureState* state_;
};

// ---------------------------------------------------------------------------
// A codec that records the thread it was called on.
//
// This is the instrument that makes the RD-6 invariant observable: the
// test compares the recorded id with the id of the thread that called
// `tick()`.
// ---------------------------------------------------------------------------

/**
 * The counters, kept *outside* the codec object.
 *
 * The engine takes ownership of the codec the factory hands out (it
 * wraps the raw pointer in a `unique_ptr`), so the codec instance
 * cannot be the thing that owns the counters — the test needs to read
 * them after `engine.close()` deleted the codec.  The state is
 * therefore a separate, test-owned object that both sides point at.
 */
struct ThreadRecordingCodecState {
    std::atomic<std::uint64_t> encode_calls{0};
    std::atomic<std::uint64_t> encode_thread_hash{0};
    std::atomic<std::uint64_t> frames_encoded{0};
    std::atomic<std::uint64_t> keyframes{0};
};

class ThreadRecordingCodec : public np::IVideoCodec {
public:
    explicit ThreadRecordingCodec(ThreadRecordingCodecState* state)
        : state_(state) {}

    // ---- IPlugin ----------------------------------------------------------
    const char* name() const noexcept override { return "ThreadRecordingCodec"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    // ---- IVideoCodec ------------------------------------------------------
    bool can_encode() const noexcept override { return true; }
    bool can_decode() const noexcept override { return false; }
    np::VideoCodecKind kind() const noexcept override {
        return np::VideoCodecKind::kH264;
    }
    std::string_view codec_name() const noexcept override { return "h264"; }

    np::Status encode(const np::VideoFrame& raw,
                      std::uint8_t* output,
                      std::size_t output_capacity,
                      np::EncodedVideoFrame& encoded_out) noexcept override {
        // The measurement: which thread is the codec being called on?
        ++state_->encode_calls;
        state_->encode_thread_hash.store(std::hash<std::thread::id>{}(
                                             std::this_thread::get_id()),
                                         std::memory_order_release);

        if (output_capacity < 16) return np::kErrBufferTooSmall;

        // A tiny, deterministic Annex-B "bitstream" — enough for the
        // sender's packetizer to find one NAL unit and emit a packet.
        static const std::uint8_t kAnnexB[] = {
            0x00, 0x00, 0x00, 0x01,   // start code
            0x65, 0xAA, 0xBB, 0xCC,   // the NAL unit
        };
        for (std::size_t i = 0; i < sizeof(kAnnexB); ++i) output[i] = kAnnexB[i];
        encoded_out.payload = nc::ByteSpan{output, sizeof(kAnnexB)};
        encoded_out.is_keyframe = true;
        encoded_out.codec = np::VideoCodecKind::kH264;
        encoded_out.nalu_format = np::NaluFormat::kAnnexBStartCode;
        encoded_out.info = raw.info;
        ++state_->frames_encoded;
        ++state_->keyframes;
        return np::kOk;
    }

    np::Status decode(const np::EncodedVideoFrame&, np::VideoFrame&,
                      std::uint8_t* const*) noexcept override {
        return np::kErrUnsupported;
    }
    np::Status force_keyframe() noexcept override { return np::kOk; }

    np::VideoCodecConfig config() const noexcept override { return cfg_; }
    np::Status update_config(np::VideoCodecConfig cfg) noexcept override {
        cfg_ = std::move(cfg);
        return np::kOk;
    }
    np::VideoCodecStats stats() const noexcept override {
        np::VideoCodecStats s;
        s.frames_encoded = state_->frames_encoded.load();
        s.keyframes_encoded = state_->keyframes.load();
        s.bytes_encoded = state_->frames_encoded.load() * 8u;
        // A software codec: the CPU staging path, and a little latency.
        s.frames_encoded_staged = state_->frames_encoded.load();
        s.encode_latency_us_total = state_->frames_encoded.load() * 100u;
        s.encode_latency_us_max = 100u;
        return s;
    }
    std::uint8_t payload_type() const noexcept override { return 102; }
    std::string_view backend_id() const noexcept override { return "thread_test"; }
    bool is_hardware_accelerated() const noexcept override { return false; }

private:
    ThreadRecordingCodecState* state_ = nullptr;
    np::VideoCodecConfig cfg_{};
};

class ThreadRecordingCodecFactory : public np::IVideoCodecFactory {
public:
    explicit ThreadRecordingCodecFactory(ThreadRecordingCodecState* state)
        : state_(state) {}

    std::string_view id() const noexcept override { return "thread_test_codec"; }
    std::string_view display_name() const noexcept override {
        return "Thread-recording test codec";
    }
    np::IVideoCodec* create(np::VideoCodecConfig cfg) const override {
        // The engine owns the returned codec (it wraps the pointer in a
        // `unique_ptr`), so the instance has to live on the heap.  The
        // counters live in the test-owned state object, which outlives
        // the codec.
        auto* codec = new ThreadRecordingCodec(state_);
        codec->update_config(cfg);
        return codec;
    }

private:
    ThreadRecordingCodecState* state_ = nullptr;
};

// ---------------------------------------------------------------------------
// The engine configuration used by every test here.
//
// The "shell" transport strategy keeps `open()` from touching the
// network, so the test needs no socket.
// ---------------------------------------------------------------------------
EngineConfig make_config() {
    EngineConfig cfg;
    cfg.transport_strategy_name = "shell";
    cfg.stun_server_host = "";
    cfg.video_codec_name      = "thread_test_codec";
    cfg.video_sender_name     = "reference";
    cfg.video_receiver_name   = "reference";
    cfg.video_source_name     = "memory";
    cfg.video_sink_name       = "headless";
    cfg.video_capture_name    = "pump_test_capture";
    cfg.video_width  = 64;
    cfg.video_height = 48;
    return cfg;
}

}  // namespace


// ===========================================================================
// 1. The pump produces an outbound stream without send_video().
// ===========================================================================
TEST(EngineCapturePump, ProducesStreamWithoutSendVideo) {
    PumpCaptureState state;
    static const PumpCaptureFactory capture_factory(&state);

    ThreadRecordingCodecState codec_state;
    static const ThreadRecordingCodecFactory codec_factory(&codec_state);

    PluginRegistry::instance().register_video_capture(
        capture_factory.id(), &capture_factory);
    PluginRegistry::instance().register_video_codec(
        codec_factory.id(), &codec_factory);

    EngineConfig cfg = make_config();

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The pump is on by default; the host does *not* call send_video().
    EXPECT_TRUE(cfg.video_capture_pump);

    // Let the device produce frames, and drive the engine's tick — the
    // pump runs from there.
    const std::uint64_t thread_hash = std::hash<std::thread::id>{}(
        std::this_thread::get_id());

    for (int i = 0; i < 200; ++i) {
        engine.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (codec_state.encode_calls.load() >= 3) break;
    }

    // The codec was called — by the pump, without any send_video().
    EXPECT_GE(codec_state.encode_calls.load(), 3u);

    // And the frames reached the sender (the RTP path).
    auto* sender = engine.video_sender();
    ASSERT_NE(sender, nullptr);
    EXPECT_GE(sender->stats().frames_pushed, 3u);
    EXPECT_GT(sender->stats().packets_emitted, 0u);

    // RD-6: the codec was only ever touched on the tick thread.
    EXPECT_EQ(codec_state.encode_thread_hash.load(), thread_hash);

    // The telemetry reached the stats snapshot.
    const auto stats = engine.stats();
    EXPECT_GE(stats.video_capture_pumped_frames, 3u);
    EXPECT_EQ(stats.video_capture_pump_errors, 0u);
    EXPECT_GT(stats.video_capture_frames_captured, 0u);
    EXPECT_EQ(stats.video_codec_backend, "thread_test");
    EXPECT_FALSE(stats.video_codec_hardware_accelerated);
    EXPECT_GE(stats.video_codec_frames_staged, 3u);
    EXPECT_GT(stats.video_codec_encode_latency_avg_us, 0u);

    engine.close();
}

// ===========================================================================
// 2. The pump can be switched off: then nothing is encoded without
//    send_video().
// ===========================================================================
TEST(EngineCapturePump, DisabledPumpDoesNotEncode) {
    PumpCaptureState state;
    static const PumpCaptureFactory capture_factory(&state);

    ThreadRecordingCodecState codec_state;
    static const ThreadRecordingCodecFactory codec_factory(&codec_state);

    PluginRegistry::instance().register_video_capture(
        capture_factory.id(), &capture_factory);
    PluginRegistry::instance().register_video_codec(
        codec_factory.id(), &codec_factory);

    EngineConfig cfg = make_config();
    cfg.video_capture_pump = false;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    for (int i = 0; i < 100; ++i) {
        engine.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // The device produced frames, but the pump did not encode any.
    EXPECT_GT(state.produced.load(), 0u);
    EXPECT_EQ(codec_state.encode_calls.load(), 0u);
    EXPECT_EQ(engine.stats().video_capture_pumped_frames, 0u);

    engine.close();
}

// ===========================================================================
// 3. The bounded queue keeps the memory bounded: the device drops, it
//    does not accumulate.
// ===========================================================================
TEST(EngineCapturePump, OverloadKeepsMemoryBounded) {
    PumpCaptureState state;
    static const PumpCaptureFactory capture_factory(&state);

    ThreadRecordingCodecState codec_state;
    static const ThreadRecordingCodecFactory codec_factory(&codec_state);

    PluginRegistry::instance().register_video_capture(
        capture_factory.id(), &capture_factory);
    PluginRegistry::instance().register_video_codec(
        codec_factory.id(), &codec_factory);

    EngineConfig cfg = make_config();
    // Do not tick at all: the consumer is stalled, so the device's
    // bounded queue must be the only thing that grows.
    cfg.video_capture_pump = false;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    for (int i = 0; i < 200; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // The device produced far more frames than the queue can hold, and
    // the surplus was dropped rather than buffered.
    EXPECT_GT(state.produced.load(), 10u);
    EXPECT_GT(state.dropped.load(), 0u);
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        EXPECT_LE(state.frames.size(), 2u);
    }

    engine.close();
}

// ===========================================================================
// 4. The manual path still works: send_video() and the pump coexist.
// ===========================================================================
TEST(EngineCapturePump, ManualPathStillWorks) {
    PumpCaptureState state;
    static const PumpCaptureFactory capture_factory(&state);

    ThreadRecordingCodecState codec_state;
    static const ThreadRecordingCodecFactory codec_factory(&codec_state);

    PluginRegistry::instance().register_video_capture(
        capture_factory.id(), &capture_factory);
    PluginRegistry::instance().register_video_codec(
        codec_factory.id(), &codec_factory);

    EngineConfig cfg = make_config();
    cfg.video_capture_pump = false;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // A hand-built I420 frame.
    std::vector<std::uint8_t> pixels(64 * 48 * 3u / 2u, 0x80u);
    np::VideoSourceFrame frame{};
    frame.width  = 64;
    frame.height = 48;
    frame.format = np::VideoPixelFormat::kI420;
    frame.stride_y = 64;
    frame.stride_u = 32;
    frame.stride_v = 32;
    frame.plane_y = pixels.data();
    frame.plane_u = pixels.data() + 64 * 48;
    frame.plane_v = pixels.data() + 64 * 48 + 32 * 24;
    frame.capture_ts_us = 1'000;
    frame.frame_seq = 7;
    frame.rtp_timestamp = 90'000;

    EXPECT_EQ(engine.send_video(frame), np::kOk);
    EXPECT_EQ(codec_state.encode_calls.load(), 1u);
    EXPECT_EQ(engine.stats().video_capture_pumped_frames, 0u);
    EXPECT_EQ(engine.stats().video_frames_sent, 1u);

    engine.close();
}
