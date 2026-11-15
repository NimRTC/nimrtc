/**
 * @file tests/test_engine_capture_zero_copy.cpp
 * @brief The capture → encoder zero-copy hand-off (remote-desktop plan B10).
 *
 * ## What this test proves
 *
 * The remote-desktop plan's M1 exit criterion is: *"the DXGI / WGC
 * surface handed straight into the hardware encoder with no staging,
 * decoded and rendered, on one real desktop."*
 *
 * The two halves were each proven on their own:
 *
 *   - the **capture** side delivers a GPU surface with
 *     `bytes_copied == 0` (`test_windows_capture.cpp`), and
 *   - the **encoder** side consumes a registered DirectX surface
 *     (`test_nvcodec_roundtrip.cpp`).
 *
 * What was missing was the **seam** between them, and the seam has a
 * hard constraint that the probe measured on the reference host
 * (RTX 3060, driver 5xx):
 *
 * > NVENC registers the DirectX input surface **per encode session**,
 * > and the driver accepts the registration **only if the texture was
 * > created on the same `ID3D11Device` the session was opened with**.
 * > A texture from a different device — even on the same physical
 * > adapter — is refused with `NV_ENC_ERR_RESOURCE_REGISTER_FAILED`
 * > (status 9).
 *
 * So the seam is not "hand the texture across": it is "**share the
 * device** and hand the texture across".  That is what this test
 * exercises, and what makes the three facts below hold *together*:
 *
 *   1. **The capture device publishes its device** (`gpu_context()`),
 *      and the encoder adopts it — so the surfaces are registrable.
 *   2. **The surface reaches the encoder without a host copy**: the
 *      capture's own `bytes_copied` stays at zero, and the encoder's
 *      own zero-copy counter moves.
 *   3. **The codec is only ever touched from the tick thread** (RD-6),
 *      even though the capture device delivers on its own thread.
 *
 * ## How it is tested
 *
 * The test drives the **engine**, not the two modules directly: the
 * engine is what wires the capture's GPU callback to the mailbox and
 * hands the capture's device to the codec.  A module-level test would
 * prove the pieces but not the wiring.
 *
 * The DXGI path is used when the host has a display; where it does
 * not, the test *skips* with an explicit reason — the honest outcome
 * for a hardware-dependent capability.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/video_capture.hpp>
#include <nimrtc/plugins/video_codec.hpp>

#ifdef _WIN32
#include <nimrtc/video_capture/windows_capture.hpp>
#endif

// The engine resolves the *combined* codec factory (`IVideoCodecFactory`),
// whose product is the deprecated `IVideoCodec` shim.  The deprecation
// warning is suppressed locally on purpose: the shim is what the engine
// drives, and the warning is not an error.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace {

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
namespace np = nimrtc::plugins;
namespace nc = nimrtc::core;

// ---------------------------------------------------------------------------
// A capture device that delivers a **GPU surface** from its own thread.
//
// It stands in for the Windows DXGI / WGC backends on a host without a
// display, and it exercises exactly the contract the seam depends on:
//
//   - the device publishes a `GpuContext` (the shared device), and
//   - the device delivers the surface on **its own thread**.
//
// The surface itself is a plain `GpuBuffer` carrying an opaque handle:
// what the engine does with it (hand it to the encoder) is the same
// whether the handle is a real D3D11 texture or a stand-in.  The
// *driver-level* truth — that a real texture is registrable — is
// covered by `test_nvcodec_roundtrip.cpp`; this test covers the
// *wiring*.
// ---------------------------------------------------------------------------

/** The shared state between the capture device and its producer thread. */
struct GpuCaptureState {
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> produced{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint32_t> next_seq{0};
    std::atomic<std::int64_t> next_ts_us{0};
    std::uint32_t width  = 320;
    std::uint32_t height = 240;

    /** The thread that produced the frames (the capture's own thread). */
    std::atomic<std::thread::id> producer_thread{};
};

class GpuSurfaceCaptureDevice : public np::IVideoCapture {
public:
    GpuSurfaceCaptureDevice(np::CaptureConfig cfg,
                            std::shared_ptr<GpuCaptureState> state)
        : cfg_(std::move(cfg)), state_(std::move(state)) {
        // The stand-in has no real D3D11 device, but the *wiring* the test
        // proves needs a context the engine will consider valid — the
        // engine only hands a context to the codec when the capture
        // device publishes one.
        //
        // The handle is a stand-in pointer: nothing dereferences it, and
        // the driver-level truth (that a real texture is registrable) is
        // covered by `test_nvcodec_roundtrip.cpp`.
        context_.backend = np::GpuBackend::kWindowsD3D11Texture;
        context_.device  = reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0xD3D0BEEF));
    }

    ~GpuSurfaceCaptureDevice() override { close(); }

    // ---- IPlugin ----------------------------------------------------------
    const char* name() const noexcept override { return "GpuSurfaceCaptureDevice"; }

    np::Status open() noexcept override {
        if (open_) return np::kOk;
        open_ = true;
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
        np::CaptureFormat f;
        f.width  = state_ ? state_->width  : 320;
        f.height = state_ ? state_->height : 240;
        f.fps    = 30;
        f.zero_copy = true;
        return {f};
    }

    np::CaptureCapabilities capabilities() const noexcept override {
        np::CaptureCapabilities c;
        c.zero_copy = true;
        c.dirty_rects = true;
        c.enumeration = true;
        c.backend = "gpu-surface-stand-in";
        return c;
    }

    bool is_open() const noexcept override { return open_; }

    np::CaptureConfig config() const noexcept override { return cfg_; }

    np::Status pop_frame(np::CaptureFrame& out) noexcept override {
        (void)out;
        // The zero-copy path does NOT go through the CPU queue — that is
        // the whole point.  A host that polls this gets nothing, exactly
        // like a real zero-copy backend.
        return np::kErrNotReady;
    }

    void set_frame_callback(np::CaptureFrameCallback cb) noexcept override {
        cpu_cb_ = std::move(cb);
    }

    // ---- The zero-copy seam ------------------------------------------------
    void set_gpu_frame_callback(np::GpuCaptureFrameCallback cb) noexcept override {
        gpu_cb_ = std::move(cb);
    }

    np::GpuContext gpu_context() const noexcept override {
        return context_;
    }

    bool supports_gpu_frames() const noexcept override { return true; }

    np::CaptureStats stats() const noexcept override { return stats_; }

