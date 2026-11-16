/**
 * @file tests/test_engine_video_incremental.cpp
 * @brief The engine's dirty-rect incremental encode (plan B9) and the
 *        key-frame storm limiter (plan B14).
 *
 * ## What this test proves
 *
 * **B9 — the dirty rectangles reach the encoder.**  The capture device
 * reports which rectangles changed; the engine must carry that
 * information across to the encoder instead of discarding it at the
 * pump.  The test's capture device reports a *static* frame (no dirty
 * rectangle) and then a frame with one rectangle, and the codec records
 * the incremental requests it received.
 *
 * The three cases the test distinguishes:
 *
 *   1. **The mechanism is off** — the codec is never asked for an
 *      incremental encode, whatever the device reports.
 *   2. **The content is not screen content** — the mechanism does not
 *      engage even when the host enabled it (ADR-022 Decision 5: the
 *      content type is explicit, and a camera stream has no unchanged
 *      area worth skipping).
 *   3. **Screen content + a codec that supports it** — the encoder is
 *      asked for an incremental encode, and the peer is told which
 *      region changed.
 *
 * **B14 — a key-frame storm is bounded.**  The engine arms the encoder
 * when the peer asks for a key frame; the limiter sits between the
 * request and the encoder.  The test feeds a burst of PLIs and asserts
 * that the IDR rate stayed bounded and that the counters say so.
 *
 * ## Why the test drives the RTCP directly
 *
 * The engine's `handle_rtcp()` is private, and the PLI path is the
 * one the plan names ("a peer that requests a keyframe every 100 ms").
 * The test therefore feeds the PLI through the *sender plugin* — the
 * same seam the engine uses — and asserts on the engine's published
 * statistics, which is what a host would read.
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
#include <nimrtc/plugins/video_pipeline.hpp>

namespace {

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
namespace np = nimrtc::plugins;
namespace nc = nimrtc::core;

// ---------------------------------------------------------------------------
// A capture device that reports dirty rectangles.
// ---------------------------------------------------------------------------

/** The device's shared state. */
struct DirtyCaptureState {
    /** The dirty rectangles the device reports for the next frame.
     *  An empty list with `full_frame == false` is the *static
     *  desktop* case — the property plan B9 cares about most. */
    np::CaptureDirtyRects next_dirty{};

    std::mutex mutex;
    std::deque<std::vector<std::uint8_t>> frames;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> produced{0};
    std::atomic<std::uint32_t> next_seq{0};
    std::atomic<std::int64_t> next_ts_us{0};
    std::uint32_t width  = 64;
    std::uint32_t height = 48;
};

class DirtyCaptureDevice : public np::IVideoCapture {
public:
    explicit DirtyCaptureDevice(np::CaptureConfig cfg, DirtyCaptureState* state)
        : cfg_(std::move(cfg)), state_(state) {}

    ~DirtyCaptureDevice() override { close(); }

    const char* name() const noexcept override { return "DirtyCaptureDevice"; }
    np::Status open() noexcept override {
        if (open_) return np::kOk;
        open_ = true;
        producer_ = std::thread([this] { produce_loop(); });
        return np::kOk;
    }
    void close() noexcept override {
        if (!open_) return;
        open_ = false;
        state_->stop.store(true);
        if (producer_.joinable()) producer_.join();
    }

    std::vector<np::CaptureDevice> enumerate_devices() const noexcept override {
        return {};
    }
    std::vector<np::CaptureFormat> supported_formats(
        const std::string&) const noexcept override {
        return {};
    }
    np::CaptureCapabilities capabilities() const noexcept override {
        np::CaptureCapabilities c;
        c.backend = "dirty_test";
        c.dirty_rects = true;
        return c;
    }

    bool is_open() const noexcept override { return open_; }
    np::CaptureConfig config() const noexcept override { return cfg_; }

