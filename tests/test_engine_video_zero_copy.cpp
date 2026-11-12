/**
 * @file tests/test_engine_video_zero_copy.cpp
 * @brief Phase 3 / C3 — the GPU zero-copy video send path.
 *
 * The engine's `send_video()` normally *stages* the source frame into a
 * CPU `I420` buffer (a full-frame memcpy, plus a de-interleave for an
 * NV12 source) before handing it to the codec.  A hardware capture
 * source cannot avoid that copy unless the engine hands the native GPU
 * handle straight to the codec instead.
 *
 * C3 opens that path:
 *
 *   - `plugins::VideoSourceFrame` grew the additive GPU handle fields
 *     (`gpu_backend` / `gpu_handle` / `gpu_pitch_bytes`);
 *   - `EngineConfig::video_zero_copy` opts the host in (default: off);
 *   - when the flag is on, the frame carries a handle, AND the codec
 *     reports `supports_zero_copy_encode()`, the engine calls
 *     `encode_zero_copy()` and does NOT stage the frame.
 *
 * The three-way AND is the contract: turning the flag on must never
 * break a CPU source or a software codec.  These tests pin all four
 * combinations, plus the two failure modes (the codec refuses the
 * handle → fall back to the CPU path; the codec cannot take a handle at
 * all → the CPU path runs unchanged).
 *
 * @see docs/plan/architecture-remediation-plan.md §3.2 (C3).
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/video_codec.hpp>

namespace {

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
namespace np = nimrtc::plugins;

// ---------------------------------------------------------------------------
// A fake codec that can pretend to support the zero-copy path.
// ---------------------------------------------------------------------------

class ZeroCopyVideoCodec final : public np::IVideoCodec {
public:
    /** @param zero_copy_capable  what `supports_zero_copy_encode()` reports. */
    explicit ZeroCopyVideoCodec(bool zero_copy_capable)
        : zero_copy_capable_(zero_copy_capable) {}

    const char* name() const noexcept override { return "zero-copy-codec"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    bool can_encode() const noexcept override { return true; }
    bool can_decode() const noexcept override { return false; }
    np::VideoCodecKind kind() const noexcept override {
        return np::VideoCodecKind::kH264;
    }
    std::string_view codec_name() const noexcept override { return "zero-copy"; }

    np::Status encode(const np::VideoFrame&,
                      std::uint8_t* output,
                      std::size_t output_capacity,
                      np::EncodedVideoFrame& out) noexcept override {
        ++cpu_encode_calls_;
        return emit_idr(output, output_capacity, out);
    }

    np::Status decode(const np::EncodedVideoFrame&,
                      np::VideoFrame&,
                      std::uint8_t* const*) noexcept override {
        return np::kErrUnsupported;
    }

    np::Status force_keyframe() noexcept override { return np::kOk; }

    // ---- Zero-copy surface ------------------------------------------------

    bool supports_zero_copy_encode() const noexcept override {
        return zero_copy_capable_;
    }

    np::Status encode_zero_copy(const np::GpuBuffer& gpu,
                                 np::EncodedVideoFrame& out) noexcept override {
        ++zero_copy_calls_;
        last_backend_.store(gpu.handle().backend, std::memory_order_relaxed);
        last_handle_.store(gpu.handle().handle, std::memory_order_relaxed);
        last_pitch_.store(gpu.handle().native_pitch_bytes, std::memory_order_relaxed);
        last_width_.store(gpu.width(), std::memory_order_relaxed);
        last_height_.store(gpu.height(), std::memory_order_relaxed);

        if (zero_copy_should_fail_) {
            return np::kErrUnsupported;
        }
        // The bitstream shape is the same as the CPU path's, so the
        // sender can packetize it either way.
        static constexpr std::uint8_t kAnnexBIdr[] = {0x00, 0x00, 0x00, 0x01, 0x65};
        out.payload = nimrtc::core::ByteSpan{kAnnexBIdr, sizeof(kAnnexBIdr)};
        out.codec = np::VideoCodecKind::kH264;
        out.is_keyframe = true;
        out.nalu_format = np::NaluFormat::kAnnexBStartCode;
        out.payload_type = payload_type();
        return np::kOk;
    }

    np::VideoCodecConfig config() const noexcept override { return cfg_; }
    np::Status update_config(np::VideoCodecConfig cfg) noexcept override {
        cfg_ = std::move(cfg);
        return np::kOk;
    }
    np::VideoCodecStats stats() const noexcept override { return {}; }
    std::uint8_t payload_type() const noexcept override { return 102; }

    // ---- Test hooks -------------------------------------------------------

    void set_zero_copy_should_fail(bool v) noexcept { zero_copy_should_fail_ = v; }

    int cpu_encode_calls() const noexcept {
        return cpu_encode_calls_.load(std::memory_order_relaxed);
    }
    int zero_copy_calls() const noexcept {
        return zero_copy_calls_.load(std::memory_order_relaxed);
    }
    np::GpuBackend last_backend() const noexcept {
        return last_backend_.load(std::memory_order_relaxed);
    }
    void* last_handle() const noexcept {
        return last_handle_.load(std::memory_order_relaxed);
    }
    std::uint32_t last_pitch() const noexcept {
        return last_pitch_.load(std::memory_order_relaxed);
    }
    std::uint32_t last_width() const noexcept {
        return last_width_.load(std::memory_order_relaxed);
    }
    std::uint32_t last_height() const noexcept {
        return last_height_.load(std::memory_order_relaxed);
    }

private:
    /** Emit a minimal Annex-B IDR so the reference sender's NALU parser
     *  accepts the frame (a bare 0x65 byte has no start code and
     *  push_frame() would reject it). */
    static np::Status emit_idr(std::uint8_t* output,
                               std::size_t output_capacity,
                               np::EncodedVideoFrame& out) noexcept {
        static constexpr std::uint8_t kAnnexBIdr[] = {0x00, 0x00, 0x00, 0x01, 0x65};
        if (!output || output_capacity < sizeof(kAnnexBIdr)) {
            return np::kErrBufferTooSmall;
        }
        std::memcpy(output, kAnnexBIdr, sizeof(kAnnexBIdr));
        out.payload = nimrtc::core::ByteSpan{output, sizeof(kAnnexBIdr)};
        out.codec = np::VideoCodecKind::kH264;
        out.is_keyframe = true;
        out.nalu_format = np::NaluFormat::kAnnexBStartCode;
        out.payload_type = 102;
        return np::kOk;
    }

    bool zero_copy_capable_;
    bool zero_copy_should_fail_ = false;
    np::VideoCodecConfig cfg_{};

    std::atomic<int>           cpu_encode_calls_{0};
    std::atomic<int>           zero_copy_calls_{0};
    std::atomic<np::GpuBackend> last_backend_{np::GpuBackend::kNone};
    std::atomic<void*>         last_handle_{nullptr};
    std::atomic<std::uint32_t> last_pitch_{0};
    std::atomic<std::uint32_t> last_width_{0};
    std::atomic<std::uint32_t> last_height_{0};
};

