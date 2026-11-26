/**
 * @file src/plugins/tests/test_hw_composite_codec.cpp
 * @brief The composite codec's telemetry forwarding (unit test).
 *
 * `HwCompositeCodec` pairs an encode-only codec with a decode-only one
 * so that the engine — which holds a *single* `plugins::IVideoCodec`
 * per session — can drive both directions.
 *
 * The composite is a pure adapter, so it can be tested without a GPU:
 * the two sub-codecs here are test doubles that report whatever the
 * test tells them to.
 *
 * ## What this test protects
 *
 * The engine's *outbound* telemetry (plan item B15) is what the
 * remote-desktop plan's §B.6 budget and the J5 capture/encode
 * benchmark are read against:
 *
 *   - `is_hardware_accelerated()` — "is the send path on a GPU?".  The
 *     interface's default is `false`, so a composite that did not
 *     forward the question would report a hardware encode session as a
 *     software one, and the benchmark's hardware-vs-software
 *     comparison — the whole point of the J5 artifact — would be
 *     measuring a label instead of the machine.
 *
 *   - `stats()` — the staged / zero-copy frame split and the encode
 *     latency.  A composite that dropped them would report "0 frames
 *     staged, 0 frames zero-copy" for every hardware session, which
 *     reads as "the encoder never ran".
 *
 *   - `backend_id()` — the identity of the *encoder* (the outbound
 *     direction), falling back to the decoder for a receive-only
 *     composite.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/plugins/hw_composite_codec.hpp>
#include <nimrtc/plugins/video_codec.hpp>

// `plugins::IVideoCodec` is the deprecated compatibility shim, and the
// composite is deliberately built on it (it is the single-instance
// contract the engine still drives).  Implementing it here would
// otherwise trip MSVC's C4996 on every mention.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif

namespace {

namespace np = nimrtc::plugins;
using nimrtc::plugins::HwCompositeCodec;

/**
 * @brief A codec double that reports a fixed telemetry snapshot.
 *
 * It encodes nothing and decodes nothing — the composite's job is to
 * *forward*, and that is what this test is about.
 */
class TelemetryCodec final : public np::IVideoCodec {
public:
    TelemetryCodec(std::string id, bool hw) : id_(std::move(id)), hw_(hw) {}

