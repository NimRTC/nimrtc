/**
 * @file src/modules/h264/tests/test_dxva_hardware.cpp
 * @brief The Microsoft DXVA / Media Foundation H.264 decoder tests.
 *
 * The DXVA backend is the *hardware* decode path on a Windows host whose
 * NVIDIA driver cannot decode (an old driver, a non-default adapter, a
 * GPU with no NVDEC unit).  It was the missing link in the decode
 * fallback chain:
 *
 *   430  nvdec_h264     NVIDIA
 *   320  dxva_h264      Microsoft  <- this one
 *    50  openh264_h264  software
 *     0  stub_h264      synthetic
 *
 * The tests below assert the three things that matter:
 *
 *   1. The backend row is *present* in the registry when the plugin is
 *      compiled in, with the right priority and the right identity.
 *   2. The decoder opens, reports itself as a decoder-only codec, and
 *      names its backend honestly.
 *   3. It actually **decodes**: a real H.264 bitstream (produced by the
 *      software OpenH264 encoder) comes back as a real picture whose
 *      pixels correlate with the source.
 *
 * The file is compiled only when `NIMRTC_PLUGINS_DXVA_ON` is defined —
 * i.e. on Windows with `NIMRTC_PLUGINS_DXVA=ON`.  On any other host the
 * row is simply absent from the registry and there is nothing to test.
 *
 * The decode cases are *skipped* (not failed) when the host cannot
 * decode H.264 through DXVA.  That is a real host class, not a
 * hypothetical one: a Windows Server 2022 hosted runner has the Media
 * Foundation H.264 decoder MFT — `CoCreateInstance` succeeds — but no
 * DXVA-capable device, so the MFT **rejects** the D3D manager with
 * `0x80004002` (E_NOINTERFACE).
 *
 * The "CPU fallback" is a trap, and it is worth spelling out because
 * the code used to believe in it: with no D3D manager the MFT does
 * *not* decode on the CPU.  It answers `ProcessOutput` with
 * `0x80070057` (E_INVALIDARG) and never produces a picture at all.  A
 * decoder that opens and then never yields a frame is the worst
 * possible failure — the engine has committed to it, the peer sends,
 * and the pictures never arrive.
 *
 * So the backend's availability probe asks the *real* question ("does
 * the MFT accept a D3D manager here?"), the decoder refuses to open
 * when the answer is no, and these tests skip on such a host.  A host
 * without the capability is not a bug; a host that *claims* the
 * capability and then produces no picture is.
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

namespace plugins = nimrtc::plugins;

namespace {

constexpr std::uint32_t kWidth  = 320;
constexpr std::uint32_t kHeight = 240;

// ---------------------------------------------------------------------------
// The backend row
// ---------------------------------------------------------------------------

/** The `dxva_h264` decoder backend, or nullptr when it is not registered.
 *
 *  The lookup is by *id* rather than through `select_decoder_backend()`
 *  so the test is deterministic on a host that also has an NVIDIA GPU:
 *  the NVDEC entry has a much higher priority and would win the generic
 *  selection. */
const plugins::VideoDecoderBackend* dxva_backend() {
    nimrtc::h264::register_default_video_backends();
    for (const auto* be : plugins::HwVideoBackendRegistry::instance()
                             .list_decoders(plugins::VideoCodecKind::kH264)) {
        if (be->id == "dxva_h264") return be;
    }
    return nullptr;
}

/** The `openh264_h264` encoder — the software H.264 producer used to
 *  generate the bitstream the DXVA decoder has to decode. */
const plugins::VideoEncoderBackend* openh264_backend() {
    nimrtc::h264::register_default_video_backends();
    for (const auto* be : plugins::HwVideoBackendRegistry::instance()
                             .list_encoders(plugins::VideoCodecKind::kH264)) {
        if (be->id == "openh264_h264") return be;
    }
    return nullptr;
}

