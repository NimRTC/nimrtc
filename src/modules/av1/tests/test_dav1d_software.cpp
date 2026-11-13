/**
 * @file src/modules/av1/tests/test_dav1d_software.cpp
 * @brief dav1d software AV1 decoder tests (remote-desktop plan WP-B).
 *
 * The vendored dav1d (BSD-2-Clause, VideoLAN) is the *real* software
 * AV1 decoder NimRTC ships as the CPU bring-up baseline.  These tests:
 *
 *   1. The backend table exposes a `dav1d_av1` decoder entry with
 *      priority above the synthetic stub, and the created codec reports
 *      `backend_id() == "dav1d"` (the stub reports "software-stub").
 *   2. A real AV1 key frame decodes to *non-zero pixels* 鈥?the stub
 *      cannot do that (it paints a synthetic pattern), so this is the
 *      acceptance criterion for the AV1 leg of WP-B.
 *   3. The decoded picture has the geometry the sequence header
 *      announced, and the metadata survives the decode.
 *
 * The test bitstream is a 64x64 AV1 key frame (one temporal delimiter +
 * one sequence header + one frame OBU) in low-overhead OBU form,
 * embedded below so the test needs no external fixture.
 *
 * The whole file is compiled only when `NIMRTC_PLUGINS_DAV1D_ON` is
 * defined 鈥?i.e. when the vendored source tree is present and the option
 * is on.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/av1/codec_plugin.hpp>
#include <nimrtc/av1/hw_backends.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/video_frame/frame.hpp>

namespace plugins = nimrtc::plugins;
namespace vf      = nimrtc::video_frame;

namespace {

// ---------------------------------------------------------------------------
// A 64x64 AV1 key frame, low-overhead OBU stream.
//
//   0x12  -> temporal delimiter (type 2, has_size = 1), size 0
//   0x0A  -> sequence header  (type 1, has_size = 1), size 9
//   0x32  -> frame OBU         (type 6, has_size = 1), size 0xCA = 202
//
// Produced with `ffmpeg -f lavfi -i testsrc=size=64x64 -c:v libaom-av1
// -still-picture 1 -f obu`.
// ---------------------------------------------------------------------------

constexpr std::uint8_t kAv1KeyFrame[] = {
    0x12, 0x00, 0x0A, 0x09, 0x38, 0x15, 0x7F, 0xFD, 0xA4, 0x04, 0x34, 0x00,
    0x80, 0x32, 0xCA, 0x02, 0x46, 0x80, 0x02, 0x08, 0x20, 0xC1, 0x40, 0x41,
    0x40, 0xDC, 0x85, 0x26, 0x68, 0xF4, 0x39, 0xE4, 0xF3, 0x5F, 0x7D, 0xB5,
    0xBF, 0xDB, 0x4E, 0x77, 0xF3, 0x7C, 0x67, 0xDF, 0xB9, 0x09, 0x70, 0x47,
    0xFF, 0xF9, 0xBB, 0x0F, 0x90, 0x35, 0xB6, 0x13, 0xC3, 0xB2, 0xDC, 0x56,
    0x1F, 0x24, 0xEF, 0xA9, 0x33, 0x0C, 0xC2, 0x38, 0xF1, 0x2B, 0x17, 0x98,
    0x0C, 0x20, 0x4B, 0x6F, 0x66, 0xE5, 0x4A, 0x64, 0x79, 0x55, 0xA6, 0xFB,
    0x2C, 0x43, 0x11, 0x1B, 0x4B, 0x23, 0x17, 0xD6, 0xF5, 0xAD, 0xD6, 0x42,
    0x8F, 0x2A, 0x83, 0x79, 0x77, 0x0C, 0x7F, 0xE5, 0xF5, 0xBE, 0x4C, 0x16,
    0xF2, 0x11, 0x7F, 0x90, 0xB6, 0x9E, 0xEB, 0xFB, 0xD4, 0x79, 0xBE, 0xE7,
    0xD9, 0x3C, 0x16, 0x7C, 0xA6, 0xC5, 0xA3, 0xB4, 0x2D, 0x92, 0xCD, 0x5C,
    0xB2, 0x21, 0xEC, 0x7F, 0xB7, 0xC7, 0x82, 0x36, 0x74, 0x86, 0xC9, 0x55,
    0x30, 0xAF, 0xFD, 0xEB, 0x72, 0x47, 0xA4, 0x9D, 0x55, 0x56, 0x20, 0x60,
    0x93, 0x74, 0x29, 0x11, 0xAC, 0x8C, 0x1C, 0x00, 0x52, 0x06, 0x56, 0x87,
    0x92, 0x45, 0xDE, 0xD2, 0xC3, 0x25, 0x90, 0xB6, 0x2A, 0x52, 0xF2, 0x71,
    0x3D, 0x48, 0x57, 0xAD, 0x47, 0xCA, 0x62, 0x2A, 0xEB, 0xA3, 0x80, 0xBE,
    0x96, 0xB7, 0xC7, 0xCF, 0x8B, 0xF1, 0x9C, 0x8B, 0x7E, 0x7A, 0xF6, 0x69,
    0x7C, 0x3F, 0x23, 0x38, 0x69, 0x12, 0x63, 0x7F, 0x4F, 0x5A, 0x89, 0x6A,
    0x88, 0xB2, 0xB2, 0x60, 0x34, 0x0F, 0x8A, 0x09, 0xD5, 0x2C, 0x5A, 0x0C,
    0xDC, 0x74, 0xF4, 0x30, 0x07, 0x46, 0x99, 0x27, 0xB4, 0xFA, 0x0D, 0xE6,
    0xA7, 0x6B, 0x27, 0xF2, 0x62, 0x7B, 0x19, 0xA5, 0xF8, 0xEC, 0xFE, 0xC7,
    0x06, 0x11, 0x03, 0xAC, 0xA1, 0x6C, 0x94, 0x6F, 0x87, 0xB9, 0x40, 0xA1,
    0xEE, 0x6D, 0xED, 0xE5, 0x41, 0x84, 0xB0, 0x9B, 0xF5, 0xD2, 0x2F, 0x27,
    0x06, 0xFF, 0xB5, 0x65, 0x59, 0x4D, 0x31, 0x30, 0x10, 0x50, 0x32, 0xA0,
    0x9F, 0xE7, 0x9D, 0x85, 0x68, 0x8C, 0x06, 0x0B, 0xDE, 0x74, 0x5E, 0xD2,
    0xDC, 0x89, 0xE0, 0xF0, 0x32, 0x1A, 0x46, 0x7F, 0x90, 0x97, 0xE6, 0x66,
    0xDB, 0xB6, 0xF2, 0x74, 0x96, 0x17, 0xCD, 0xCD, 0x7F, 0x50, 0x4F, 0xCE,
    0xEE, 0x1D, 0x01, 0x21, 0xC1, 0x44, 0x24, 0xE6, 0x49, 0x81, 0x75, 0x07,
    0xC3, 0x6D, 0x7D, 0x59, 0x71, 0x03, 0xA7, 0x55, 0xBE, 0x80,
};

constexpr std::uint32_t kWidth  = 64;
constexpr std::uint32_t kHeight = 64;

/** The `dav1d_av1` decoder backend, or nullptr when the vendored dav1d
 *  is not linked into this build. */