class ZeroCopyVideoCodecFactory final : public np::IVideoCodecFactory {
public:
    explicit ZeroCopyVideoCodecFactory(bool zero_copy_capable)
        : zero_copy_capable_(zero_copy_capable) {}

    std::string_view id() const noexcept override {
        return zero_copy_capable_ ? "zero-copy-codec" : "cpu-only-codec";
    }
    std::string_view display_name() const noexcept override {
        return "Zero-copy video codec (test)";
    }
    np::IVideoCodec* create(np::VideoCodecConfig) const override {
        last_created() = new ZeroCopyVideoCodec(zero_copy_capable_);
        return last_created();
    }

    /** The instance the engine instantiated — the test inspects it. */
    static ZeroCopyVideoCodec*& last_created() {
        static ZeroCopyVideoCodec* instance = nullptr;
        return instance;
    }

private:
    bool zero_copy_capable_;
};

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class EngineVideoZeroCopy : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();

        static const ZeroCopyVideoCodecFactory capable(true);
        PluginRegistry::instance().register_video_codec(
            "zero-copy-codec", &capable);

        static const ZeroCopyVideoCodecFactory incapable(false);
        PluginRegistry::instance().register_video_codec(
            "cpu-only-codec", &incapable);
    }

    void SetUp() override {
        ZeroCopyVideoCodecFactory::last_created() = nullptr;
    }

    EngineConfig make_config(std::string codec_id) {
        EngineConfig cfg{};
        cfg.video_codec_name    = std::move(codec_id);
        cfg.video_receiver_name = "reference";
        cfg.video_sender_name   = "reference";
        cfg.video_source_name   = "memory";
        cfg.video_sink_name     = "headless";
        cfg.datachannel_backend = "";
        cfg.bwe_name            = "";
        cfg.scheduler_config.enabled = false;
        return cfg;
    }

    /** A CPU frame — no GPU handle. */
    static np::VideoSourceFrame make_cpu_frame() {
        np::VideoSourceFrame f{};
        f.width  = 320;
        f.height = 240;
        f.format = np::VideoPixelFormat::kI420;
        f.stride_y = 320;
        f.stride_u = 160;
        f.stride_v = 160;
        f.plane_y = reinterpret_cast<const std::uint8_t*>(&cpu_plane_y_);
        f.plane_u = reinterpret_cast<const std::uint8_t*>(&cpu_plane_u_);
        f.plane_v = reinterpret_cast<const std::uint8_t*>(&cpu_plane_v_);
        f.capture_ts_us = 33333;
        f.frame_seq     = 1;
        f.rtp_timestamp = 3000;
        return f;
    }

    /** A GPU frame — carries a native handle, no CPU planes. */
    static np::VideoSourceFrame make_gpu_frame(void* handle) {
        np::VideoSourceFrame f{};
        f.width  = 1920;
        f.height = 1080;
        f.format = np::VideoPixelFormat::kNV12;
        f.gpu_backend     = np::GpuBackend::kWindowsD3D11Texture;
        f.gpu_handle      = handle;
        f.gpu_pitch_bytes = 1920;
        f.capture_ts_us = 33333;
        f.frame_seq     = 1;
        f.rtp_timestamp = 3000;
        return f;
    }