/**
 * @brief Skip the calling test when this host cannot decode via DXVA.
 *
 * The backend's `available()` is the *capability* probe: it opens a real
 * session and asks the Media Foundation H.264 MFT whether it accepts a
 * D3D manager on this host.  When the answer is no, there is no DXVA
 * decode to test — and that is not a failure, it is a host without the
 * capability.
 *
 * The skip carries the backend's own explanation, so the test report
 * says *why* the case did not run rather than leaving a bare "skipped".
 *
 * This is a **macro**, not a function, and it has to be: `GTEST_SKIP()`
 * works by `return`ing from the *test body*, so a `GTEST_SKIP()` inside
 * a helper function only returns from the helper — the test then walks
 * on and fails on the assertion that follows.  (Measured: the helper
 * version reported "Skipped" *and* "FAILED" for the same case.)  The
 * macro keeps the `return` in the test body, where it belongs.
 */
#define SKIP_WITHOUT_DXVA(be, cfg)                                           \
    do {                                                                     \
        const plugins::VideoDecoderBackend* nimrtc_skip_be = (be);          \
        const plugins::VideoCodecConfig& nimrtc_skip_cfg = (cfg);           \
        if (!nimrtc_skip_be->available(nimrtc_skip_cfg)) {                   \
            const std::string nimrtc_skip_reason =                           \
                nimrtc_skip_be->unavailable_reason                            \
                    ? nimrtc_skip_be->unavailable_reason(nimrtc_skip_cfg)     \
                    : std::string{"the DXVA decode path is not available "    \
                                  "on this host"};                           \
            GTEST_SKIP() << "no usable DXVA decode path on this host — "     \
                          << nimrtc_skip_reason;                             \
        }                                                                    \
    } while (false)

// ---------------------------------------------------------------------------
// The synthetic source picture
// ---------------------------------------------------------------------------

/** Fill one I420 picture with a deterministic pattern.
 *
 *  A smooth ramp with a hard edge: the ramp gives the rate controller
 *  something to chew on, the edge gives the test a sharp feature to look
 *  for in the decoded output. */
void fill_pattern(std::uint8_t* y, std::uint8_t* u, std::uint8_t* v,
                  std::uint32_t width, std::uint32_t height,
                  std::uint32_t stride_y, std::uint32_t stride_c) {
    for (std::uint32_t row = 0; row < height; ++row) {
        for (std::uint32_t col = 0; col < width; ++col) {
            std::uint32_t value = 16 + (col * 200) / width;
            if ((row / 16) % 2 == 1) value = 235 - (row * 8) % 200;
            y[static_cast<std::size_t>(row) * stride_y + col] =
                static_cast<std::uint8_t>(value);
        }
    }
    const std::uint32_t cw = width / 2;
    const std::uint32_t ch = height / 2;
    for (std::uint32_t row = 0; row < ch; ++row) {
        for (std::uint32_t col = 0; col < cw; ++col) {
            u[static_cast<std::size_t>(row) * stride_c + col] = 128;
            v[static_cast<std::size_t>(row) * stride_c + col] = 128;
        }
    }
}

/** Build the source frame the encoder codes. */
plugins::VideoFrame make_source_frame(std::uint32_t width, std::uint32_t height) {
    const auto layout = nimrtc::video_frame::compute_layout(
        nimrtc::video_frame::PixelFormat::kI420, width, height);
    auto buf = std::make_shared<nimrtc::video_frame::VideoFrameBuffer>(layout);
    fill_pattern(buf->plane(0), buf->plane(1), buf->plane(2),
                 width, height,
                 static_cast<std::uint32_t>(layout.strides[0]),
                 static_cast<std::uint32_t>(layout.strides[1]));

    plugins::VideoFrame f;
    f.buffer = std::move(buf);
    f.info.frame_seq      = 7;
    f.info.capture_ts_us  = 987654;
    f.info.rtp_timestamp  = 0x55667788;
    return f;
}