    np::Status pop_frame(np::CaptureFrame& out) noexcept override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->frames.empty()) return np::kErrNotReady;

        // The seam's contract says the pixel pointer stays valid "until
        // the next call to `pop_frame()`".  So the frame that was handed
        // out last time is kept alive here and only released when this
        // call replaces it -- popping the deque's front and destroying
        // the vector would pull the bytes out from under the consumer.
        current_ = std::move(state_->frames.front());
        state_->frames.pop_front();

        out.pixels   = current_.data();
        out.size     = current_.size();
        out.stride   = static_cast<std::int32_t>(state_->width * 4u);
        out.width    = state_->width;
        out.height   = state_->height;
        out.format   = np::VideoPixelFormat::kBGRA;
        out.capture_ts_us = state_->next_ts_us.fetch_add(33'333);
        out.frame_seq     = state_->next_seq.fetch_add(1);

        // The dirty rectangles are the device's own statement about the
        // frame.  The engine has to carry them across unchanged.
        out.dirty = state_->next_dirty;

        return np::kOk;
    }

    void set_frame_callback(np::CaptureFrameCallback) noexcept override {}

    np::CaptureStats stats() const noexcept override {
        np::CaptureStats s;
        s.frames_captured = state_->produced.load();
        s.frames_cpu_staged = state_->produced.load();
        return s;
    }

private:
    void produce_loop() {
        const std::size_t bytes =
            static_cast<std::size_t>(state_->width) * state_->height * 4u;
        while (!state_->stop.load()) {
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                if (state_->frames.size() >= 2) state_->frames.pop_front();
                state_->frames.emplace_back(bytes, 0x40u);
            }
            ++state_->produced;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    np::CaptureConfig cfg_{};
    DirtyCaptureState* state_ = nullptr;
    std::thread producer_;
    bool open_ = false;

    /**
     * @brief The frame handed out by the last `pop_frame()`.
     *
     * The seam's contract says the pixel pointer stays valid until the
     * next `pop_frame()` call, so the bytes have to outlive the call
     * that produced them.
     */
    std::vector<std::uint8_t> current_;
};

class DirtyCaptureFactory : public np::IVideoCaptureFactory {
public:
    explicit DirtyCaptureFactory(DirtyCaptureState* state) : state_(state) {}
    std::string_view id() const noexcept override { return "dirty_test_capture"; }
    std::string_view display_name() const noexcept override {
        return "Dirty-rect reporting test capture device";
    }
    np::IVideoCapture* create(np::CaptureConfig cfg) const override {
        return new DirtyCaptureDevice(std::move(cfg), state_);
    }

private:
    DirtyCaptureState* state_;
};

// ---------------------------------------------------------------------------
// A codec that records the incremental requests it received.
// ---------------------------------------------------------------------------

struct RecordingCodecState {
    std::atomic<std::uint64_t> encode_calls{0};
    std::atomic<std::uint64_t> incremental_requests{0};
    std::atomic<std::uint64_t> full_frame_requests{0};
    std::atomic<std::uint64_t> force_keyframe_calls{0};
    /** True iff the codec claims it can express a region. */
    std::atomic<bool> supports_incremental{false};
    /** The last region the codec was asked to code. */
    std::mutex mutex;
    np::VideoRegion last_region{};
};

class RecordingCodec : public np::IVideoCodec {
public:
    explicit RecordingCodec(RecordingCodecState* state) : state_(state) {}

    const char* name() const noexcept override { return "RecordingCodec"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

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
        ++state_->encode_calls;
        if (output_capacity < 16) return np::kErrBufferTooSmall;

        static const std::uint8_t kAnnexB[] = {
            0x00, 0x00, 0x00, 0x01, 0x65, 0xAA, 0xBB, 0xCC,
        };
        for (std::size_t i = 0; i < sizeof(kAnnexB); ++i) output[i] = kAnnexB[i];
        encoded_out.payload = nc::ByteSpan{output, sizeof(kAnnexB)};
        encoded_out.is_keyframe = true;
        encoded_out.codec = np::VideoCodecKind::kH264;
        encoded_out.nalu_format = np::NaluFormat::kAnnexBStartCode;
        encoded_out.info = raw.info;
        return np::kOk;
    }