const plugins::VideoDecoderBackend* dav1d_backend() {
    plugins::VideoCodecConfig cfg;
    cfg.codec        = plugins::VideoCodecKind::kAV1;
    cfg.width        = kWidth;
    cfg.height       = kHeight;
    cfg.pixel_format = plugins::VideoPixelFormat::kI420;
    return nimrtc::av1::select_decoder_backend(cfg);
}

/** Decode until a picture comes out.
 *
 *  dav1d may buffer a frame (it answers `kErrNotReady` when no picture is
 *  ready yet), so the caller feeds the same access unit until the
 *  decoder produces one.  Returns kOk on the first picture, or the last
 *  error the decoder reported. */
plugins::Status decode_with_retry(plugins::IVideoCodec* codec,
                                  const plugins::EncodedVideoFrame& encoded,
                                  plugins::VideoFrame& out,
                                  std::uint8_t* const* buffers) {
    plugins::Status last = plugins::kErrNotReady;
    for (int attempt = 0; attempt < 8; ++attempt) {
        last = codec->decode(encoded, out, buffers);
        if (last != plugins::kErrNotReady) return last;
    }
    return last;
}

} // namespace

// ---------------------------------------------------------------------------
// Backend table
// ---------------------------------------------------------------------------

TEST(Dav1dSoftware, BackendIsRegistered) {
    nimrtc::av1::register_default_video_backends();

    const plugins::VideoDecoderBackend* be = dav1d_backend();
    ASSERT_NE(be, nullptr);
    EXPECT_EQ(be->id, "dav1d_av1");
    EXPECT_FALSE(be->is_hw);
    EXPECT_TRUE(be->available(plugins::VideoCodecConfig{}));
}