/** The mean absolute difference between two byte vectors. */
double mean_abs_error(const std::vector<std::uint8_t>& a,
                      const std::vector<std::uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) return 1e9;
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        sum += std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
    }
    return sum / static_cast<double>(a.size());
}

/** The Pearson correlation of the two vectors' row means.
 *
 *  Comparing the *row means* rather than individual pixels keeps the
 *  test from failing on a global brightness offset, while still
 *  catching a wrong picture, a blank frame, or a torn image. */
double row_mean_correlation(const std::vector<std::uint8_t>& a,
                            const std::vector<std::uint8_t>& b,
                            std::uint32_t width, std::uint32_t height) {
    if (a.size() < static_cast<std::size_t>(width) * height) return -1.0;
    if (b.size() < static_cast<std::size_t>(width) * height) return -1.0;

    std::vector<double> ra(height), rb(height);
    for (std::uint32_t row = 0; row < height; ++row) {
        double sa = 0.0, sb = 0.0;
        for (std::uint32_t col = 0; col < width; ++col) {
            sa += a[static_cast<std::size_t>(row) * width + col];
            sb += b[static_cast<std::size_t>(row) * width + col];
        }
        ra[row] = sa / width;
        rb[row] = sb / width;
    }
    double num = 0.0, da = 0.0, db = 0.0;
    for (std::uint32_t row = 0; row < height; ++row) {
        num += ra[row] * rb[row];
        da  += ra[row] * ra[row];
        db  += rb[row] * rb[row];
    }
    if (da <= 0.0 || db <= 0.0) return -1.0;
    return num / (std::sqrt(da) * std::sqrt(db));
}

/** Encode the source picture with the software encoder. */
std::vector<std::uint8_t> encode_source(const plugins::VideoFrame& source,
                                         plugins::EncodedVideoFrame& encoded) {
    const plugins::VideoEncoderBackend* enc = openh264_backend();
    if (!enc) return {};

    plugins::VideoCodecConfig cfg;
    cfg.width         = kWidth;
    cfg.height        = kHeight;
    cfg.fps           = 30;
    cfg.bitrate_bps   = 2000000;
    cfg.pixel_format  = plugins::VideoPixelFormat::kI420;
    cfg.payload_type  = 102;

    auto codec = enc->create(cfg);
    if (!codec) return {};
    if (codec->open() != plugins::kOk) return {};

    std::vector<std::uint8_t> bitstream(512 * 1024);
    if (codec->encode(source, bitstream.data(), bitstream.size(), encoded)
        != plugins::kOk) {
        return {};
    }
    return bitstream;
}

} // namespace

// ---------------------------------------------------------------------------
// The backend row
// ---------------------------------------------------------------------------

TEST(DxvaHardware, BackendRowIsRegistered) {
    const plugins::VideoDecoderBackend* be = dxva_backend();
    ASSERT_NE(be, nullptr) << "the dxva_h264 decoder row is not registered";

    EXPECT_EQ(be->id, "dxva_h264");
    EXPECT_TRUE(be->is_hw);
    EXPECT_EQ(be->priority, 320);
    EXPECT_EQ(be->codec_kind, plugins::VideoCodecKind::kH264);

    // The decoder produces NV12 in *system memory*, so the pixels are
    // copied out of the MFT's buffer.  Claiming a zero-copy path would
    // make the engine wire the GPU output path and then get
    // "unsupported" on every frame.
    EXPECT_FALSE(be->zero_copy_supported);
}

