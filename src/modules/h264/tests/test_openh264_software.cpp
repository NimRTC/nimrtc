/**
 * @file src/modules/h264/tests/test_openh264_software.cpp
 * @brief OpenH264 software codec tests (remote-desktop plan WP-B).
 *
 * The vendored OpenH264 (BSD-2-Clause, Cisco) is the *real* software
 * H.264 codec NimRTC ships as the CPU bring-up baseline.  These tests
 * exercise the plugin seam end-to-end:
 *
 *   1. The backend table exposes an `openh264_h264` entry with priority
 *      above the synthetic stub, and the entry reports
 *      `backend_id() == "openh264"` (the stub reports "software-stub").
 *   2. Software encode -> software decode round-trips a synthetic I420
 *      frame: the decoded pixels match the source within a lossy-codec
 *      tolerance (mean absolute error and a correlation bound).
 *   3. The decoded picture is *real*: it is neither blank nor the
 *      synthetic stub pattern.
 *   4. Metadata (frame_seq / capture_ts / rtp_timestamp) survives the
 *      round-trip.
 *
 * The whole file is compiled only when `NIMRTC_PLUGINS_OPENH264_ON` is
 * defined -- i.e. when the vendored source tree is present and the option
 * is on.  Without it, the CMake target is not built at all (the stub
 * tests in test_h264_codec.cpp cover the fallback path).
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/h264/bitstream.hpp>
#include <nimrtc/h264/codec_plugin.hpp>
#include <nimrtc/h264/hw_backends.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/video_frame/frame.hpp>
#include <nimrtc/video_payload/h264.hpp>

namespace plugins = nimrtc::plugins;
namespace vf      = nimrtc::video_frame;

namespace {

// ---------------------------------------------------------------------------
// Synthetic source picture
//
// A smooth ramp with a hard edge: the ramp gives the rate controller
// something to chew on, the edge gives the test a sharp feature to look
// for in the decoded output.  Everything stays in the 16..235 range so
// the codec does not spend its budget on clipping.
// ---------------------------------------------------------------------------

constexpr std::uint32_t kWidth  = 320;
constexpr std::uint32_t kHeight = 240;

/** Fill one I420 picture with a deterministic pattern.
 *
 *  @param stride_y  Row stride of the luma plane, in bytes.
 *  @param stride_c  Row stride of each chroma plane, in bytes.
 */
void fill_pattern(std::uint8_t* y, std::uint8_t* u, std::uint8_t* v,
                  std::uint32_t width, std::uint32_t height,
                  std::uint32_t stride_y, std::uint32_t stride_c) {
    for (std::uint32_t row = 0; row < height; ++row) {
        for (std::uint32_t col = 0; col < width; ++col) {
            // A vertical ramp plus a horizontal band that flips the sign,
            // so the encoder has to spend bits on a real edge.
            std::uint32_t value = 16 + (col * 200) / width;
            if ((row / 16) % 2 == 1) value = 235 - (row * 8) % 200;
            y[static_cast<std::size_t>(row) * stride_y + col] =
                static_cast<std::uint8_t>(value);
        }
    }
    // Neutral chroma (grey) -- the pattern is luma-only on purpose, so a
    // chroma mismatch cannot mask a luma mismatch.
    const std::uint32_t cw = width / 2;
    const std::uint32_t ch = height / 2;
    for (std::uint32_t row = 0; row < ch; ++row) {
        for (std::uint32_t col = 0; col < cw; ++col) {
            u[static_cast<std::size_t>(row) * stride_c + col] = 128;
            v[static_cast<std::size_t>(row) * stride_c + col] = 128;
        }
    }
}

/** Build a VideoFrame that owns its pixels (the encoder reads the
 *  buffer's own strides, so the frame must carry a real VideoFrameBuffer
 *  rather than a bare plane pointer). */
plugins::VideoFrame make_source_frame(std::uint32_t width, std::uint32_t height) {
    const vf::VideoFrameLayout layout =
        vf::compute_layout(vf::PixelFormat::kI420, width, height);
    auto buffer = std::make_shared<vf::VideoFrameBuffer>(layout);
    fill_pattern(buffer->plane(0), buffer->plane(1), buffer->plane(2),
                 width, height, layout.strides[0], layout.strides[1]);

    plugins::VideoFrame frame;
    frame.buffer = std::move(buffer);
    frame.info.frame_seq     = 42;
    frame.info.capture_ts_us = 1234567;
    frame.info.rtp_timestamp = 0x11223344;
    return frame;
}