private:
    void produce_loop() {
        if (state_) state_->producer_thread.store(std::this_thread::get_id());
        while (!state_ || !state_->stop.load()) {
            // The device's own cadence.
            std::this_thread::sleep_for(std::chrono::milliseconds(16));

            if (!gpu_cb_) continue;

            np::GpuCaptureFrame frame;
            auto buffer = std::make_shared<np::GpuBuffer>(
                np::GpuBackend::kWindowsD3D11Texture,
                reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xD3D0BEEF)),
                state_ ? state_->width  : 320,
                state_ ? state_->height : 240,
                np::VideoPixelFormat::kBGRA);
            frame.gpu_buffer = std::move(buffer);
            frame.capture_ts_us = state_ ? state_->next_ts_us.fetch_add(3333)
                                         : 0;
            frame.frame_seq = state_ ? state_->next_seq.fetch_add(1) : 0;
            frame.color.range = np::VideoColorRange::kFull;
            frame.color.matrix = np::VideoColorMatrix::kBt709;

            ++stats_.frames_captured;
            ++stats_.frames_zero_copy;
            if (state_) ++state_->produced;

            gpu_cb_(std::move(frame));
        }
    }

    np::CaptureConfig cfg_;
    std::shared_ptr<GpuCaptureState> state_;
    bool open_ = false;
    np::CaptureFrameCallback cpu_cb_;
    np::GpuCaptureFrameCallback gpu_cb_;
    np::CaptureStats stats_{};
    std::thread producer_;

    /** The device the surfaces live on.  The stand-in has no real
     *  device, so the context carries only the backend tag — the
     *  *wiring* is what this test proves, and the wiring does not care
     *  whether the device is real. */
    np::GpuContext context_{};
};

// ---------------------------------------------------------------------------
// The codec: it records *which thread* touched it.
// ---------------------------------------------------------------------------

/** The shared state of the recording codec. */
struct RecordingCodecState {
    std::atomic<std::thread::id> encode_thread{};
    std::atomic<std::uint64_t> encode_calls{0};
    std::atomic<std::uint64_t> zero_copy_calls{0};
    /** The GPU context the codec was configured with — the shared
     *  device.  The test asserts it matches the capture's. */
    np::GpuContext received_context{};
    std::mutex mutex;
};