TEST(DxvaHardware, BackendReportsItsOwnAvailability) {
    const plugins::VideoDecoderBackend* be = dxva_backend();
    ASSERT_NE(be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width  = kWidth;
    cfg.height = kHeight;

    const bool available = be->available(cfg);
    if (!available) {
        // An unavailable backend has to say *why* — that is the whole
        // point of the `unavailable_reason` seam.
        const std::string reason = be->unavailable_reason(cfg);
        EXPECT_FALSE(reason.empty())
            << "an unavailable backend must explain itself";
    }
    SUCCEED();
}

// ---------------------------------------------------------------------------
// The probe must tell the truth
// ---------------------------------------------------------------------------

/**
 * @brief The availability probe and the decoder must agree.
 *
 * This is the regression test for the bug that made the `windows / dxva`
 * CI leg red — and, far worse, made the *production* decode path select
 * a decoder that never produced a picture.
 *
 * The bug: the probe asked "does the Media Foundation H.264 MFT CLSID
 * exist?" and answered *yes* on a host with no DXVA-capable device
 * (a Windows Server 2022 hosted runner, for instance).  The registry
 * then selected `dxva_h264` (priority 320) over the software
 * `openh264_h264` (50), and the MFT — which produces **no** picture at
 * all without a D3D manager, despite the code's belief that it would
 * "decode on the CPU" — never yielded a frame.
 *
 * The contract this test pins:
 *
 *  - `available() == true`  ⇒ the decoder opens **and** really is
 *    hardware accelerated.
 *  - `available() == false` ⇒ the decoder does **not** open, so the
 *    registry moves on to the next backend instead of handing the engine
 *    a dead decoder.
 *
 * Either way the two answers agree.  A host without the capability is
 * not a bug; a probe that answers differently from the truth is.
 */
TEST(DxvaHardware, TheProbeAgreesWithTheDecoder) {
    const plugins::VideoDecoderBackend* be = dxva_backend();
    ASSERT_NE(be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width        = kWidth;
    cfg.height       = kHeight;
    cfg.fps          = 30;
    cfg.pixel_format = plugins::VideoPixelFormat::kI420;

    const bool available = be->available(cfg);

    auto codec = be->create(cfg);
    if (!codec) {
        // The decoder refused to open.  That is only legitimate when the
        // probe said the host cannot do it — otherwise the registry was
        // told "yes" and handed a dead decoder.
        EXPECT_FALSE(available)
            << "the probe says this host can decode through DXVA, but the "
               "decoder refused to open — the registry would select a "
               "backend that cannot decode";
        return;
    }

    // The decoder opened.  Then the probe must have said "yes", and the
    // decoder must really be on the hardware path.
    EXPECT_TRUE(available)
        << "the decoder opened on a host the probe reported as incapable — "
           "the probe is wrong, and the registry would skip a working "
           "decoder";
    EXPECT_TRUE(codec->is_hardware_accelerated())
        << "the decoder opened but is not running on the hardware path";
}

// ---------------------------------------------------------------------------
// The decoder instance
// ---------------------------------------------------------------------------

TEST(DxvaHardware, DecoderIsDecoderOnlyAndNamesItsBackend) {
    const plugins::VideoDecoderBackend* be = dxva_backend();
    ASSERT_NE(be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width  = kWidth;
    cfg.height = kHeight;

    SKIP_WITHOUT_DXVA(be, cfg);

    auto codec = be->create(cfg);
    ASSERT_NE(codec, nullptr);

    // DXVA is a decode-only backend -- the split seam makes that a\n    // *type* fact (IVideoDecoder), not a runtime predicate.\n    EXPECT_EQ(codec->kind(), plugins::VideoCodecKind::kH264);
    EXPECT_EQ(codec->codec_name(), "h264");
    EXPECT_EQ(codec->backend_id(), "dxva");
    EXPECT_NE(codec->backend_id(), "software-stub");

    // The hardware claim has to be tied to the session actually being
    // open — a backend that failed to initialise falls back to software
    // and must not keep claiming the GPU.
    EXPECT_TRUE(codec->is_hardware_accelerated());
}

// ---------------------------------------------------------------------------
// The decode itself
// ---------------------------------------------------------------------------

TEST(DxvaHardware, DecodesARealH264Bitstream) {
    const plugins::VideoDecoderBackend* be = dxva_backend();
    ASSERT_NE(be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width         = kWidth;
    cfg.height        = kHeight;
    cfg.fps           = 30;
    cfg.pixel_format  = plugins::VideoPixelFormat::kI420;
    cfg.payload_type  = 102;

    SKIP_WITHOUT_DXVA(be, cfg);

    // ---- Produce the bitstream with the software encoder ------------------
    const plugins::VideoFrame source = make_source_frame(kWidth, kHeight);
    plugins::EncodedVideoFrame encoded;
    const auto bitstream = encode_source(source, encoded);
    if (bitstream.empty()) {
        GTEST_SKIP() << "the software H.264 encoder is not available here";
    }
    ASSERT_GT(encoded.payload.size(), 0u);

    // ---- Decode it with DXVA ----------------------------------------------
    auto codec = be->create(cfg);
    ASSERT_NE(codec, nullptr);
    ASSERT_EQ(codec->open(), plugins::kOk);

    // The decode contract carries no stride, so the caller's planes are
    // tightly packed (stride == width).
    std::vector<std::uint8_t> dec_y(static_cast<std::size_t>(kWidth) * kHeight);
    std::vector<std::uint8_t> dec_u(static_cast<std::size_t>(kWidth / 2) * (kHeight / 2));
    std::vector<std::uint8_t> dec_v(static_cast<std::size_t>(kWidth / 2) * (kHeight / 2));
    std::uint8_t* buffers[3] = {dec_y.data(), dec_u.data(), dec_v.data()};

    plugins::VideoFrame decoded;
    const plugins::Status st = codec->decode(encoded, decoded, buffers);

    // The MFT may need more than one access unit before it emits a
    // picture, and it may report "not ready" for the first frame — so
    // the decode is retried a few times before it is called a failure.
    plugins::Status last = st;
    for (int attempt = 0; attempt < 8 && last == plugins::kErrNotReady; ++attempt) {
        last = codec->decode(encoded, decoded, buffers);
    }
    ASSERT_EQ(last, plugins::kOk)
        << "the DXVA decoder did not produce a picture";

    EXPECT_EQ(decoded.width(),  kWidth);
    EXPECT_EQ(decoded.height(), kHeight);
    EXPECT_EQ(decoded.info.frame_seq,     7u);
    EXPECT_EQ(decoded.info.capture_ts_us, 987654u);
    EXPECT_EQ(decoded.info.rtp_timestamp, 0x55667788u);

    // ---- Compare against the source --------------------------------------
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
    EXPECT_LT(mae, 12.0) << "mean absolute luma error too large — the "
                            "decoded picture does not match the source";

    const double corr = row_mean_correlation(src_y_tight, dec_y, kWidth, kHeight);
    EXPECT_GT(corr, 0.95)
        << "the decoded picture does not correlate with the source";

    // ---- Stats ------------------------------------------------------------
    const plugins::VideoCodecStats s = codec->stats();
    EXPECT_GE(s.frames_decoded, 1u);
    EXPECT_EQ(s.decode_errors, 0u);
    EXPECT_EQ(s.backend_id, "dxva");
    EXPECT_TRUE(s.hardware);
}

TEST(DxvaHardware, DecodesALengthPrefixedBitstream) {
    const plugins::VideoDecoderBackend* be = dxva_backend();
    ASSERT_NE(be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width        = kWidth;
    cfg.height       = kHeight;
    cfg.fps          = 30;
    cfg.pixel_format = plugins::VideoPixelFormat::kI420;

    SKIP_WITHOUT_DXVA(be, cfg);

    const plugins::VideoFrame source = make_source_frame(kWidth, kHeight);
    plugins::EncodedVideoFrame encoded;
    const auto bitstream = encode_source(source, encoded);
    if (bitstream.empty()) {
        GTEST_SKIP() << "the software H.264 encoder is not available here";
    }

    // The engine's RTP path delivers *length-prefixed* NAL units, while
    // the MFT wants Annex B.  The decoder has to do the conversion — a
    // decoder that only handled Annex B would fail every real frame.
    nimrtc::h264::BitstreamParser parser;
    const auto nalus = parser.parse_all(encoded.payload);
    if (nalus.empty()) GTEST_SKIP() << "the encoder produced no NAL units";

    std::vector<std::uint8_t> length_prefixed;
    for (const auto& n : nalus) {
        const std::uint32_t len = static_cast<std::uint32_t>(n.data.size());
        length_prefixed.push_back(static_cast<std::uint8_t>((len >> 24) & 0xFF));
        length_prefixed.push_back(static_cast<std::uint8_t>((len >> 16) & 0xFF));
        length_prefixed.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
        length_prefixed.push_back(static_cast<std::uint8_t>(len & 0xFF));
        length_prefixed.insert(length_prefixed.end(),
                               n.data.begin(), n.data.end());
    }

    plugins::EncodedVideoFrame lp = encoded;
    lp.nalu_format = plugins::NaluFormat::kLengthPrefixed;
    lp.payload = nimrtc::core::ByteSpan(length_prefixed.data(),
                                        length_prefixed.size());

    auto codec = be->create(cfg);
    ASSERT_NE(codec, nullptr);
    ASSERT_EQ(codec->open(), plugins::kOk);

    std::vector<std::uint8_t> dec_y(static_cast<std::size_t>(kWidth) * kHeight);
    std::vector<std::uint8_t> dec_u(static_cast<std::size_t>(kWidth / 2) * (kHeight / 2));
    std::vector<std::uint8_t> dec_v(static_cast<std::size_t>(kWidth / 2) * (kHeight / 2));
    std::uint8_t* buffers[3] = {dec_y.data(), dec_u.data(), dec_v.data()};

    plugins::VideoFrame decoded;
    plugins::Status last = codec->decode(lp, decoded, buffers);
    for (int attempt = 0; attempt < 8 && last == plugins::kErrNotReady; ++attempt) {
        last = codec->decode(lp, decoded, buffers);
    }
    ASSERT_EQ(last, plugins::kOk)
        << "the DXVA decoder did not decode a length-prefixed bitstream";
}

// ---------------------------------------------------------------------------
// The macroblock padding
// ---------------------------------------------------------------------------

TEST(DxvaHardware, CropsTheMacroblockPaddingOffTheVisiblePicture) {
    const plugins::VideoDecoderBackend* be = dxva_backend();
    ASSERT_NE(be, nullptr);

    // 322x242 is deliberately *not* a multiple of 16: H.264 codes it as
    // 336x256, and the MFT hands the padded picture over.  The decoder
    // has to crop it back to the visible size, or the caller gets a
    // picture with 14 columns and 14 rows of garbage — and, worse, one
    // whose size does not match the geometry the peer announced.
    constexpr std::uint32_t kOddWidth  = 322;
    constexpr std::uint32_t kOddHeight = 242;

    plugins::VideoCodecConfig cfg;
    cfg.width        = kOddWidth;
    cfg.height       = kOddHeight;
    cfg.fps          = 30;
    cfg.pixel_format = plugins::VideoPixelFormat::kI420;

    SKIP_WITHOUT_DXVA(be, cfg);

    const plugins::VideoFrame source = make_source_frame(kOddWidth, kOddHeight);
    plugins::EncodedVideoFrame encoded;
    const auto bitstream = encode_source(source, encoded);
    if (bitstream.empty()) {
        GTEST_SKIP() << "the software H.264 encoder is not available here";
    }

    auto codec = be->create(cfg);
    ASSERT_NE(codec, nullptr);
    ASSERT_EQ(codec->open(), plugins::kOk);

    // The decoder-owned mode: the codec allocates the buffer, because
    // only it knows that the *coded* picture is bigger than the visible
    // one.
    plugins::VideoFrame decoded;
    plugins::Status last = codec->decode(encoded, decoded, nullptr);
    for (int attempt = 0; attempt < 8 && last == plugins::kErrNotReady; ++attempt) {
        last = codec->decode(encoded, decoded, nullptr);
    }
    ASSERT_EQ(last, plugins::kOk);

    // The visible geometry, not the padded one.
    EXPECT_EQ(decoded.width(),  kOddWidth);
    EXPECT_EQ(decoded.height(), kOddHeight);

    // And the pixels have to be the *visible* picture — the crop must
    // have taken the top-left rectangle, not an arbitrary window.
    const auto src = source.cpu_buffer();
    const auto dst = decoded.cpu_buffer();
    ASSERT_NE(src, nullptr);
    ASSERT_NE(dst, nullptr);

    const std::uint32_t src_stride = src->layout().strides[0];
    const std::uint32_t dst_stride = dst->layout().strides[0];

    std::vector<std::uint8_t> src_y(static_cast<std::size_t>(kOddWidth) * kOddHeight);
    std::vector<std::uint8_t> dst_y(static_cast<std::size_t>(kOddWidth) * kOddHeight);
    for (std::uint32_t row = 0; row < kOddHeight; ++row) {
        std::copy(src->plane(0) + static_cast<std::size_t>(row) * src_stride,
                  src->plane(0) + static_cast<std::size_t>(row) * src_stride + kOddWidth,
                  src_y.begin() + static_cast<std::ptrdiff_t>(row * kOddWidth));
        std::copy(dst->plane(0) + static_cast<std::size_t>(row) * dst_stride,
                  dst->plane(0) + static_cast<std::size_t>(row) * dst_stride + kOddWidth,
                  dst_y.begin() + static_cast<std::ptrdiff_t>(row * kOddWidth));
    }

    const double mae = mean_abs_error(src_y, dst_y);
    EXPECT_LT(mae, 12.0) << "the cropped picture does not match the source";
}

TEST(DxvaHardware, AllocatesItsOwnOutputBuffer) {    const plugins::VideoDecoderBackend* be = dxva_backend();
    ASSERT_NE(be, nullptr);

    plugins::VideoCodecConfig cfg;
    cfg.width        = kWidth;
    cfg.height       = kHeight;
    cfg.fps          = 30;
    cfg.pixel_format = plugins::VideoPixelFormat::kI420;

    SKIP_WITHOUT_DXVA(be, cfg);

    const plugins::VideoFrame source = make_source_frame(kWidth, kHeight);
    plugins::EncodedVideoFrame encoded;
    const auto bitstream = encode_source(source, encoded);
    if (bitstream.empty()) {
        GTEST_SKIP() << "the software H.264 encoder is not available here";
    }

    auto codec = be->create(cfg);
    ASSERT_NE(codec, nullptr);
    ASSERT_EQ(codec->open(), plugins::kOk);

    // The decoder-owned mode: the codec allocates the buffer and
    // publishes it through `raw_out.buffer`, because only it knows the
    // geometry the bitstream really carries.
    plugins::VideoFrame decoded;
    plugins::Status last = codec->decode(encoded, decoded, nullptr);
    for (int attempt = 0; attempt < 8 && last == plugins::kErrNotReady; ++attempt) {
        last = codec->decode(encoded, decoded, nullptr);
    }
    ASSERT_EQ(last, plugins::kOk);

    const auto buf = decoded.cpu_buffer();
    ASSERT_NE(buf, nullptr);
    EXPECT_EQ(decoded.width(),  kWidth);
    EXPECT_EQ(decoded.height(), kHeight);
    EXPECT_EQ(decoded.format(), nimrtc::video_frame::PixelFormat::kI420);
    EXPECT_NE(buf->plane(0), nullptr);
    EXPECT_NE(buf->plane(1), nullptr);
    EXPECT_NE(buf->plane(2), nullptr);
}