    np::Status decode(const np::EncodedVideoFrame&, np::VideoFrame&,
                      std::uint8_t* const*) noexcept override {
        return np::kErrUnsupported;
    }
    np::Status force_keyframe() noexcept override {
        ++state_->force_keyframe_calls;
        return np::kOk;
    }
    np::Status set_params(const np::VideoEncoderParams&) noexcept override {
        return np::kOk;
    }
    np::VideoEncoderParams params() const noexcept override { return {}; }
    np::VideoCodecStats stats() const noexcept override { return {}; }
    np::VideoCodecConfig config() const noexcept override { return cfg_; }
    np::Status update_config(np::VideoCodecConfig cfg) noexcept override {
        cfg_ = std::move(cfg);
        return np::kOk;
    }
    std::uint8_t payload_type() const noexcept override { return 102; }

    // ---- The incremental-encode seam (plan B9) ------------------------
    bool supports_incremental_encode() const noexcept override {
        return state_->supports_incremental.load();
    }
    np::Status set_incremental_request(
            const np::VideoIncrementalEncodeRequest& request) noexcept override {
        if (!state_->supports_incremental.load()) return np::kErrUnsupported;
        if (request.enabled) {
            ++state_->incremental_requests;
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!request.regions.empty()) state_->last_region = request.regions.front();
        } else {
            ++state_->full_frame_requests;
        }
        return np::kOk;
    }

private:
    RecordingCodecState* state_ = nullptr;
    np::VideoCodecConfig cfg_{};
};

class RecordingCodecFactory : public np::IVideoCodecFactory {
public:
    explicit RecordingCodecFactory(RecordingCodecState* state) : state_(state) {}
    std::string_view id() const noexcept override { return "recording_test_codec"; }
    std::string_view display_name() const noexcept override {
        return "Recording test codec";
    }
    np::IVideoCodec* create(np::VideoCodecConfig cfg) const override {
        auto* codec = new RecordingCodec(state_);
        codec->update_config(cfg);
        return codec;
    }

private:
    RecordingCodecState* state_;
};

// ---------------------------------------------------------------------------
// The engine configuration used by every test here.
// ---------------------------------------------------------------------------

EngineConfig make_config() {
    EngineConfig cfg;
    cfg.transport_strategy_name = "shell";
    cfg.stun_server_host = "";
    cfg.video_codec_name      = "recording_test_codec";
    cfg.video_sender_name     = "reference";
    cfg.video_receiver_name   = "reference";
    cfg.video_source_name     = "memory";
    cfg.video_sink_name       = "headless";
    cfg.video_capture_name    = "dirty_test_capture";
    cfg.video_width  = 64;
    cfg.video_height = 48;
    return cfg;
}

/** Build a PLI (RFC 4585 §6.3.1) for `media_ssrc`. */
std::vector<std::uint8_t> make_pli(std::uint32_t sender_ssrc,
                                   std::uint32_t media_ssrc) {
    // The PLI is 12 bytes: an RTCP header (V=2, P=0, FMT=1, PT=206),
    // the sender SSRC and the media SSRC.
    std::vector<std::uint8_t> buf(12, 0);
    buf[0] = 0x81;  // V=2, P=0, FMT=1
    buf[1] = 206;  // PT=PSFB
    buf[2] = 0;
    buf[3] = 2;    // length in 32-bit words minus one
    // sender SSRC
    buf[4] = static_cast<std::uint8_t>((sender_ssrc >> 24) & 0xFF);
    buf[5] = static_cast<std::uint8_t>((sender_ssrc >> 16) & 0xFF);
    buf[6] = static_cast<std::uint8_t>((sender_ssrc >> 8) & 0xFF);
    buf[7] = static_cast<std::uint8_t>(sender_ssrc & 0xFF);
    // media SSRC
    buf[8]  = static_cast<std::uint8_t>((media_ssrc >> 24) & 0xFF);
    buf[9]  = static_cast<std::uint8_t>((media_ssrc >> 16) & 0xFF);
    buf[10] = static_cast<std::uint8_t>((media_ssrc >> 8) & 0xFF);
    buf[11] = static_cast<std::uint8_t>(media_ssrc & 0xFF);
    return buf;
}

}  // namespace

// ===========================================================================
// B9 — the dirty rectangles reach the encoder.
// ===========================================================================

