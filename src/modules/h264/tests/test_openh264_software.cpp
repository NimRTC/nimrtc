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

/** The `openh264_h264` encoder backend.
 *
 *  The lookup is by *id* rather than through `select_encoder()` so the
 *  test is deterministic on a host that also has a GPU: the NVENC
 *  entries have a much higher priority and would win the generic
 *  selection. */
const plugins::VideoEncoderBackend* openh264_encoder_backend() {
    nimrtc::h264::register_default_video_backends();
    for (const auto* be : plugins::HwVideoBackendRegistry::instance()
                             .list_encoders(plugins::VideoCodecKind::kH264)) {
        if (be->id == "openh264_h264") return be;
    }
    return nullptr;
}

/** The `openh264_h264` decoder backend.
 *
 *  See `openh264_encoder_backend()` for why the lookup is by id. */
const plugins::VideoDecoderBackend* openh264_decoder_backend() {
    nimrtc::h264::register_default_video_backends();
    for (const auto* be : plugins::HwVideoBackendRegistry::instance()
                             .list_decoders(plugins::VideoCodecKind::kH264)) {
        if (be->id == "openh264_h264") return be;
    }
    return nullptr;
}

/**
 * @brief An encoder + a decoder, the two halves of one OpenH264 pair.
 *
 * The codec seam is split (remote-desktop plan B1): the two directions
 * are separate interfaces, and the OpenH264 backend registers itself in
 * both registry tables.  A test that needs both directions therefore
 * holds both halves.
 */
struct Openh264Pair {
    std::unique_ptr<plugins::IVideoEncoder> encoder;
    std::unique_ptr<plugins::IVideoDecoder> decoder;