TEST(Dav1dSoftware, BackendIdDistinguishesRealCodecFromStub) {
    nimrtc::av1::register_default_video_backends();

    const plugins::VideoDecoderBackend* be = dav1d_backend();
    ASSERT_NE(be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width  = kWidth;
    cfg.height = kHeight;
    auto codec = be->create(cfg);
    ASSERT_NE(codec, nullptr);

    EXPECT_EQ(codec->backend_id(), "dav1d");
    EXPECT_NE(codec->backend_id(), "software-stub");
    EXPECT_FALSE(codec->is_hardware_accelerated());

    codec.reset();
}

TEST(Dav1dSoftware, StubStillReportsSoftwareStub) {
    nimrtc::av1::CodecPluginAdapter stub({});
    EXPECT_EQ(stub.backend_id(), "software-stub");
}

// ---------------------------------------------------------------------------
// Decode
// ---------------------------------------------------------------------------

TEST(Dav1dSoftware, DecodesKeyFrameToNonZeroPixels) {
    nimrtc::av1::register_default_video_backends();

    const plugins::VideoDecoderBackend* be = dav1d_backend();
    ASSERT_NE(be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width  = kWidth;
    cfg.height = kHeight;
    cfg.pixel_format = plugins::VideoPixelFormat::kI420;

    auto codec = be->create(cfg);
    ASSERT_NE(codec, nullptr);
    ASSERT_EQ(codec->open(), plugins::kOk);
    ASSERT_TRUE(codec->can_decode());
    // dav1d is decode-only.
    EXPECT_FALSE(codec->can_encode());

    plugins::EncodedVideoFrame encoded;
    encoded.codec       = plugins::VideoCodecKind::kAV1;
    encoded.payload     = nimrtc::core::ByteSpan{kAv1KeyFrame, sizeof(kAv1KeyFrame)};
    encoded.is_keyframe = true;
    encoded.info.frame_seq     = 7;
    encoded.info.capture_ts_us = 555;
    encoded.info.rtp_timestamp = 0x99887766;

    // The decode contract has no stride parameter, so the caller's
    // planes are tightly packed (stride == width).
    std::vector<std::uint8_t> dec_y(static_cast<std::size_t>(kWidth) * kHeight);
    std::vector<std::uint8_t> dec_u(static_cast<std::size_t>(kWidth / 2) * (kHeight / 2));
    std::vector<std::uint8_t> dec_v(static_cast<std::size_t>(kWidth / 2) * (kHeight / 2));
    std::uint8_t* buffers[3] = {dec_y.data(), dec_u.data(), dec_v.data()};

    plugins::VideoFrame decoded;
    ASSERT_EQ(decode_with_retry(codec.get(), encoded, decoded, buffers), plugins::kOk);

    // The geometry comes from the sequence header, not the config.
    EXPECT_EQ(decoded.width(),  kWidth);
    EXPECT_EQ(decoded.height(), kHeight);
    EXPECT_EQ(decoded.info.frame_seq,     7u);
    EXPECT_EQ(decoded.info.capture_ts_us, 555);
    EXPECT_EQ(decoded.info.rtp_timestamp, 0x99887766u);

    // ---- The acceptance criterion: real, non-zero pixels ------------------
    std::size_t non_zero = 0;
    for (std::uint8_t p : dec_y) {
        if (p != 0) ++non_zero;
    }
    EXPECT_GT(non_zero, dec_y.size() / 2)
        << "the decoded luma plane is (mostly) zero 鈥?the AV1 decoder "
           "did not reconstruct a real picture";

    const auto [min_it, max_it] = std::minmax_element(dec_y.begin(), dec_y.end());
    EXPECT_GT(*max_it, 0u) << "decoded luma plane is entirely zero";
    // The source was a `testsrc` pattern: it carries real contrast, so
    // the decoded luma must span a meaningful range (not a constant
    // grey, which would mean the decoder produced nothing but a
    // mid-level fill).
    EXPECT_GT(*max_it - *min_it, 20u)
        << "decoded luma plane is constant 鈥?the decoder produced no "
           "reconstructed picture";

    // ---- Stats ------------------------------------------------------------
    const plugins::VideoCodecStats s = codec->stats();
    EXPECT_EQ(s.frames_decoded, 1u);
    EXPECT_GT(s.bytes_decoded, 0u);
    EXPECT_EQ(s.decode_errors, 0u);

    codec->close();
    codec.reset();
}

TEST(Dav1dSoftware, DecodesAnnexBStreamWithStartCodes) {
    // The engine hands the decoder Annex-B framing (`00 00 00 01`
    // before each OBU — see `video_payload::av1::parse_annex_b_obus`),
    // while dav1d expects a low-overhead OBU stream.  The decoder must
    // strip the start codes, so the same key frame wrapped in Annex-B
    // must decode to the same non-zero picture.
    nimrtc::av1::register_default_video_backends();

    const plugins::VideoDecoderBackend* be = dav1d_backend();
    ASSERT_NE(be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width  = kWidth;
    cfg.height = kHeight;
    cfg.pixel_format = plugins::VideoPixelFormat::kI420;

    auto codec = be->create(cfg);
    ASSERT_NE(codec, nullptr);
    ASSERT_EQ(codec->open(), plugins::kOk);

    // Split the low-overhead fixture into its three OBUs: TD (2 bytes),
    // sequence header (2 + 9 bytes), frame (the rest).
    constexpr std::size_t kTdSize = 2;
    constexpr std::size_t kSeqSize = 2 + 9;
    ASSERT_GT(sizeof(kAv1KeyFrame), kTdSize + kSeqSize);
    constexpr std::uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};
    std::vector<std::uint8_t> annex_b;
    annex_b.reserve(sizeof(kAv1KeyFrame) + 3 * sizeof(kStartCode));
    auto append_obu = [&](const std::uint8_t* data, std::size_t size) {
        annex_b.insert(annex_b.end(), std::begin(kStartCode), std::end(kStartCode));
        annex_b.insert(annex_b.end(), data, data + size);
    };
    append_obu(kAv1KeyFrame, kTdSize);
    append_obu(kAv1KeyFrame + kTdSize, kSeqSize);
    append_obu(kAv1KeyFrame + kTdSize + kSeqSize,
               sizeof(kAv1KeyFrame) - kTdSize - kSeqSize);

    plugins::EncodedVideoFrame encoded;
    encoded.codec       = plugins::VideoCodecKind::kAV1;
    encoded.payload     = nimrtc::core::ByteSpan{annex_b.data(), annex_b.size()};
    encoded.is_keyframe = true;
    encoded.info.frame_seq = 8;

    std::vector<std::uint8_t> dec_y(static_cast<std::size_t>(kWidth) * kHeight);
    std::vector<std::uint8_t> dec_u(static_cast<std::size_t>(kWidth / 2) * (kHeight / 2));
    std::vector<std::uint8_t> dec_v(static_cast<std::size_t>(kWidth / 2) * (kHeight / 2));
    std::uint8_t* buffers[3] = {dec_y.data(), dec_u.data(), dec_v.data()};

    plugins::VideoFrame decoded;
    ASSERT_EQ(decode_with_retry(codec.get(), encoded, decoded, buffers), plugins::kOk);
    EXPECT_EQ(decoded.width(), kWidth);
    EXPECT_EQ(decoded.height(), kHeight);

    std::size_t non_zero = 0;
    for (std::uint8_t p : dec_y) {
        if (p != 0) ++non_zero;
    }
    EXPECT_GT(non_zero, dec_y.size() / 2)
        << "Annex-B input did not decode to a real picture — "
           "the start-code stripping is broken";

    codec->close();
    codec.reset();
}

TEST(Dav1dSoftware, DecodeIntoOwnedBufferProducesSamePicture) {
    // The decode contract has two shapes: caller-supplied planes, and a
    // codec-owned buffer.  Both must yield the same pixels.
    nimrtc::av1::register_default_video_backends();

    const plugins::VideoDecoderBackend* be = dav1d_backend();
    ASSERT_NE(be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width  = kWidth;
    cfg.height = kHeight;

    auto codec = be->create(cfg);
    ASSERT_NE(codec, nullptr);
    ASSERT_EQ(codec->open(), plugins::kOk);

    plugins::EncodedVideoFrame encoded;
    encoded.codec       = plugins::VideoCodecKind::kAV1;
    encoded.payload     = nimrtc::core::ByteSpan{kAv1KeyFrame, sizeof(kAv1KeyFrame)};
    encoded.is_keyframe = true;

    // (a) caller-supplied planes
    std::vector<std::uint8_t> dec_y(static_cast<std::size_t>(kWidth) * kHeight);
    std::vector<std::uint8_t> dec_u(static_cast<std::size_t>(kWidth / 2) * (kHeight / 2));
    std::vector<std::uint8_t> dec_v(static_cast<std::size_t>(kWidth / 2) * (kHeight / 2));
    std::uint8_t* buffers[3] = {dec_y.data(), dec_u.data(), dec_v.data()};
    plugins::VideoFrame out_a;
    ASSERT_EQ(decode_with_retry(codec.get(), encoded, out_a, buffers), plugins::kOk);

    // (b) codec-owned buffer
    plugins::VideoFrame out_b;
    ASSERT_EQ(decode_with_retry(codec.get(), encoded, out_b, nullptr), plugins::kOk);

    auto owned = out_b.cpu_buffer();
    ASSERT_NE(owned, nullptr);
    ASSERT_NE(owned->plane(0), nullptr);
    EXPECT_EQ(owned->width(),  kWidth);
    EXPECT_EQ(owned->height(), kHeight);

    const std::uint32_t stride = owned->layout().strides[0];
    for (std::uint32_t row = 0; row < kHeight; ++row) {
        const std::uint8_t* src = owned->plane(0) + static_cast<std::size_t>(row) * stride;
        const std::uint8_t* ref = dec_y.data() + static_cast<std::size_t>(row) * kWidth;
        EXPECT_EQ(std::memcmp(src, ref, kWidth), 0) << "row " << row << " differs";
    }

    codec->close();
    codec.reset();
}

TEST(Dav1dSoftware, DecodeRejectsEmptyPayload) {
    nimrtc::av1::register_default_video_backends();

    const plugins::VideoDecoderBackend* be = dav1d_backend();
    ASSERT_NE(be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width  = kWidth;
    cfg.height = kHeight;
    auto codec = be->create(cfg);
    ASSERT_NE(codec, nullptr);
    ASSERT_EQ(codec->open(), plugins::kOk);

    plugins::EncodedVideoFrame encoded;
    plugins::VideoFrame decoded;
    std::uint8_t dummy[1] = {0};
    std::uint8_t* buffers[3] = {dummy, dummy, dummy};
    EXPECT_EQ(codec->decode(encoded, decoded, buffers), plugins::kErrInvalidParam);

    codec->close();
    codec.reset();
}

// ---------------------------------------------------------------------------
// Factory wiring
// ---------------------------------------------------------------------------

TEST(Dav1dSoftware, TheFactoryPairsTheStubEncoderWithTheDav1dDecoder) {
    // The engine holds ONE codec per session and drives both directions
    // through it.  The AV1 stub is encode-only (it cannot decode AV1),
    // so the factory has to compose it with the real decoder — otherwise
    // dav1d would sit in the registry and never be reached, no matter
    // how high its priority is.
    nimrtc::av1::register_default_video_backends();

    plugins::VideoCodecConfig cfg;
    cfg.codec        = plugins::VideoCodecKind::kAV1;
    cfg.width        = kWidth;
    cfg.height       = kHeight;
    cfg.pixel_format = plugins::VideoPixelFormat::kI420;

    nimrtc::av1::Av1PluginFactory factory;
    std::unique_ptr<plugins::IVideoCodec> codec(factory.create(cfg));
    ASSERT_NE(codec, nullptr);
    ASSERT_EQ(codec->open(), plugins::kOk);

    EXPECT_TRUE(codec->can_encode()) << "the stub encoder must still encode";
    EXPECT_TRUE(codec->can_decode()) << "the real decoder must be reachable";

    // The composite names the *encoder* — that is the half the engine's
    // codec telemetry describes — and the AV1 encoder is still the
    // synthetic stub, so the id says so.
    EXPECT_EQ(codec->backend_id(), "software-stub");

    codec->close();
    codec.reset();
}