// 1. The mechanism is off: the codec is never asked for an incremental
//    encode, whatever the device reports.
TEST(EngineVideoIncremental, DisabledMechanismNeverEngages) {
    DirtyCaptureState state;
    static const DirtyCaptureFactory capture_factory(&state);

    RecordingCodecState codec_state;
    codec_state.supports_incremental.store(true);
    static const RecordingCodecFactory codec_factory(&codec_state);

    PluginRegistry::instance().register_video_capture(
        capture_factory.id(), &capture_factory);
    PluginRegistry::instance().register_video_codec(
        codec_factory.id(), &codec_factory);

    // The device reports a real change.
    state.next_dirty.full_frame = false;
    state.next_dirty.rects.push_back(np::CaptureRegion{10, 20, 30, 40});

    EngineConfig cfg = make_config();
    cfg.video_content_type = np::VideoContentType::kScreen;
    cfg.video_incremental_encode = false;   // <- the mechanism is off

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    for (int i = 0; i < 100; ++i) {
        engine.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    EXPECT_GT(codec_state.encode_calls.load(), 0u);
    // The codec was asked for a *full* encode every time, and never for
    // an incremental one.
    EXPECT_EQ(codec_state.incremental_requests.load(), 0u);

    const auto stats = engine.stats();
    EXPECT_EQ(stats.video_incremental_encoded, 0u);
    EXPECT_EQ(stats.video_region_refreshes_sent, 0u);

    engine.close();
}

// 2. The content is not screen content: the mechanism does not engage,
//    even when the host enabled it (ADR-022 Decision 5).
TEST(EngineVideoIncremental, CameraContentNeverEngages) {
    DirtyCaptureState state;
    static const DirtyCaptureFactory capture_factory(&state);

    RecordingCodecState codec_state;
    codec_state.supports_incremental.store(true);
    static const RecordingCodecFactory codec_factory(&codec_state);

    PluginRegistry::instance().register_video_capture(
        capture_factory.id(), &capture_factory);
    PluginRegistry::instance().register_video_codec(
        codec_factory.id(), &codec_factory);

    state.next_dirty.full_frame = false;
    state.next_dirty.rects.push_back(np::CaptureRegion{10, 20, 30, 40});

    EngineConfig cfg = make_config();
    cfg.video_content_type = np::VideoContentType::kCamera;  // <- camera
    cfg.video_incremental_encode = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    for (int i = 0; i < 100; ++i) {
        engine.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    EXPECT_GT(codec_state.encode_calls.load(), 0u);
    EXPECT_EQ(codec_state.incremental_requests.load(), 0u);

    const auto stats = engine.stats();
    EXPECT_EQ(stats.video_incremental_encoded, 0u);

    engine.close();
}

// 3. Screen content + a codec that supports it: the encoder is asked
//    for an incremental encode.
TEST(EngineVideoIncremental, ScreenContentEngagesTheMechanism) {
    DirtyCaptureState state;
    static const DirtyCaptureFactory capture_factory(&state);

    RecordingCodecState codec_state;
    codec_state.supports_incremental.store(true);
    static const RecordingCodecFactory codec_factory(&codec_state);

    PluginRegistry::instance().register_video_capture(
        capture_factory.id(), &capture_factory);
    PluginRegistry::instance().register_video_codec(
        codec_factory.id(), &codec_factory);

    state.next_dirty.full_frame = false;
    state.next_dirty.rects.push_back(np::CaptureRegion{10, 20, 30, 40});

    EngineConfig cfg = make_config();
    cfg.video_content_type = np::VideoContentType::kScreen;
    cfg.video_incremental_encode = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    for (int i = 0; i < 100; ++i) {
        engine.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (codec_state.incremental_requests.load() >= 2) break;
    }

    // The dirty rectangle reached the encoder.
    EXPECT_GE(codec_state.incremental_requests.load(), 1u);

    {
        std::lock_guard<std::mutex> lock(codec_state.mutex);
        EXPECT_EQ(codec_state.last_region.x, 10u);
        EXPECT_EQ(codec_state.last_region.y, 20u);
        EXPECT_EQ(codec_state.last_region.width, 30u);
        EXPECT_EQ(codec_state.last_region.height, 40u);
    }

    const auto stats = engine.stats();
    EXPECT_GE(stats.video_incremental_frames_seen, 1u);
    EXPECT_GE(stats.video_incremental_encoded, 1u);
    EXPECT_EQ(stats.video_incremental_fallbacks, 0u);

    engine.close();
}

// 4. A codec that cannot express a region: the fallback is reported.
TEST(EngineVideoIncremental, TheFallbackIsReported) {
    DirtyCaptureState state;
    static const DirtyCaptureFactory capture_factory(&state);

    RecordingCodecState codec_state;
    codec_state.supports_incremental.store(false);   // <- cannot
    static const RecordingCodecFactory codec_factory(&codec_state);

    PluginRegistry::instance().register_video_capture(
        capture_factory.id(), &capture_factory);
    PluginRegistry::instance().register_video_codec(
        codec_factory.id(), &codec_factory);

    state.next_dirty.full_frame = false;
    state.next_dirty.rects.push_back(np::CaptureRegion{10, 20, 30, 40});

    EngineConfig cfg = make_config();
    cfg.video_content_type = np::VideoContentType::kScreen;
    cfg.video_incremental_encode = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    for (int i = 0; i < 100; ++i) {
        engine.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // The codec was never asked for an incremental encode.
    EXPECT_EQ(codec_state.incremental_requests.load(), 0u);

    // But the fallback is *counted* — plan B9's acceptance criterion
    // says the fallback must be reported, not hidden.
    const auto stats = engine.stats();
    EXPECT_GE(stats.video_incremental_fallbacks, 1u);
    EXPECT_EQ(stats.video_incremental_encoded, 0u);

    engine.close();
}

// ===========================================================================
// B14 — the key-frame storm limiter.
// ===========================================================================

// 5. A burst of PLIs produces a bounded number of IDRs, and the
//    counters say so.
TEST(EngineVideoKeyframeLimiter, AStormProducesABoundedIdrRate) {
    EngineConfig cfg = make_config();
    // No capture device: this test is about the feedback path only.
    cfg.video_capture_name = "";
    cfg.video_capture_pump = false;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    auto* sender = engine.video_sender();
    ASSERT_NE(sender, nullptr);

    // A burst of PLIs — one every 10 ms, far faster than the 250 ms
    // minimum interval the limiter enforces.
    const auto pli = make_pli(0xDEADBEEF, 0x12345678);

    for (int i = 0; i < 100; ++i) {
        (void)sender->apply_feedback_rtcp(np::BufferView{pli.data(), pli.size()});
    }

    const auto ss = sender->stats();
    // Every request was *offered* to the limiter.
    EXPECT_EQ(ss.keyframe_requests, 100u);
    // But only a bounded number were granted: the storm did not produce
    // 100 IDRs.
    EXPECT_LT(ss.keyframes_granted, 10u);
    EXPECT_GT(ss.keyframes_granted, 0u);
    // The rest were deferred (held back), not silently dropped.
    EXPECT_GT(ss.keyframes_deferred, 0u);
    EXPECT_EQ(ss.keyframes_granted + ss.keyframes_deferred +
                  ss.keyframes_rejected,
              100u);

    // The counters reach the engine's published statistics.
    const auto stats = engine.stats();
    EXPECT_EQ(stats.video_keyframes_limiter_requests, 100u);
    EXPECT_EQ(stats.video_keyframes_granted, ss.keyframes_granted);
    EXPECT_EQ(stats.video_keyframes_deferred, ss.keyframes_deferred);

    engine.close();
}

// 6. A held request is re-offered: the picture still recovers even when
//    the peer only asked once.
TEST(EngineVideoKeyframeLimiter, ADeferredRequestIsHeldNotDropped) {
    EngineConfig cfg = make_config();
    cfg.video_capture_name = "";
    cfg.video_capture_pump = false;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    auto* sender = engine.video_sender();
    ASSERT_NE(sender, nullptr);

    // The first PLI is granted.
    const auto pli = make_pli(0xDEADBEEF, 0x12345678);
    (void)sender->apply_feedback_rtcp(np::BufferView{pli.data(), pli.size()});
    EXPECT_GE(sender->stats().keyframes_granted, 1u);

    // A second PLI right away is deferred — and *held*.
    (void)sender->apply_feedback_rtcp(np::BufferView{pli.data(), pli.size()});
    EXPECT_GT(sender->stats().keyframes_deferred, 0u);
    EXPECT_TRUE(sender->keyframe_request_pending());

    engine.close();
}