    // ---- IPlugin ----------------------------------------------------------
    const char* name() const noexcept override { return "TelemetryCodec"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    // ---- IVideoCodec ------------------------------------------------------
    bool can_encode() const noexcept override { return can_encode_; }
    bool can_decode() const noexcept override { return can_decode_; }

    np::VideoCodecKind kind() const noexcept override {
        return np::VideoCodecKind::kH264;
    }
    std::string_view codec_name() const noexcept override { return "h264"; }

    np::Status encode(const np::VideoFrame&, std::uint8_t*, std::size_t,
                       np::EncodedVideoFrame&) noexcept override {
        return np::kOk;
    }
    np::Status decode(const np::EncodedVideoFrame&, np::VideoFrame&,
                       std::uint8_t* const*) noexcept override {
        return np::kOk;
    }
    np::Status force_keyframe() noexcept override { return np::kOk; }

    np::Status set_params(const np::VideoEncoderParams&) noexcept override {
        return np::kOk;
    }
    np::VideoEncoderParams params() const noexcept override { return {}; }

    np::VideoCodecConfig config() const noexcept override { return cfg_; }
    np::Status update_config(np::VideoCodecConfig cfg) noexcept override {
        cfg_ = std::move(cfg);
        return np::kOk;
    }

    np::VideoCodecStats stats() const noexcept override {
        np::VideoCodecStats s;
        s.frames_encoded             = frames_encoded_;
        s.bytes_encoded              = bytes_encoded_;
        s.frames_encoded_staged      = staged_;
        s.frames_encoded_zero_copy   = zero_copy_;
        s.keyframes_encoded          = keyframes_;
        s.encode_latency_us_total    = latency_total_;
        s.encode_latency_us_max      = latency_max_;
        return s;
    }

    std::uint8_t payload_type() const noexcept override { return 102; }
    std::string_view backend_id() const noexcept override { return id_; }
    bool is_hardware_accelerated() const noexcept override { return hw_; }

    void set_can_encode(bool v) noexcept { can_encode_ = v; }
    void set_can_decode(bool v) noexcept { can_decode_ = v; }
    void set_frames_encoded(std::uint64_t v) noexcept { frames_encoded_ = v; }
    void set_bytes_encoded(std::uint64_t v) noexcept { bytes_encoded_ = v; }
    void set_staged(std::uint64_t v) noexcept { staged_ = v; }
    void set_zero_copy(std::uint64_t v) noexcept { zero_copy_ = v; }
    void set_keyframes(std::uint64_t v) noexcept { keyframes_ = v; }
    void set_latency(std::uint64_t total, std::uint64_t max) noexcept {
        latency_total_ = total;
        latency_max_ = max;
    }

    // ---- The zero-copy seam ------------------------------------------------
    //
    // The double answers the *capability* and records the *call*, so the
    // test can tell the two apart — which is the whole point: a
    // composite that answers "yes, I can take a surface" but never
    // forwards the call is the worst of both worlds.
    bool supports_zero_copy_encode() const noexcept override {
        return zero_copy_encode_;
    }
    bool supports_zero_copy_decode() const noexcept override {
        return zero_copy_decode_;
    }
    np::Status encode_zero_copy(const np::GpuBuffer& gpu,
                                np::EncodedVideoFrame& encoded_out) noexcept override {
        ++zero_copy_encode_calls_;
        if (!zero_copy_encode_) return np::kErrUnsupported;
        last_gpu_ = &gpu;
        encoded_out.payload_type = 102;
        // The payload is a *span*, so it needs a backing buffer that
        // outlives the call — the double owns one.
        encoded_bytes_.assign(kEncodedBytes, 0xAB);
        encoded_out.payload = nimrtc::core::ByteSpan(encoded_bytes_);
        return np::kOk;
    }
    np::Status decode_zero_copy(const np::EncodedVideoFrame& encoded,
                                np::GpuBuffer& out_surface) noexcept override {
        ++zero_copy_decode_calls_;
        if (!zero_copy_decode_) return np::kErrUnsupported;
        (void)encoded;
        (void)out_surface;
        return np::kOk;
    }

    std::uint32_t zero_copy_encode_calls() const noexcept {
        return zero_copy_encode_calls_;
    }
    std::uint32_t zero_copy_decode_calls() const noexcept {
        return zero_copy_decode_calls_;
    }
    const np::GpuBuffer* last_encoded_gpu() const noexcept { return last_gpu_; }

    void set_zero_copy_capabilities(bool encode, bool decode) noexcept {
        zero_copy_encode_ = encode;
        zero_copy_decode_ = decode;
    }

private:
    static constexpr std::size_t kEncodedBytes = 64;

    std::string   id_;
    bool          hw_ = false;
    bool          can_encode_ = true;
    bool          can_decode_ = true;
    bool          zero_copy_encode_ = false;
    bool          zero_copy_decode_ = false;
    std::uint32_t zero_copy_encode_calls_ = 0;
    std::uint32_t zero_copy_decode_calls_ = 0;
    const np::GpuBuffer* last_gpu_ = nullptr;
    std::vector<std::uint8_t> encoded_bytes_;
    std::uint64_t frames_encoded_ = 0;
    std::uint64_t bytes_encoded_ = 0;
    std::uint64_t staged_ = 0;
    std::uint64_t zero_copy_ = 0;
    std::uint64_t keyframes_ = 0;
    std::uint64_t latency_total_ = 0;
    std::uint64_t latency_max_ = 0;
    np::VideoCodecConfig cfg_{};
};

}  // namespace