private:
    static inline std::uint8_t cpu_plane_y_[320 * 240] = {};
    static inline std::uint8_t cpu_plane_u_[160 * 120] = {};
    static inline std::uint8_t cpu_plane_v_[160 * 120] = {};
};

// ---------------------------------------------------------------------------
// The additive GPU handle fields on `VideoSourceFrame`.
// ---------------------------------------------------------------------------

TEST_F(EngineVideoZeroCopy, VideoSourceFrameCarriesTheGpuHandle) {
    np::VideoSourceFrame f{};
    EXPECT_FALSE(f.has_gpu_handle()) << "a default frame is a CPU frame";

    f.gpu_backend = np::GpuBackend::kVulkanImage;
    f.gpu_handle  = nullptr;
    EXPECT_FALSE(f.has_gpu_handle())
        << "a backend without a handle is not a GPU frame";

    void* handle = reinterpret_cast<void*>(0xABCDEF);
    f.gpu_handle = handle;
    EXPECT_TRUE(f.has_gpu_handle());
    EXPECT_EQ(f.gpu_backend, np::GpuBackend::kVulkanImage);
    EXPECT_EQ(f.gpu_handle, handle);

    // The pitch is carried verbatim.
    f.gpu_pitch_bytes = 4096;
    EXPECT_EQ(f.gpu_pitch_bytes, 4096u);
}

// ---------------------------------------------------------------------------
// Case 1: the flag is off → the CPU staging path runs, even for a GPU frame.
// ---------------------------------------------------------------------------