/** Mean absolute error between two tightly-packed luma planes. */
double mean_abs_error(const std::vector<std::uint8_t>& a,
                      const std::vector<std::uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) return 255.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const int diff = static_cast<int>(a[i]) - static_cast<int>(b[i]);
        sum += (diff < 0 ? -diff : diff);
    }
    return sum / static_cast<double>(a.size());
}

/** The `openh264_h264` decoder backend.
 *
 *  The lookup is by *id* rather than through `select_decoder_backend()`
 *  so the test is deterministic on a host that also has a GPU: the
 *  NVENC / NVDEC entries have a much higher priority and would win the
 *  generic selection. */
const plugins::VideoDecoderBackend* openh264_backend() {
    nimrtc::h264::register_default_video_backends();
    for (const auto* be : plugins::HwVideoBackendRegistry::instance()
                             .list_decoders(plugins::VideoCodecKind::kH264)) {
        if (be->id == "openh264_h264") return be;
    }
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Backend table
// ---------------------------------------------------------------------------

TEST(Openh264Software, BackendIsRegisteredAboveTheStub) {
    const plugins::VideoDecoderBackend* be = openh264_backend();
    ASSERT_NE(be, nullptr);
    EXPECT_EQ(be->id, "openh264_h264");
    EXPECT_FALSE(be->is_hw);
    EXPECT_GT(be->priority, 0);
    EXPECT_TRUE(be->available(plugins::VideoCodecConfig{}));
}

TEST(Openh264Software, BackendIdDistinguishesRealCodecFromStub) {
    const plugins::VideoDecoderBackend* be = openh264_backend();
    ASSERT_NE(be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width  = kWidth;
    cfg.height = kHeight;
    auto codec = be->create(cfg);
    ASSERT_NE(codec, nullptr);

    // The real software codec says "openh264"; the synthetic stub says
    // "software-stub".  A host MUST be able to tell them apart.
    EXPECT_EQ(codec->backend_id(), "openh264");
    EXPECT_NE(codec->backend_id(), "software-stub");
    EXPECT_FALSE(codec->is_hardware_accelerated());

    codec.reset();
}

TEST(Openh264Software, StubStillReportsSoftwareStub) {
    // The stub survives as the fallback.  It must keep reporting the
    // honest label so a host can tell the two apart.
    nimrtc::h264::CodecPluginAdapter stub({});
    EXPECT_EQ(stub.backend_id(), "software-stub");
}

// ---------------------------------------------------------------------------
// Encode -> decode round-trip
// ---------------------------------------------------------------------------

TEST(Openh264Software, EncodeDecodeRoundTripsPixels) {
    const plugins::VideoDecoderBackend* be = openh264_backend();
    ASSERT_NE(be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width        = kWidth;
    cfg.height       = kHeight;
    cfg.fps          = 30;
    cfg.bitrate_bps  = 2000000;
    cfg.pixel_format = plugins::VideoPixelFormat::kI420;
    cfg.payload_type = 102;

    auto codec = be->create(cfg);
    ASSERT_NE(codec, nullptr);
    ASSERT_EQ(codec->open(), plugins::kOk);
    ASSERT_TRUE(codec->can_encode());
    ASSERT_TRUE(codec->can_decode());

    // ---- Encode -----------------------------------------------------------
    const plugins::VideoFrame source = make_source_frame(kWidth, kHeight);
    std::vector<std::uint8_t> bitstream(512 * 1024);
    plugins::EncodedVideoFrame encoded;
    ASSERT_EQ(codec->encode(source, bitstream.data(), bitstream.size(), encoded),
              plugins::kOk);

    EXPECT_GT(encoded.payload.size(), 0u);
    EXPECT_EQ(encoded.codec, plugins::VideoCodecKind::kH264);
    EXPECT_EQ(encoded.nalu_format, plugins::NaluFormat::kAnnexBStartCode);
    EXPECT_TRUE(encoded.is_keyframe);

    // The bitstream must be a *real* H.264 stream: SPS + PPS + IDR.
    nimrtc::h264::BitstreamParser parser;
    const auto nalus = parser.parse_all(encoded.payload);
    bool saw_sps = false, saw_pps = false, saw_idr = false;
    for (const auto& n : nalus) {
        if (n.type == nimrtc::video_payload::h264::NaluType::kSPS)  saw_sps = true;
        if (n.type == nimrtc::video_payload::h264::NaluType::kPPS)  saw_pps = true;
        if (n.type == nimrtc::video_payload::h264::NaluType::kSliceIDR) saw_idr = true;
    }
    EXPECT_TRUE(saw_sps);
    EXPECT_TRUE(saw_pps);
    EXPECT_TRUE(saw_idr);

    // ---- Decode -----------------------------------------------------------
    //
    // The decode contract has no stride parameter, so the caller's
    // planes are tightly packed (stride == width).  The decoded picture
    // is therefore compared against a tightly-packed copy of the source.
    std::vector<std::uint8_t> dec_y(static_cast<std::size_t>(kWidth) * kHeight);
    std::vector<std::uint8_t> dec_u(static_cast<std::size_t>(kWidth / 2) * (kHeight / 2));
    std::vector<std::uint8_t> dec_v(static_cast<std::size_t>(kWidth / 2) * (kHeight / 2));
    std::uint8_t* buffers[3] = {dec_y.data(), dec_u.data(), dec_v.data()};

    plugins::VideoFrame decoded;
    ASSERT_EQ(codec->decode(encoded, decoded, buffers), plugins::kOk);

    EXPECT_EQ(decoded.width(),  kWidth);
    EXPECT_EQ(decoded.height(), kHeight);
    EXPECT_EQ(decoded.info.frame_seq,     42u);
    EXPECT_EQ(decoded.info.capture_ts_us, 1234567);
    EXPECT_EQ(decoded.info.rtp_timestamp, 0x11223344u);

    // ---- Compare ----------------------------------------------------------
    const auto src = source.cpu_buffer();
    ASSERT_NE(src, nullptr);
    const std::uint32_t stride_y = src->layout().strides[0];

    std::vector<std::uint8_t> src_y_tight(static_cast<std::size_t>(kWidth) * kHeight);
    for (std::uint32_t row = 0; row < kHeight; ++row) {
        std::copy(src->plane(0) + static_cast<std::size_t>(row) * stride_y,
                  src->plane(0) + static_cast<std::size_t>(row) * stride_y + kWidth,
                  src_y_tight.begin() + static_cast<std::ptrdiff_t>(row * kWidth));
    }

    const double mae = mean_abs_error(src_y_tight, dec_y);
    // A lossy codec is not bit-exact; 12 grey levels of mean error on a
    // 16..235 luma ramp is a generous bound that still catches a wrong
    // picture, a blank frame, or the synthetic stub pattern.
    EXPECT_LT(mae, 12.0) << "mean absolute luma error too large";

    // The decoded picture must actually contain the source's structure:
    // compare the correlation of the row means rather than individual
    // pixels, so a global brightness offset does not fail the test.
    std::vector<double> src_row(kHeight), dec_row(kHeight);
    for (std::uint32_t row = 0; row < kHeight; ++row) {
        double s = 0.0, d = 0.0;
        for (std::uint32_t col = 0; col < kWidth; ++col) {
            s += src_y_tight[row * kWidth + col];
            d += dec_y[row * kWidth + col];
        }
        src_row[row] = s / kWidth;
        dec_row[row] = d / kWidth;
    }
    double num = 0.0, den_s = 0.0, den_d = 0.0;
    for (std::uint32_t row = 0; row < kHeight; ++row) {
        num   += src_row[row] * dec_row[row];
        den_s += src_row[row] * src_row[row];
        den_d += dec_row[row] * dec_row[row];
    }
    const double corr = num / (std::sqrt(den_s) * std::sqrt(den_d));
    EXPECT_GT(corr, 0.95) << "decoded picture does not correlate with the source";

    // ---- Stats ------------------------------------------------------------
    const plugins::VideoCodecStats s = codec->stats();
    EXPECT_EQ(s.frames_encoded, 1u);
    EXPECT_EQ(s.frames_decoded, 1u);
    EXPECT_GT(s.bytes_encoded, 0u);
    EXPECT_EQ(s.encode_errors, 0u);
    EXPECT_EQ(s.decode_errors, 0u);

    codec->close();
    codec.reset();
}

TEST(Openh264Software, DecodedPictureIsNotBlank) {
    // A decoder that returns an all-black frame would still satisfy a
    // loose round-trip check against a dark source; this test pins the
    // opposite direction: the decoded picture must carry real signal.
    const plugins::VideoDecoderBackend* be = openh264_backend();
    ASSERT_NE(be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width  = kWidth;
    cfg.height = kHeight;
    cfg.bitrate_bps = 2000000;

    auto codec = be->create(cfg);
    ASSERT_NE(codec, nullptr);
    ASSERT_EQ(codec->open(), plugins::kOk);

    const plugins::VideoFrame source = make_source_frame(kWidth, kHeight);
    std::vector<std::uint8_t> bitstream(512 * 1024);
    plugins::EncodedVideoFrame encoded;
    ASSERT_EQ(codec->encode(source, bitstream.data(), bitstream.size(), encoded),
              plugins::kOk);

    std::vector<std::uint8_t> dec_y(static_cast<std::size_t>(kWidth) * kHeight);
    std::vector<std::uint8_t> dec_u(static_cast<std::size_t>(kWidth / 2) * (kHeight / 2));
    std::vector<std::uint8_t> dec_v(static_cast<std::size_t>(kWidth / 2) * (kHeight / 2));
    std::uint8_t* buffers[3] = {dec_y.data(), dec_u.data(), dec_v.data()};

    plugins::VideoFrame decoded;
    ASSERT_EQ(codec->decode(encoded, decoded, buffers), plugins::kOk);

    const auto [min_it, max_it] =
        std::minmax_element(dec_y.begin(), dec_y.end());
    EXPECT_GT(*max_it, 100u) << "decoded luma has no bright pixels";
    EXPECT_LT(*min_it, 200u) << "decoded luma has no dark pixels";
    // Real signal means the plane is not a constant.
    EXPECT_GT(*max_it - *min_it, 30u);

    codec->close();
    codec.reset();
}

TEST(Openh264Software, EncodeAdaptsToAChangedGeometry) {
    // The OpenH264 encoder is configured for one geometry, but a host's
    // capture resolution can change at any time (and the engine's
    // configured default need not match the first frame the host
    // pushes).  A real codec must follow the picture, not reject it.
    const plugins::VideoDecoderBackend* be = openh264_backend();
    ASSERT_NE(be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width  = kWidth;
    cfg.height = kHeight;
    auto codec = be->create(cfg);
    ASSERT_NE(codec, nullptr);
    ASSERT_EQ(codec->open(), plugins::kOk);

    // A smaller frame: the encoder must re-configure and encode it.
    const plugins::VideoFrame smaller = make_source_frame(160, 120);

    std::vector<std::uint8_t> bitstream(512 * 1024);
    plugins::EncodedVideoFrame encoded;
    ASSERT_EQ(codec->encode(smaller, bitstream.data(), bitstream.size(), encoded),
              plugins::kOk);
    EXPECT_GT(encoded.payload.size(), 0u);
    EXPECT_TRUE(encoded.is_keyframe);

    // And the original geometry must work again afterwards (the codec
    // follows the picture in both directions).
    const plugins::VideoFrame original = make_source_frame(kWidth, kHeight);
    ASSERT_EQ(codec->encode(original, bitstream.data(), bitstream.size(), encoded),
              plugins::kOk);
    EXPECT_GT(encoded.payload.size(), 0u);

    codec->close();
    codec.reset();
}

TEST(Openh264Software, ForceKeyframeSucceeds) {
    const plugins::VideoDecoderBackend* be = openh264_backend();
    ASSERT_NE(be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width  = kWidth;
    cfg.height = kHeight;
    auto codec = be->create(cfg);
    ASSERT_NE(codec, nullptr);
    ASSERT_EQ(codec->open(), plugins::kOk);
    EXPECT_EQ(codec->force_keyframe(), plugins::kOk);

    codec->close();
    codec.reset();
}

// ---------------------------------------------------------------------------
// Factory wiring
// ---------------------------------------------------------------------------

TEST(Openh264Software, TheFactorySelectsTheRealSoftwareCodec) {
    // The engine holds ONE codec per session and drives both directions
    // through it, so the factory has to hand back a codec that can both
    // encode and decode.  With the vendored OpenH264 linked in, that is
    // the real software codec — not the synthetic stub — and the host
    // must be able to see the difference in the telemetry.
    nimrtc::h264::register_default_video_backends();

    plugins::VideoCodecConfig cfg;
    cfg.codec        = plugins::VideoCodecKind::kH264;
    cfg.width        = kWidth;
    cfg.height       = kHeight;
    cfg.bitrate_bps  = 2000000;
    cfg.pixel_format = plugins::VideoPixelFormat::kI420;

    nimrtc::h264::H264PluginFactory factory;
    std::unique_ptr<plugins::IVideoCodec> codec(factory.create(cfg));
    ASSERT_NE(codec, nullptr);
    ASSERT_EQ(codec->open(), plugins::kOk);

    EXPECT_TRUE(codec->can_encode());
    EXPECT_TRUE(codec->can_decode());
    EXPECT_EQ(codec->backend_id(), "openh264")
        << "the engine must run on the real software codec, not the stub";

    codec->close();
    codec.reset();
}