// ---------------------------------------------------------------------------
// The hardware flag: the outbound direction's identity.
// ---------------------------------------------------------------------------
TEST(HwCompositeCodec, TheHardwareFlagComesFromTheEncoder) {
    auto enc = std::make_unique<TelemetryCodec>("nvenc", /*hw=*/true);
    auto dec = std::make_unique<TelemetryCodec>("nvdec", /*hw=*/true);
    enc->set_can_decode(false);
    dec->set_can_encode(false);

    HwCompositeCodec composite(np::VideoCodecConfig{}, std::move(enc),
                               std::move(dec));

    // A hardware encoder must be reported as accelerated — the J5
    // benchmark's hardware-vs-software comparison is read off this.
    EXPECT_TRUE(composite.is_hardware_accelerated());
}

TEST(HwCompositeCodec, ASoftwareEncoderIsNotAccelerated) {
    auto enc = std::make_unique<TelemetryCodec>("openh264", /*hw=*/false);
    auto dec = std::make_unique<TelemetryCodec>("nvdec", /*hw=*/true);
    enc->set_can_decode(false);
    dec->set_can_encode(false);

    HwCompositeCodec composite(np::VideoCodecConfig{}, std::move(enc),
                               std::move(dec));

    // The *decoder* is on a GPU, but the *send* path is not — and the
    // budget §B.6 writes is a send-side budget.
    EXPECT_FALSE(composite.is_hardware_accelerated());
}

TEST(HwCompositeCodec, AReceiveOnlyCompositeFallsBackToTheDecoder) {
    auto dec = std::make_unique<TelemetryCodec>("nvdec", /*hw=*/true);
    dec->set_can_encode(false);

    HwCompositeCodec composite(np::VideoCodecConfig{}, nullptr, std::move(dec));

    EXPECT_TRUE(composite.is_hardware_accelerated());
    EXPECT_EQ(composite.backend_id(), "nvdec");
}

TEST(HwCompositeCodec, AnEmptyCompositeIsNotAccelerated) {
    HwCompositeCodec composite(np::VideoCodecConfig{}, nullptr, nullptr);
    EXPECT_FALSE(composite.is_hardware_accelerated());
    EXPECT_EQ(composite.backend_id(), "unknown");
}