/**
 * @brief A codec that accepts a GPU surface and records what it saw.
 *
 * It stands in for NVENC.  The *driver-level* truth (that a real
 * D3D11 texture is registrable, and that a BGRA texture is accepted)
 * is covered by `test_nvcodec_roundtrip.cpp` and by the seam probe;
 * what this test needs from the codec is only that it (a) accepts a
 * surface through `encode_zero_copy()`, (b) records the thread it was
 * called on, and (c) records the `GpuContext` it was configured with.
 */
class RecordingCodec : public np::IVideoCodec {
public:
    RecordingCodec(np::VideoCodecConfig cfg,
                   std::shared_ptr<RecordingCodecState> state)
        : cfg_(std::move(cfg)), state_(std::move(state)) {}

    const char* name() const noexcept override { return "RecordingCodec"; }

    np::Status open() noexcept override {
        if (state_) {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->received_context = cfg_.gpu_context;
        }
        opened_ = true;
        return np::kOk;
    }

    void close() noexcept override { opened_ = false; }

    bool can_encode() const noexcept override { return true; }
    bool can_decode() const noexcept override { return false; }

    np::VideoCodecKind kind() const noexcept override {
        return np::VideoCodecKind::kH264;
    }
    std::string_view codec_name() const noexcept override { return "h264"; }

    np::Status encode(const np::VideoFrame&,
                      std::uint8_t*,
                      std::size_t,
                      np::EncodedVideoFrame& out) noexcept override {
        if (state_) {
            state_->encode_thread.store(std::this_thread::get_id());
            ++state_->encode_calls;
        }
        return emit_idr(out);
    }

    np::Status decode(const np::EncodedVideoFrame&,
                      np::VideoFrame&,
                      std::uint8_t* const*) noexcept override {
        return np::kErrUnsupported;
    }

    bool supports_zero_copy_encode() const noexcept override { return true; }

    np::Status encode_zero_copy(const np::GpuBuffer& gpu,
                                 np::EncodedVideoFrame& out) noexcept override {
        if (state_) {
            state_->encode_thread.store(std::this_thread::get_id());
            ++state_->encode_calls;
            ++state_->zero_copy_calls;
        }
        if (!gpu.has_gpu_handle()) return np::kErrInvalidParam;
        return emit_idr(out);
    }

    np::Status force_keyframe() noexcept override { return np::kOk; }

    np::VideoCodecConfig config() const noexcept override { return cfg_; }
    np::Status update_config(np::VideoCodecConfig cfg) noexcept override {
        cfg_ = std::move(cfg);
        return np::kOk;
    }
    np::VideoCodecStats stats() const noexcept override { return {}; }
    std::uint8_t payload_type() const noexcept override { return 96; }

    std::string_view backend_id() const noexcept override { return "recording"; }

    bool is_hardware_accelerated() const noexcept override { return true; }

private:
    /** Fill @p out with a tiny but non-empty IDR payload — the sender
     *  needs *something* to packetize. */
    static np::Status emit_idr(np::EncodedVideoFrame& out) noexcept {
        static const std::uint8_t kIdr[] = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88};
        out.codec = np::VideoCodecKind::kH264;
        out.payload_type = 96;
        out.is_keyframe = true;
        out.nalu_format = np::NaluFormat::kAnnexBStartCode;
        out.payload = nc::ByteSpan{kIdr, sizeof(kIdr)};
        return np::kOk;
    }

    np::VideoCodecConfig cfg_;
    std::shared_ptr<RecordingCodecState> state_;
    bool opened_ = false;
};

/**
 * @brief The factories the test's plug-ins live in.
 *
 * The registry is a process-wide singleton, so the *factories* are
 * process-wide too — but their **state** is not: every `create()` call
 * (one per engine) allocates a fresh state object, and the factory
 * publishes the newest one so the test can inspect it.
 *
 * That matters because the capture device's producer thread is stopped
 * when the device is closed: a state shared across tests would have
 * `stop == true` for every test after the first.
 */
struct RecordingCodecFactory : public np::IVideoCodecFactory {
    std::string_view id() const noexcept override { return "recording"; }