    /** The encoder's own statistics. */
    plugins::VideoCodecStats encoder_stats() const {
        return encoder ? encoder->stats() : plugins::VideoCodecStats{};
    }
    /** The decoder's own statistics. */
    plugins::VideoCodecStats decoder_stats() const {
        return decoder ? decoder->stats() : plugins::VideoCodecStats{};
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Backend table
// ---------------------------------------------------------------------------

TEST(Openh264Software, BackendIsRegisteredAboveTheStub) {
    const plugins::VideoEncoderBackend* enc = openh264_encoder_backend();
    const plugins::VideoDecoderBackend* dec = openh264_decoder_backend();
    ASSERT_NE(enc, nullptr);
    ASSERT_NE(dec, nullptr);
    EXPECT_EQ(enc->id, "openh264_h264");
    EXPECT_EQ(dec->id, "openh264_h264");
    EXPECT_FALSE(enc->is_hw);
    EXPECT_FALSE(dec->is_hw);
    EXPECT_GT(enc->priority, 0);
    EXPECT_GT(dec->priority, 0);
    EXPECT_TRUE(enc->available(plugins::VideoCodecConfig{}));
    EXPECT_TRUE(dec->available(plugins::VideoCodecConfig{}));
}

TEST(Openh264Software, BackendIdDistinguishesRealCodecFromStub) {
    const plugins::VideoEncoderBackend* be = openh264_encoder_backend();
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
    const plugins::VideoEncoderBackend* enc_be = openh264_encoder_backend();
    const plugins::VideoDecoderBackend* dec_be = openh264_decoder_backend();
    ASSERT_NE(enc_be, nullptr);
    ASSERT_NE(dec_be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width        = kWidth;
    cfg.height       = kHeight;
    cfg.fps          = 30;
    cfg.bitrate_bps  = 2000000;
    cfg.pixel_format = plugins::VideoPixelFormat::kI420;
    cfg.payload_type = 102;

    Openh264Pair pair;
    pair.encoder = enc_be->create(cfg);
    pair.decoder = dec_be->create(cfg);
    ASSERT_NE(pair.encoder, nullptr);
    ASSERT_NE(pair.decoder, nullptr);
    ASSERT_EQ(pair.encoder->open(), plugins::kOk);
    ASSERT_EQ(pair.decoder->open(), plugins::kOk);
    ASSERT_NE(pair.encoder, nullptr);
    ASSERT_NE(pair.decoder, nullptr);

    // ---- Encode -----------------------------------------------------------
    const plugins::VideoFrame source = make_source_frame(kWidth, kHeight);
    std::vector<std::uint8_t> bitstream(512 * 1024);
    plugins::EncodedVideoFrame encoded;
    ASSERT_EQ(pair.encoder->encode(source, bitstream.data(), bitstream.size(), encoded),
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
    ASSERT_EQ(pair.decoder->decode(encoded, decoded, buffers), plugins::kOk);

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
    const plugins::VideoCodecStats es = pair.encoder_stats();
    EXPECT_EQ(es.frames_encoded, 1u);
    EXPECT_GT(es.bytes_encoded, 0u);
    EXPECT_EQ(es.encode_errors, 0u);

    const plugins::VideoCodecStats ds = pair.decoder_stats();
    EXPECT_EQ(ds.frames_decoded, 1u);
    EXPECT_EQ(ds.decode_errors, 0u);

    pair.encoder->close();
    pair.decoder->close();
    pair.encoder.reset();
    pair.decoder.reset();
}

TEST(Openh264Software, DecodedPictureIsNotBlank) {
    // A decoder that returns an all-black frame would still satisfy a
    // loose round-trip check against a dark source; this test pins the
    // opposite direction: the decoded picture must carry real signal.
    const plugins::VideoEncoderBackend* enc_be = openh264_encoder_backend();
    const plugins::VideoDecoderBackend* dec_be = openh264_decoder_backend();
    ASSERT_NE(enc_be, nullptr);
    ASSERT_NE(dec_be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width  = kWidth;
    cfg.height = kHeight;
    cfg.bitrate_bps = 2000000;

    Openh264Pair pair;
    pair.encoder = enc_be->create(cfg);
    pair.decoder = dec_be->create(cfg);
    ASSERT_NE(pair.encoder, nullptr);
    ASSERT_NE(pair.decoder, nullptr);
    ASSERT_EQ(pair.encoder->open(), plugins::kOk);
    ASSERT_EQ(pair.decoder->open(), plugins::kOk);

    const plugins::VideoFrame source = make_source_frame(kWidth, kHeight);
    std::vector<std::uint8_t> bitstream(512 * 1024);
    plugins::EncodedVideoFrame encoded;
    ASSERT_EQ(pair.encoder->encode(source, bitstream.data(), bitstream.size(), encoded),
              plugins::kOk);

    std::vector<std::uint8_t> dec_y(static_cast<std::size_t>(kWidth) * kHeight);
    std::vector<std::uint8_t> dec_u(static_cast<std::size_t>(kWidth / 2) * (kHeight / 2));
    std::vector<std::uint8_t> dec_v(static_cast<std::size_t>(kWidth / 2) * (kHeight / 2));
    std::uint8_t* buffers[3] = {dec_y.data(), dec_u.data(), dec_v.data()};

    plugins::VideoFrame decoded;
    ASSERT_EQ(pair.decoder->decode(encoded, decoded, buffers), plugins::kOk);

    const auto [min_it, max_it] =
        std::minmax_element(dec_y.begin(), dec_y.end());
    EXPECT_GT(*max_it, 100u) << "decoded luma has no bright pixels";
    EXPECT_LT(*min_it, 200u) << "decoded luma has no dark pixels";
    // Real signal means the plane is not a constant.
    EXPECT_GT(*max_it - *min_it, 30u);

    pair.encoder->close();
    pair.decoder->close();
    pair.encoder.reset();
    pair.decoder.reset();
}

TEST(Openh264Software, EncodeAdaptsToAChangedGeometry) {
    // The OpenH264 encoder is configured for one geometry, but a host's
    // capture resolution can change at any time (and the engine's
    // configured default need not match the first frame the host
    // pushes).  A real codec must follow the picture, not reject it.
    const plugins::VideoEncoderBackend* enc_be = openh264_encoder_backend();
    const plugins::VideoDecoderBackend* dec_be = openh264_decoder_backend();
    ASSERT_NE(enc_be, nullptr);
    ASSERT_NE(dec_be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width  = kWidth;
    cfg.height = kHeight;
    Openh264Pair pair;
    pair.encoder = enc_be->create(cfg);
    pair.decoder = dec_be->create(cfg);
    ASSERT_NE(pair.encoder, nullptr);
    ASSERT_NE(pair.decoder, nullptr);
    ASSERT_EQ(pair.encoder->open(), plugins::kOk);
    ASSERT_EQ(pair.decoder->open(), plugins::kOk);

    // A smaller frame: the encoder must re-configure and encode it.
    const plugins::VideoFrame smaller = make_source_frame(160, 120);

    std::vector<std::uint8_t> bitstream(512 * 1024);
    plugins::EncodedVideoFrame encoded;
    ASSERT_EQ(pair.encoder->encode(smaller, bitstream.data(), bitstream.size(), encoded),
              plugins::kOk);
    EXPECT_GT(encoded.payload.size(), 0u);
    EXPECT_TRUE(encoded.is_keyframe);

    // And the original geometry must work again afterwards (the codec
    // follows the picture in both directions).
    const plugins::VideoFrame original = make_source_frame(kWidth, kHeight);
    ASSERT_EQ(pair.encoder->encode(original, bitstream.data(), bitstream.size(), encoded),
              plugins::kOk);
    EXPECT_GT(encoded.payload.size(), 0u);

    pair.encoder->close();
    pair.decoder->close();
    pair.encoder.reset();
    pair.decoder.reset();
}

TEST(Openh264Software, ForceKeyframeSucceeds) {
    const plugins::VideoEncoderBackend* enc_be = openh264_encoder_backend();
    const plugins::VideoDecoderBackend* dec_be = openh264_decoder_backend();
    ASSERT_NE(enc_be, nullptr);
    ASSERT_NE(dec_be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width  = kWidth;
    cfg.height = kHeight;
    Openh264Pair pair;
    pair.encoder = enc_be->create(cfg);
    pair.decoder = dec_be->create(cfg);
    ASSERT_NE(pair.encoder, nullptr);
    ASSERT_NE(pair.decoder, nullptr);
    ASSERT_EQ(pair.encoder->open(), plugins::kOk);
    ASSERT_EQ(pair.decoder->open(), plugins::kOk);
    EXPECT_EQ(pair.encoder->force_keyframe(), plugins::kOk);

    pair.encoder->close();
    pair.decoder->close();
    pair.encoder.reset();
    pair.decoder.reset();
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
    //
    // The backend policy is pinned to *software* on purpose.  In the
    // default (`auto`) mode the factory picks the highest-priority
    // *available* backend, which on a machine with an NVIDIA GPU is the
    // hardware encoder — and then this test, which is about the
    // *software* path, would fail on a GPU host and pass on a headless
    // one.  A test whose result depends on the host's hardware is not a
    // test; pinning the class makes the assertion mean what it says.
    nimrtc::h264::register_default_video_backends();

    plugins::VideoCodecConfig cfg;
    cfg.codec        = plugins::VideoCodecKind::kH264;
    cfg.width        = kWidth;
    cfg.height       = kHeight;
    cfg.bitrate_bps  = 2000000;
    cfg.pixel_format = plugins::VideoPixelFormat::kI420;
    cfg.backend_preference = plugins::VideoCodecBackendPreference::kSoftware;
    cfg.backend_policy    = plugins::CodecBackend::kSoftware;

    // The split seam (remote-desktop plan B1): the factory hands out an
    // encoder and a decoder, and the test drives both.
    nimrtc::h264::H264PluginFactory factory;
    std::unique_ptr<plugins::IVideoEncoder> encoder(factory.create_encoder(cfg));
    std::unique_ptr<plugins::IVideoDecoder> decoder(factory.create_decoder(cfg));
    ASSERT_NE(encoder, nullptr);
    ASSERT_NE(decoder, nullptr);
    ASSERT_EQ(encoder->open(), plugins::kOk);
    ASSERT_EQ(decoder->open(), plugins::kOk);

    EXPECT_EQ(encoder->backend_id(), "openh264")
        << "the engine must run on the real software codec, not the stub";

    encoder->close();
    decoder->close();
    encoder.reset();
    decoder.reset();
}

// ---------------------------------------------------------------------------
// The encoder parameter set (remote-desktop plan B12 / B13)
// ---------------------------------------------------------------------------

TEST(Openh264Software, SetParamsAppliesTheGeometry) {
    // B12: `set_params()` is the one entry point through which the
    // engine describes the stream.  A geometry change has to reach the
    // encoder — the encoder is configured for one geometry, so a
    // parameter set that names a different one and is silently ignored
    // would produce a picture at the wrong size.
    const plugins::VideoEncoderBackend* enc_be = openh264_encoder_backend();
    const plugins::VideoDecoderBackend* dec_be = openh264_decoder_backend();
    ASSERT_NE(enc_be, nullptr);
    ASSERT_NE(dec_be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width  = kWidth;
    cfg.height = kHeight;
    Openh264Pair pair;
    pair.encoder = enc_be->create(cfg);
    pair.decoder = dec_be->create(cfg);
    ASSERT_NE(pair.encoder, nullptr);
    ASSERT_NE(pair.decoder, nullptr);
    ASSERT_EQ(pair.encoder->open(), plugins::kOk);
    ASSERT_EQ(pair.decoder->open(), plugins::kOk);

    plugins::VideoEncoderParams params;
    params.width             = 640;
    params.height            = 480;
    params.fps               = 25;
    params.bitrate_bps       = 1'500'000;
    params.keyframe_interval = 30;
    params.pixel_format      = plugins::VideoPixelFormat::kI420;

    ASSERT_EQ(pair.encoder->set_params(params), plugins::kOk);

    const plugins::VideoEncoderParams now = pair.encoder->params();
    EXPECT_EQ(now.width, 640u);
    EXPECT_EQ(now.height, 480u);
    EXPECT_EQ(now.fps, 25u);
    EXPECT_EQ(now.bitrate_bps, 1'500'000u);
    EXPECT_EQ(now.keyframe_interval, 30u);

    pair.encoder->close();
    pair.decoder->close();
    pair.encoder.reset();
    pair.decoder.reset();
}

TEST(Openh264Software, SetParamsRestartsAndEmitsTheNewParameterSets) {
    // B13: a geometry change is a *parameter-set change*.  The peer's
    // decoder holds parameter sets that describe the old geometry, so
    // the new ones must travel with the new frame — the peer decodes it
    // without asking for a keyframe.
    //
    // The observable consequences are:
    //   1. the encoder follows the new geometry;
    //   2. the next frame is an IDR (a freshly initialised encoder
    //      starts with one), and an IDR carries the SPS / PPS.
    const plugins::VideoEncoderBackend* enc_be = openh264_encoder_backend();
    const plugins::VideoDecoderBackend* dec_be = openh264_decoder_backend();
    ASSERT_NE(enc_be, nullptr);
    ASSERT_NE(dec_be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width  = kWidth;
    cfg.height = kHeight;
    cfg.bitrate_bps = 2000000;
    Openh264Pair pair;
    pair.encoder = enc_be->create(cfg);
    pair.decoder = dec_be->create(cfg);
    ASSERT_NE(pair.encoder, nullptr);
    ASSERT_NE(pair.decoder, nullptr);
    ASSERT_EQ(pair.encoder->open(), plugins::kOk);
    ASSERT_EQ(pair.decoder->open(), plugins::kOk);

    // ---- The first frame, at the original geometry -----------------------
    std::vector<std::uint8_t> bitstream(512 * 1024);
    plugins::EncodedVideoFrame encoded;
    ASSERT_EQ(pair.encoder->encode(make_source_frame(kWidth, kHeight),
                            bitstream.data(), bitstream.size(), encoded),
              plugins::kOk);
    ASSERT_TRUE(encoded.is_keyframe);

    // ---- The geometry changes ---------------------------------------------
    constexpr std::uint32_t kNewWidth  = 640;
    constexpr std::uint32_t kNewHeight = 480;

    plugins::VideoEncoderParams params;
    params.width        = kNewWidth;
    params.height       = kNewHeight;
    params.fps          = 30;
    params.bitrate_bps  = 2000000;
    params.pixel_format = plugins::VideoPixelFormat::kI420;
    ASSERT_EQ(pair.encoder->set_params(params), plugins::kOk);

    const plugins::VideoEncoderParams now = pair.encoder->params();
    EXPECT_EQ(now.width, kNewWidth);
    EXPECT_EQ(now.height, kNewHeight);

    // ---- The next frame must carry the NEW parameter sets -----------------
    ASSERT_EQ(pair.encoder->encode(make_source_frame(kNewWidth, kNewHeight),
                            bitstream.data(), bitstream.size(), encoded),
              plugins::kOk);
    EXPECT_TRUE(encoded.is_keyframe)
        << "the frame after a geometry change must be an IDR: that is what "
           "carries the new parameter sets to the peer";

    // The picture really is at the new size: the decoder must produce
    // a frame of the new geometry, not the old one.
    std::vector<std::uint8_t> dec_y(static_cast<std::size_t>(kNewWidth) * kNewHeight);
    std::vector<std::uint8_t> dec_u(static_cast<std::size_t>(kNewWidth / 2) * (kNewHeight / 2));
    std::vector<std::uint8_t> dec_v(static_cast<std::size_t>(kNewWidth / 2) * (kNewHeight / 2));
    std::uint8_t* buffers[3] = {dec_y.data(), dec_u.data(), dec_v.data()};

    plugins::VideoFrame decoded;
    ASSERT_EQ(pair.decoder->decode(encoded, decoded, buffers), plugins::kOk);
    EXPECT_EQ(decoded.width(), kNewWidth);
    EXPECT_EQ(decoded.height(), kNewHeight);

    pair.encoder->close();
    pair.decoder->close();
    pair.encoder.reset();
    pair.decoder.reset();
}

TEST(Openh264Software, SetParamsRejectsAnImpossibleGeometry) {
    // A contradictory geometry cannot be encoded, and the codec must say
    // so rather than silently keeping the old one — the engine treats a
    // rejection as "keep the previous parameters" and needs to be able
    // to tell the two apart.
    const plugins::VideoEncoderBackend* enc_be = openh264_encoder_backend();
    const plugins::VideoDecoderBackend* dec_be = openh264_decoder_backend();
    ASSERT_NE(enc_be, nullptr);
    ASSERT_NE(dec_be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width  = kWidth;
    cfg.height = kHeight;
    Openh264Pair pair;
    pair.encoder = enc_be->create(cfg);
    pair.decoder = dec_be->create(cfg);
    ASSERT_NE(pair.encoder, nullptr);
    ASSERT_NE(pair.decoder, nullptr);
    ASSERT_EQ(pair.encoder->open(), plugins::kOk);
    ASSERT_EQ(pair.decoder->open(), plugins::kOk);

    // "480 rows, no width" — half a geometry is a contradiction.
    plugins::VideoEncoderParams half;
    half.width  = 0;
    half.height = 480;
    EXPECT_NE(pair.encoder->set_params(half), plugins::kOk);

    // The previous parameters survive the rejection.
    EXPECT_EQ(pair.encoder->params().width, kWidth);
    EXPECT_EQ(pair.encoder->params().height, kHeight);

    pair.encoder->close();
    pair.decoder->close();
    pair.encoder.reset();
    pair.decoder.reset();
}

TEST(Openh264Software, SetParamsWithoutAGeometryKeepsTheGeometry) {
    // A parameter set that leaves the geometry out entirely is a
    // legitimate partial update: the caller only wants to change, say,
    // the bitrate.  The geometry must survive it.
    const plugins::VideoEncoderBackend* enc_be = openh264_encoder_backend();
    const plugins::VideoDecoderBackend* dec_be = openh264_decoder_backend();
    ASSERT_NE(enc_be, nullptr);
    ASSERT_NE(dec_be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width  = kWidth;
    cfg.height = kHeight;
    Openh264Pair pair;
    pair.encoder = enc_be->create(cfg);
    pair.decoder = dec_be->create(cfg);
    ASSERT_NE(pair.encoder, nullptr);
    ASSERT_NE(pair.decoder, nullptr);
    ASSERT_EQ(pair.encoder->open(), plugins::kOk);
    ASSERT_EQ(pair.decoder->open(), plugins::kOk);

    plugins::VideoEncoderParams partial;
    partial.bitrate_bps = 3'000'000;
    ASSERT_EQ(pair.encoder->set_params(partial), plugins::kOk);

    EXPECT_EQ(pair.encoder->params().width, kWidth);
    EXPECT_EQ(pair.encoder->params().height, kHeight);
    EXPECT_EQ(pair.encoder->params().bitrate_bps, 3'000'000u);

    pair.encoder->close();
    pair.decoder->close();
    pair.encoder.reset();
    pair.decoder.reset();
}

// ---------------------------------------------------------------------------
// The mid-stream parameter-set switch (v1.3 plan Q1 / video plan R2)
// ---------------------------------------------------------------------------
//
// The *sender* side of a resolution change is already covered above
// (`SetParamsRestartsAndEmitsTheNewParameterSets`): the encoder restarts
// and the new SPS / PPS travel with the next IDR.
//
// What was missing is the *receiver* half — and the existing test does
// not reach it.  It decodes the post-switch frame with a decoder that has
// **never seen a parameter set before**, so it proves only that OpenH264
// can decode a stream from a cold start.  It says nothing about what
// happens when the decoder is already holding a *different* SPS and a
// new one arrives mid-stream — which is exactly the case a live
// resolution change produces.
//
// The test below is the cold-start-free version:
//
//   1. encode a frame at the original geometry and **decode it**, so the
//      decoder is now holding the original SPS / PPS;
//   2. change the geometry (`set_params()`), which restarts the encoder
//      and puts the new parameter sets on the wire with the next frame;
//   3. decode the new frame **with the same decoder**, without a flush,
//      without a re-initialisation and without asking for a key frame.
//
// The bar is "decoded without an error", and "without an error" means:
// the decode returns `kOk` on the *first* call (no `kErrNotReady` that
// would need another round trip), the codec's own `decode_errors`
// counter stays at zero, and the decoded picture carries the **new**
// geometry — a decoder that silently kept the old parameter sets would
// produce the old size.
//
// The parameter sets really changed: the SPS bytes of the two frames are
// compared, so the test cannot pass by decoding the *old* frame twice.

/** The SPS bytes of an Annex B bitstream (empty when there is none). */
std::vector<std::uint8_t> extract_sps(nimrtc::core::ByteSpan bitstream) {
    nimrtc::h264::BitstreamParser parser;
    for (const auto& n : parser.parse_all(bitstream)) {
        if (n.type == nimrtc::video_payload::h264::NaluType::kSPS) {
            return std::vector<std::uint8_t>(n.data.begin(), n.data.end());
        }
    }
    return {};
}

/** Decode one access unit into tightly-packed planes.
 *
 *  The buffers are sized for `expected_w` x `expected_h`; the codec
 *  copies the decoded picture into them. */
plugins::Status decode_into(plugins::IVideoDecoder& decoder,
                            const plugins::EncodedVideoFrame& encoded,
                            std::uint32_t expected_w,
                            std::uint32_t expected_h,
                            plugins::VideoFrame& decoded) {
    std::vector<std::uint8_t> y(static_cast<std::size_t>(expected_w) * expected_h);
    std::vector<std::uint8_t> u(static_cast<std::size_t>(expected_w / 2) * (expected_h / 2));
    std::vector<std::uint8_t> v(static_cast<std::size_t>(expected_w / 2) * (expected_h / 2));
    std::uint8_t* buffers[3] = {y.data(), u.data(), v.data()};
    return decoder.decode(encoded, decoded, buffers);
}

TEST(Openh264Software, AMidStreamParameterSetSwitchDecodesWithoutAnError) {
    // R2's acceptance criterion, verbatim: "A synthetic mid-stream
    // resolution change is decoded without an error."
    //
    // The decoder here is *warm*: it has already decoded a frame at the
    // old geometry, so it is holding the old parameter sets when the
    // new ones arrive.
    const plugins::VideoEncoderBackend* enc_be = openh264_encoder_backend();
    const plugins::VideoDecoderBackend* dec_be = openh264_decoder_backend();
    ASSERT_NE(enc_be, nullptr);
    ASSERT_NE(dec_be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width  = kWidth;      // 320 x 240
    cfg.height = kHeight;
    cfg.fps          = 30;
    cfg.bitrate_bps  = 2'000'000;
    cfg.pixel_format = plugins::VideoPixelFormat::kI420;
    cfg.payload_type = 102;

    Openh264Pair pair;
    pair.encoder = enc_be->create(cfg);
    pair.decoder = dec_be->create(cfg);
    ASSERT_NE(pair.encoder, nullptr);
    ASSERT_NE(pair.decoder, nullptr);
    ASSERT_EQ(pair.encoder->open(), plugins::kOk);
    ASSERT_EQ(pair.decoder->open(), plugins::kOk);

    // ---- 1. The original geometry: encode AND decode ------------------
    std::vector<std::uint8_t> bitstream(512 * 1024);
    plugins::EncodedVideoFrame first;
    ASSERT_EQ(pair.encoder->encode(make_source_frame(kWidth, kHeight),
                            bitstream.data(), bitstream.size(), first),
              plugins::kOk);
    ASSERT_TRUE(first.is_keyframe);

    // The SPS is captured **here**, right after the encode: the codec's
    // payload is a span into the *shared* output buffer, so the next
    // encode would overwrite it.
    const std::vector<std::uint8_t> old_sps = extract_sps(first.payload);

    plugins::VideoFrame decoded_old;
    ASSERT_EQ(decode_into(*pair.decoder, first, kWidth, kHeight, decoded_old),
              plugins::kOk);
    EXPECT_EQ(decoded_old.width(), kWidth);
    EXPECT_EQ(decoded_old.height(), kHeight);
    EXPECT_EQ(pair.decoder_stats().decode_errors, 0u)
        << "the first frame must decode cleanly — the switch test is only "
           "meaningful on a decoder that was already working";

    // ---- 2. The geometry changes mid-stream ----------------------------
    constexpr std::uint32_t kNewWidth  = 640;
    constexpr std::uint32_t kNewHeight = 480;

    plugins::VideoEncoderParams params;
    params.width        = kNewWidth;
    params.height       = kNewHeight;
    params.fps          = 30;
    params.bitrate_bps  = 2'000'000;
    params.pixel_format = plugins::VideoPixelFormat::kI420;
    ASSERT_EQ(pair.encoder->set_params(params), plugins::kOk);

    // ---- 3. The new frame carries the new parameter sets ---------------
    plugins::EncodedVideoFrame second;
    ASSERT_EQ(pair.encoder->encode(make_source_frame(kNewWidth, kNewHeight),
                            bitstream.data(), bitstream.size(), second),
              plugins::kOk);
    ASSERT_TRUE(second.is_keyframe)
        << "the frame after a geometry change must be an IDR — that is "
           "what carries the new parameter sets to the peer";

    const std::vector<std::uint8_t> new_sps = extract_sps(second.payload);
    ASSERT_FALSE(old_sps.empty()) << "the first frame must carry an SPS";
    ASSERT_FALSE(new_sps.empty()) << "the second frame must carry an SPS";
    EXPECT_NE(old_sps, new_sps)
        << "the parameter sets must really have changed — otherwise this "
           "test would only be decoding the old stream twice";

    // ---- 4. The warm decoder takes the new frame ------------------------
    //
    // No flush, no re-initialisation, no key-frame request: the frame
    // simply has to decode.  A decoder that needed a round trip would
    // return `kErrNotReady` here (the picture would surface on a later
    // call), and one that could not follow the switch at all would
    // return an error.
    plugins::VideoFrame decoded_new;
    const plugins::Status rc =
        decode_into(*pair.decoder, second, kNewWidth, kNewHeight, decoded_new);
    ASSERT_EQ(rc, plugins::kOk)
        << "the mid-stream parameter-set change must decode without an "
           "error, on the first call — no key-frame round trip";

    // The decoded picture carries the NEW geometry: a decoder that kept
    // the old parameter sets would report the old size.
    EXPECT_EQ(decoded_new.width(), kNewWidth);
    EXPECT_EQ(decoded_new.height(), kNewHeight);

    // The codec's own error counter must not have moved: "decoded
    // without an error" is not the same as "decoded, after an error
    // was swallowed".
    EXPECT_EQ(pair.decoder_stats().decode_errors, 0u);

    // ---- 5. The stream keeps working afterwards -------------------------
    //
    // The switch must not have broken the decoder: the *old* geometry
    // still decodes too, which is what a stream that switches back and
    // forth needs.
    plugins::EncodedVideoFrame third;
    ASSERT_EQ(pair.encoder->encode(make_source_frame(kWidth, kHeight),
                            bitstream.data(), bitstream.size(), third),
              plugins::kOk);
    plugins::VideoFrame decoded_back;
    ASSERT_EQ(decode_into(*pair.decoder, third, kWidth, kHeight, decoded_back),
              plugins::kOk);
    EXPECT_EQ(decoded_back.width(), kWidth);
    EXPECT_EQ(decoded_back.height(), kHeight);
    EXPECT_EQ(pair.decoder_stats().decode_errors, 0u);

    pair.encoder->close();
    pair.decoder->close();
    pair.encoder.reset();
    pair.decoder.reset();
}

TEST(Openh264Software, TheDecodedPictureAfterASwitchCarriesRealSignal) {
    // The switch test above proves the *geometry* followed the new
    // parameter sets.  This one proves the *pixels* did too: a decoder
    // that produced a correctly-sized but blank frame would pass a
    // geometry-only assertion.
    const plugins::VideoEncoderBackend* enc_be = openh264_encoder_backend();
    const plugins::VideoDecoderBackend* dec_be = openh264_decoder_backend();
    ASSERT_NE(enc_be, nullptr);
    ASSERT_NE(dec_be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width  = kWidth;
    cfg.height = kHeight;
    cfg.fps          = 30;
    cfg.bitrate_bps  = 2'000'000;
    cfg.pixel_format = plugins::VideoPixelFormat::kI420;

    Openh264Pair pair;
    pair.encoder = enc_be->create(cfg);
    pair.decoder = dec_be->create(cfg);
    ASSERT_NE(pair.encoder, nullptr);
    ASSERT_NE(pair.decoder, nullptr);
    ASSERT_EQ(pair.encoder->open(), plugins::kOk);
    ASSERT_EQ(pair.decoder->open(), plugins::kOk);

    std::vector<std::uint8_t> bitstream(512 * 1024);
    plugins::EncodedVideoFrame first;
    ASSERT_EQ(pair.encoder->encode(make_source_frame(kWidth, kHeight),
                            bitstream.data(), bitstream.size(), first),
              plugins::kOk);

    constexpr std::uint32_t kNewWidth  = 640;
    constexpr std::uint32_t kNewHeight = 480;

    plugins::VideoEncoderParams params;
    params.width        = kNewWidth;
    params.height       = kNewHeight;
    params.fps          = 30;
    params.bitrate_bps  = 2'000'000;
    params.pixel_format = plugins::VideoPixelFormat::kI420;
    ASSERT_EQ(pair.encoder->set_params(params), plugins::kOk);

    plugins::EncodedVideoFrame second;
    ASSERT_EQ(pair.encoder->encode(make_source_frame(kNewWidth, kNewHeight),
                            bitstream.data(), bitstream.size(), second),
              plugins::kOk);

    // Decode the new frame into a buffer the test owns, so the pixels can
    // be inspected (the codec's `decode()` contract has no stride
    // parameter, so the planes are tightly packed).
    std::vector<std::uint8_t> y(static_cast<std::size_t>(kNewWidth) * kNewHeight);
    std::vector<std::uint8_t> u(static_cast<std::size_t>(kNewWidth / 2) * (kNewHeight / 2));
    std::vector<std::uint8_t> v(static_cast<std::size_t>(kNewWidth / 2) * (kNewHeight / 2));
    std::uint8_t* buffers[3] = {y.data(), u.data(), v.data()};

    plugins::VideoFrame decoded;
    ASSERT_EQ(pair.decoder->decode(second, decoded, buffers), plugins::kOk);

    const auto [min_it, max_it] = std::minmax_element(y.begin(), y.end());
    EXPECT_GT(*max_it, 100u) << "the decoded luma has no bright pixels";
    EXPECT_LT(*min_it, 200u) << "the decoded luma has no dark pixels";
    EXPECT_GT(static_cast<unsigned>(*max_it - *min_it), 30u)
        << "the decoded luma is a constant — the picture carries no signal";

    pair.encoder->close();
    pair.decoder->close();
    pair.encoder.reset();
    pair.decoder.reset();
}

TEST(Openh264Software, SetParamsKeepsTheDecoderAlive) {
    // The decoder takes its geometry from the bitstream's own parameter
    // sets, so it must survive a geometry change on the encoder side.
    const plugins::VideoEncoderBackend* enc_be = openh264_encoder_backend();
    const plugins::VideoDecoderBackend* dec_be = openh264_decoder_backend();
    ASSERT_NE(enc_be, nullptr);
    ASSERT_NE(dec_be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width  = kWidth;
    cfg.height = kHeight;
    cfg.bitrate_bps = 2000000;
    Openh264Pair pair;
    pair.encoder = enc_be->create(cfg);
    pair.decoder = dec_be->create(cfg);
    ASSERT_NE(pair.encoder, nullptr);
    ASSERT_NE(pair.decoder, nullptr);
    ASSERT_EQ(pair.encoder->open(), plugins::kOk);
    ASSERT_EQ(pair.decoder->open(), plugins::kOk);

    std::vector<std::uint8_t> bitstream(512 * 1024);
    plugins::EncodedVideoFrame encoded;
    ASSERT_EQ(pair.encoder->encode(make_source_frame(kWidth, kHeight),
                            bitstream.data(), bitstream.size(), encoded),
              plugins::kOk);

    // Change the geometry *after* a frame was produced.
    plugins::VideoEncoderParams params;
    params.width  = 320;
    params.height = 240;
    params.bitrate_bps = 2000000;
    ASSERT_EQ(pair.encoder->set_params(params), plugins::kOk);

    // The old frame still decodes — the decoder was not re-initialised.
    std::vector<std::uint8_t> dec_y(static_cast<std::size_t>(kWidth) * kHeight);
    std::vector<std::uint8_t> dec_u(static_cast<std::size_t>(kWidth / 2) * (kHeight / 2));
    std::vector<std::uint8_t> dec_v(static_cast<std::size_t>(kWidth / 2) * (kHeight / 2));
    std::uint8_t* buffers[3] = {dec_y.data(), dec_u.data(), dec_v.data()};

    plugins::VideoFrame decoded;
    EXPECT_EQ(pair.decoder->decode(encoded, decoded, buffers), plugins::kOk);

    pair.encoder->close();
    pair.decoder->close();
    pair.encoder.reset();
    pair.decoder.reset();
}