// ---------------------------------------------------------------------------
// The statistics: the staged / zero-copy split and the encode latency.
// ---------------------------------------------------------------------------
TEST(HwCompositeCodec, TheEncoderStatisticsAreForwarded) {
    auto enc = std::make_unique<TelemetryCodec>("nvenc", /*hw=*/true);
    enc->set_can_decode(false);
    enc->set_frames_encoded(120);
    enc->set_bytes_encoded(480'000);
    enc->set_staged(120);
    enc->set_zero_copy(0);
    enc->set_keyframes(4);
    enc->set_latency(120 * 3000, 5200);

    auto dec = std::make_unique<TelemetryCodec>("nvdec", /*hw=*/true);
    dec->set_can_encode(false);

    HwCompositeCodec composite(np::VideoCodecConfig{}, std::move(enc),
                               std::move(dec));

    const auto s = composite.stats();
    EXPECT_EQ(s.frames_encoded, 120u);
    EXPECT_EQ(s.bytes_encoded, 480'000u);
    // The split is what tells a host *how* the picture reached the
    // encoder; dropping it would read as "the encoder never ran".
    EXPECT_EQ(s.frames_encoded_staged, 120u);
    EXPECT_EQ(s.frames_encoded_zero_copy, 0u);
    EXPECT_EQ(s.keyframes_encoded, 4u);
    EXPECT_EQ(s.encode_latency_us_total, 120u * 3000u);
    EXPECT_EQ(s.encode_latency_us_max, 5200u);
}

TEST(HwCompositeCodec, TheBackendIdIsTheEncoders) {
    auto enc = std::make_unique<TelemetryCodec>("nvenc", /*hw=*/true);
    enc->set_can_decode(false);
    auto dec = std::make_unique<TelemetryCodec>("nvdec", /*hw=*/true);
    dec->set_can_encode(false);

    HwCompositeCodec composite(np::VideoCodecConfig{}, std::move(enc),
                               std::move(dec));

    // The encoder's id, because the engine's codec telemetry describes
    // the *outbound* stream.
    EXPECT_EQ(composite.backend_id(), "nvenc");
}

// ---------------------------------------------------------------------------
// The zero-copy seam: the capability *and* the call.
// ---------------------------------------------------------------------------
//
// The composite has to answer both questions consistently.  Forwarding
// `supports_zero_copy_encode()` while *not* forwarding
// `encode_zero_copy()` is the worst of both worlds: the engine believes
// the seam is live, wires the capture device's GPU callback, hands the
// surface to the codec, and is told "unsupported" for every frame — and
// because the capture device keeps no CPU mirror once the callback is
// attached, every frame is lost and the session shows a picture that
// never moves.

TEST(HwCompositeCodec, TheZeroCopyEncodeIsForwardedToTheEncoder) {
    auto enc = std::make_unique<TelemetryCodec>("nvenc", /*hw=*/true);
    enc->set_can_decode(false);
    enc->set_zero_copy_capabilities(/*encode=*/true, /*decode=*/false);

    auto dec = std::make_unique<TelemetryCodec>("nvdec", /*hw=*/true);
    dec->set_can_encode(false);

    HwCompositeCodec composite(np::VideoCodecConfig{}, std::move(enc),
                               std::move(dec));

    // The capability is the encoder's, so the composite must be able to
    // do what it claims.
    ASSERT_TRUE(composite.supports_zero_copy_encode());

    np::GpuBuffer gpu(np::GpuBackend::kWindowsD3D11Texture,
                       reinterpret_cast<void*>(0x1234), 1920, 1080,
                       nimrtc::video_frame::PixelFormat::kBGRA);
    np::EncodedVideoFrame encoded{};
    EXPECT_EQ(composite.encode_zero_copy(gpu, encoded), np::kOk);

    // The call really reached the encoder (not the interface's default,
    // which would have answered "unsupported").
    EXPECT_EQ(encoded.payload_type, 102);
    EXPECT_FALSE(encoded.payload.empty());
}

TEST(HwCompositeCodec, TheZeroCopyDecodeIsForwardedToTheDecoder) {
    auto enc = std::make_unique<TelemetryCodec>("nvenc", /*hw=*/true);
    enc->set_can_decode(false);

    auto dec = std::make_unique<TelemetryCodec>("nvdec", /*hw=*/true);
    dec->set_can_encode(false);
    dec->set_zero_copy_capabilities(/*encode=*/false, /*decode=*/true);

    HwCompositeCodec composite(np::VideoCodecConfig{}, std::move(enc),
                               std::move(dec));

    ASSERT_TRUE(composite.supports_zero_copy_decode());

    np::EncodedVideoFrame encoded{};
    np::GpuBuffer out;
    EXPECT_EQ(composite.decode_zero_copy(encoded, out), np::kOk);
}

TEST(HwCompositeCodec, ACompositeWithoutAZeroCopyEncoderRefusesTheCall) {
    // The encoder cannot take a surface: the composite must say so, and
    // the call must be refused rather than silently succeeding.
    auto enc = std::make_unique<TelemetryCodec>("openh264", /*hw=*/false);
    enc->set_can_decode(false);

    auto dec = std::make_unique<TelemetryCodec>("nvdec", /*hw=*/true);
    dec->set_can_encode(false);

    HwCompositeCodec composite(np::VideoCodecConfig{}, std::move(enc),
                               std::move(dec));

    EXPECT_FALSE(composite.supports_zero_copy_encode());

    np::GpuBuffer gpu(np::GpuBackend::kWindowsD3D11Texture,
                       reinterpret_cast<void*>(0x1234), 1920, 1080,
                       nimrtc::video_frame::PixelFormat::kBGRA);
    np::EncodedVideoFrame encoded{};
    EXPECT_EQ(composite.encode_zero_copy(gpu, encoded), np::kErrUnsupported);
}