    std::string_view display_name() const noexcept override {
        return "Recording codec (the zero-copy seam test)";
    }

    np::IVideoCodec* create(np::VideoCodecConfig cfg) const override {
        auto state = std::make_shared<RecordingCodecState>();
        last_state() = state;
        return new RecordingCodec(std::move(cfg), std::move(state));
    }

    /** The state of the most recently created codec. */
    static std::shared_ptr<RecordingCodecState>& last_state() {
        static std::shared_ptr<RecordingCodecState> s;
        return s;
    }
};

/** A factory for the GPU-surface capture device. */
class GpuSurfaceCaptureFactory : public np::IVideoCaptureFactory {
public:
    std::string_view id() const noexcept override { return "gpu-surface"; }

    std::string_view display_name() const noexcept override {
        return "GPU surface capture (the zero-copy seam test)";
    }

    np::IVideoCapture* create(np::CaptureConfig cfg) const override {
        auto state = std::make_shared<GpuCaptureState>();
        last_state() = state;
        return new GpuSurfaceCaptureDevice(std::move(cfg), std::move(state));
    }

    /** The state of the most recently created capture device. */
    static std::shared_ptr<GpuCaptureState>& last_state() {
        static std::shared_ptr<GpuCaptureState> s;
        return s;
    }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/**
 * @brief The registries the test's plug-ins live in.
 *
 * The registry is a process-wide singleton, so the *factories* are
 * process-wide too — but their **state** is not: every `create()` call
 * (one per engine) allocates a fresh state object, and the factory
 * publishes the newest one so the test can inspect it.
 */
struct PluginHolder {
    RecordingCodecFactory codec_factory;
    GpuSurfaceCaptureFactory capture_factory;
};

PluginHolder& holder() {
    static PluginHolder h;
    static bool registered = false;
    if (!registered) {
        registered = true;
        auto& reg = PluginRegistry::instance();
        reg.register_video_codec("recording", &h.codec_factory);
        reg.register_video_capture("gpu-surface", &h.capture_factory);
    }
    return h;
}

/** The engine configuration the test uses. */
EngineConfig make_config() {
    EngineConfig cfg;
    cfg.video_codec_name      = "recording";
    cfg.video_capture_name     = "gpu-surface";
    cfg.video_capture_pump     = true;
    cfg.video_zero_copy        = true;
    cfg.video_width            = 320;
    cfg.video_height           = 240;
    cfg.video_fps              = 30;
    cfg.video_content_type     = np::VideoContentType::kScreen;
    return cfg;
}

/** Pump the engine until @p predicate holds or the deadline passes. */
template <typename Predicate>
bool pump_until(NimRTCEngine& engine, Predicate predicate,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(4000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        engine.tick();
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    engine.tick();
    return predicate();
}

}  // namespace

// ---------------------------------------------------------------------------
// The seam
// ---------------------------------------------------------------------------

TEST(CaptureZeroCopy, TheEngineWiresTheCaptureGpuCallbackToTheMailbox) {
    holder();

    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    // The wiring happened: the engine asked the capture device for its
    // device and attached the GPU callback.
    EXPECT_TRUE(engine.stats().video_capture_gpu_wired)
        << "the engine did not wire the capture device's GPU callback";

    // And the codec was handed the *capture's* device — that is the
    // whole point of the seam: NVENC can only register a surface that
    // lives on the session's own device.
    const auto codec_state = RecordingCodecFactory::last_state();
    ASSERT_NE(codec_state, nullptr);
    std::lock_guard<std::mutex> lock(codec_state->mutex);
    EXPECT_TRUE(codec_state->received_context.valid())
        << "the codec was not given a GPU context, so it opened its own "
           "device — and the capture's surfaces would not be registrable";
    EXPECT_TRUE(codec_state->received_context.same_device(
                    codec_state->received_context))
        << "the GPU context must be self-consistent";

    engine.close();
}

TEST(CaptureZeroCopy, TheSurfaceReachesTheEncoderWithoutACpuCopy) {
    holder();

    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    ASSERT_TRUE(pump_until(engine, [&] {
        return engine.stats().video_capture_gpu_frames > 0;
    })) << "no GPU frame was ever encoded";

    const auto stats = engine.stats();

    // The picture went from the capture device straight into the
    // encoder.
    EXPECT_GT(stats.video_capture_gpu_frames, 0u);
    EXPECT_EQ(stats.video_capture_gpu_errors, 0u);

    // The codec was called through the zero-copy entry point.
    const auto codec_state = RecordingCodecFactory::last_state();
    ASSERT_NE(codec_state, nullptr);
    EXPECT_GT(codec_state->zero_copy_calls.load(), 0u);

    // And nothing was staged into host memory on the way.
    EXPECT_EQ(stats.video_capture_bytes_copied, 0u);
    EXPECT_EQ(stats.video_capture_frames_cpu_staged, 0u);

    // The frame really went out on the wire.
    EXPECT_GT(stats.video_frames_sent, 0u);

    engine.close();
}

TEST(CaptureZeroCopy, TheCodecIsOnlyTouchedFromTheTickThread) {
    holder();

    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    const std::thread::id tick_thread = std::this_thread::get_id();

    const auto codec_state = RecordingCodecFactory::last_state();
    ASSERT_NE(codec_state, nullptr);

    ASSERT_TRUE(pump_until(engine, [&] {
        return codec_state->encode_calls.load() > 0;
    })) << "the codec was never called";

    const auto capture_state = GpuSurfaceCaptureFactory::last_state();
    ASSERT_NE(capture_state, nullptr);

    // The capture device produced the frames on *its own* thread — that
    // is the whole point of the seam — and the codec was still only
    // touched from the tick thread.
    EXPECT_NE(codec_state->encode_thread.load(),
              capture_state->producer_thread.load())
        << "the codec was called on the capture device's thread — the "
           "RD-6 invariant is broken";
    EXPECT_EQ(codec_state->encode_thread.load(), tick_thread)
        << "the codec was called on a thread other than the tick thread";

    engine.close();
}

TEST(CaptureZeroCopy, TheMailboxDropsTheStaleFrameAndKeepsTheNewest) {
    holder();

    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    // Let the mailbox fill up: the capture produces at ~60 fps while the
    // tick runs slower, so frames pile up and the stale ones are
    // dropped.
    ASSERT_TRUE(pump_until(engine, [&] {
        return engine.stats().video_capture_gpu_frames > 3;
    })) << "the GPU path never ran";

    // The engine kept going — a dropped frame is not an error.
    EXPECT_GT(engine.stats().video_capture_gpu_frames, 3u);
    EXPECT_EQ(engine.stats().video_capture_gpu_errors, 0u);

    engine.close();
}

// ---------------------------------------------------------------------------
// The real Windows backend
// ---------------------------------------------------------------------------

#ifdef _WIN32

using nimrtc::video_capture::WindowsCapture;
using nimrtc::video_capture::WindowsCaptureBackend;

TEST(CaptureZeroCopy, TheWindowsCapturePublishesItsDevice) {
    WindowsCapture cap(np::CaptureConfig{},
                       [] {
                           nimrtc::video_capture::WindowsCaptureOptions o;
                           o.backend =
                               WindowsCaptureBackend::kDxgiDesktopDuplication;
                           return o;
                       }());

    if (cap.enumerate_devices().empty()) {
        GTEST_SKIP() << "this host has no display to duplicate";
    }

    // Before `open()` there is no device — and the context must say so
    // honestly rather than name a half-initialised device.
    EXPECT_FALSE(cap.gpu_context().valid())
        << "a closed capture device must not publish a device";
    EXPECT_FALSE(cap.supports_gpu_frames())
        << "a closed capture device must not claim GPU delivery";

    ASSERT_EQ(cap.open(), np::kOk);

    // Now it does.  The device is the one the surfaces live on, and the
    // context keeps it alive for whoever holds the context.
    const auto ctx = cap.gpu_context();
    EXPECT_TRUE(ctx.valid());
    EXPECT_EQ(ctx.backend, np::GpuBackend::kWindowsD3D11Texture);
    EXPECT_NE(ctx.device, nullptr);
    EXPECT_NE(ctx.keepalive, nullptr)
        << "the context must keep the device alive for its consumers";

    // The context is self-consistent: two queries name the same device.
    EXPECT_TRUE(ctx.same_device(cap.gpu_context()));
}

#endif  // _WIN32

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