TEST_F(EngineVideoZeroCopy, FlagOffKeepsTheCpuPathEvenForAGpuFrame) {
    EngineConfig cfg = make_config("zero-copy-codec");
    cfg.video_zero_copy = false;   // the default

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    auto* codec = ZeroCopyVideoCodecFactory::last_created();
    ASSERT_NE(codec, nullptr);

    void* handle = reinterpret_cast<void*>(0x1234);
    const auto rc = engine.send_video(make_gpu_frame(handle));
    EXPECT_EQ(rc, np::kOk);

    EXPECT_EQ(codec->zero_copy_calls(), 0)
        << "the zero-copy path must not be taken when the flag is off";
    EXPECT_EQ(codec->cpu_encode_calls(), 1)
        << "the CPU staging path must run instead";

    const auto stats = engine.stats();
    EXPECT_EQ(stats.video_zero_copy_frames, 0u);
    EXPECT_EQ(stats.video_zero_copy_fallbacks, 0u)
        << "the flag being off is not a fallback — it was never requested";

    engine.close();
}

// ---------------------------------------------------------------------------
// Case 2: the flag is on, the frame carries a handle, the codec supports
// zero-copy → the handle goes straight to the codec, no staging.
// ---------------------------------------------------------------------------

TEST_F(EngineVideoZeroCopy, FlagOnWithAGpuFrameTakesTheZeroCopyPath) {
    EngineConfig cfg = make_config("zero-copy-codec");
    cfg.video_zero_copy = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    auto* codec = ZeroCopyVideoCodecFactory::last_created();
    ASSERT_NE(codec, nullptr);

    void* handle = reinterpret_cast<void*>(0xDEADBEEF);
    const auto rc = engine.send_video(make_gpu_frame(handle));
    EXPECT_EQ(rc, np::kOk);

    EXPECT_EQ(codec->zero_copy_calls(), 1)
        << "the codec must receive the native handle";
    EXPECT_EQ(codec->cpu_encode_calls(), 0)
        << "the CPU staging path must be skipped entirely";

    // The handle, the backend, the pitch and the geometry must reach the
    // codec unchanged.
    EXPECT_EQ(codec->last_handle(), handle);
    EXPECT_EQ(codec->last_backend(), np::GpuBackend::kWindowsD3D11Texture);
    EXPECT_EQ(codec->last_pitch(), 1920u);
    EXPECT_EQ(codec->last_width(), 1920u);
    EXPECT_EQ(codec->last_height(), 1080u);

    const auto stats = engine.stats();
    EXPECT_EQ(stats.video_zero_copy_frames, 1u);
    EXPECT_EQ(stats.video_zero_copy_fallbacks, 0u);

    // The frame still reached the wire.
    auto* sender = engine.video_sender();
    ASSERT_NE(sender, nullptr);
    EXPECT_EQ(sender->stats().frames_pushed, 1u);

    engine.close();
}

// ---------------------------------------------------------------------------
// Case 3: the flag is on but the frame is a CPU frame → the CPU path runs.
// ---------------------------------------------------------------------------

TEST_F(EngineVideoZeroCopy, FlagOnWithACpuFrameKeepsTheCpuPath) {
    EngineConfig cfg = make_config("zero-copy-codec");
    cfg.video_zero_copy = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    auto* codec = ZeroCopyVideoCodecFactory::last_created();
    ASSERT_NE(codec, nullptr);

    const auto rc = engine.send_video(make_cpu_frame());
    EXPECT_EQ(rc, np::kOk);

    EXPECT_EQ(codec->zero_copy_calls(), 0)
        << "a CPU frame has no handle to hand over";
    EXPECT_EQ(codec->cpu_encode_calls(), 1);

    const auto stats = engine.stats();
    EXPECT_EQ(stats.video_zero_copy_frames, 0u);
    EXPECT_EQ(stats.video_zero_copy_fallbacks, 0u)
        << "a CPU frame is not a fallback — there was no handle to begin with";

    engine.close();
}

// ---------------------------------------------------------------------------
// Case 4: the flag is on, the frame carries a handle, but the codec does
// NOT support zero-copy → the CPU path runs (and the miss is counted).
// ---------------------------------------------------------------------------

TEST_F(EngineVideoZeroCopy, FlagOnWithoutCodecSupportFallsBackToCpu) {
    EngineConfig cfg = make_config("cpu-only-codec");
    cfg.video_zero_copy = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    auto* codec = ZeroCopyVideoCodecFactory::last_created();
    ASSERT_NE(codec, nullptr);

    void* handle = reinterpret_cast<void*>(0x777);
    const auto rc = engine.send_video(make_gpu_frame(handle));
    EXPECT_EQ(rc, np::kOk);

    EXPECT_EQ(codec->zero_copy_calls(), 0)
        << "a codec that does not support zero-copy must not be called on it";
    EXPECT_EQ(codec->cpu_encode_calls(), 1)
        << "the CPU path must run instead";

    const auto stats = engine.stats();
    EXPECT_EQ(stats.video_zero_copy_frames, 0u);
    EXPECT_EQ(stats.video_zero_copy_fallbacks, 1u)
        << "the missed opportunity must be observable";

    engine.close();
}

// ---------------------------------------------------------------------------
// Case 5: the codec refuses the handle → the engine falls back to the CPU
// path (the frame is not lost).
// ---------------------------------------------------------------------------

TEST_F(EngineVideoZeroCopy, ARefusedZeroCopyEncodeFallsBackToTheCpuPath) {
    EngineConfig cfg = make_config("zero-copy-codec");
    cfg.video_zero_copy = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    auto* codec = ZeroCopyVideoCodecFactory::last_created();
    ASSERT_NE(codec, nullptr);
    codec->set_zero_copy_should_fail(true);

    // A GPU frame *with* CPU planes — the fallback needs something to
    // stage.  The geometry must match the CPU planes, because the
    // staging path reads `width` × `height` pixels out of them.
    np::VideoSourceFrame f = make_cpu_frame();
    f.gpu_backend     = np::GpuBackend::kWindowsD3D11Texture;
    f.gpu_handle      = reinterpret_cast<void*>(0x999);
    f.gpu_pitch_bytes = 320;

    const auto rc = engine.send_video(f);
    EXPECT_EQ(rc, np::kOk) << "a refused zero-copy encode must not lose the frame";

    EXPECT_EQ(codec->zero_copy_calls(), 1);
    EXPECT_EQ(codec->cpu_encode_calls(), 1)
        << "the engine must retry on the CPU path";

    const auto stats = engine.stats();
    EXPECT_EQ(stats.video_zero_copy_frames, 0u);
    EXPECT_EQ(stats.video_zero_copy_fallbacks, 1u);

    engine.close();
}

// ---------------------------------------------------------------------------
// The zero-copy path never stages a CPU buffer, so a GPU-only frame (no
// CPU planes at all) still encodes successfully.
// ---------------------------------------------------------------------------

TEST_F(EngineVideoZeroCopy, AGpuOnlyFrameNeedsNoCpuPlanes) {
    EngineConfig cfg = make_config("zero-copy-codec");
    cfg.video_zero_copy = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    auto* codec = ZeroCopyVideoCodecFactory::last_created();
    ASSERT_NE(codec, nullptr);

    const np::VideoSourceFrame f = make_gpu_frame(reinterpret_cast<void*>(0x1));
    ASSERT_EQ(f.plane_y, nullptr) << "this frame is GPU-only by construction";

    const auto rc = engine.send_video(f);
    EXPECT_EQ(rc, np::kOk);

    EXPECT_EQ(codec->zero_copy_calls(), 1);
    EXPECT_EQ(codec->cpu_encode_calls(), 0);

    const auto stats = engine.stats();
    EXPECT_EQ(stats.video_zero_copy_frames, 1u);

    engine.close();
}

} // namespace
