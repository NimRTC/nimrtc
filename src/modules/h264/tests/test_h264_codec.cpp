/**
 * @file src/modules/h264/tests/test_h264_codec.cpp
 * @brief H.264 module unit tests.
 *
 * Covers:
 *   1. BitstreamParser: start-code detection, single + multi-NALU streams.
 *   2. find_sps / find_pps / find_idr_slice helpers.
 *   3. build_annex_b roundtrip.
 *   4. StubDecoder: consumes bitstream, returns empty frame, is_keyframe set.
 *   5. CodecPluginAdapter: encode (unsupported), decode (returns empty pixels),
 *      stats, payload_type, force_keyframe.
 *   6. Plugin registration under id "h264".
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/h264/bitstream.hpp>
#include <nimrtc/h264/decoder.hpp>
#include <nimrtc/h264/codec_plugin.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/video_frame/frame.hpp>

namespace h264 = nimrtc::h264;
namespace core = nimrtc::core;
using h264::BitstreamParser;
using h264::ParsedNalu;
using h264::find_sps;
using h264::find_pps;
using h264::find_idr_slice;
using h264::build_annex_b;
using h264::StubDecoder;
using h264::create_stub_decoder;

// -----------------------------------------------------------------------------
// Helpers — build a small Annex B bitstream from NAL units
// -----------------------------------------------------------------------------

namespace {

void push_nalu(std::vector<std::uint8_t>& bs, std::initializer_list<std::uint8_t> nalu) {
    constexpr std::uint8_t sc[4] = {0x00, 0x00, 0x00, 0x01};
    bs.insert(bs.end(), sc, sc + 4);
    bs.insert(bs.end(), nalu.begin(), nalu.end());
}

} // namespace

// -----------------------------------------------------------------------------
// BitstreamParser
// -----------------------------------------------------------------------------

TEST(BitstreamParser, IsStartCode) {
    // Note: avoid variable name `not` — `not` is a C++ alternative token
    // for `!`, which makes `not[]` invalid syntax.
    const std::uint8_t sc3[]  = {0x00, 0x00, 0x01};
    const std::uint8_t sc4[]  = {0x00, 0x00, 0x00, 0x01};
    const std::uint8_t nosc[] = {0x00, 0x01, 0x02};
    EXPECT_TRUE(h264::is_start_code(core::ByteSpan{sc3, sizeof(sc3)}));
    EXPECT_TRUE(h264::is_start_code(core::ByteSpan{sc4, sizeof(sc4)}));
    EXPECT_FALSE(h264::is_start_code(core::ByteSpan{nosc, sizeof(nosc)}));
}

TEST(BitstreamParser, SingleNalu) {
    std::vector<std::uint8_t> bs;
    push_nalu(bs, {0x65, 0xAA, 0xBB});   // IDR slice
    BitstreamParser p;
    p.reset({bs.data(), bs.size()});
    ParsedNalu n;
    ASSERT_TRUE(p.next(n));
    EXPECT_EQ(n.type, nimrtc::video_payload::h264::NaluType::kSliceIDR);
    EXPECT_EQ(n.data.size(), 3u);
    EXPECT_EQ(n.data[0], 0x65);
    EXPECT_FALSE(p.next(n));
    EXPECT_TRUE(p.finished());
}

TEST(BitstreamParser, MultipleNalus) {
    std::vector<std::uint8_t> bs;
    push_nalu(bs, {0x67, 0x42});        // SPS
    push_nalu(bs, {0x68, 0xCE});        // PPS
    push_nalu(bs, {0x65, 0xAA, 0xBB});  // IDR
    push_nalu(bs, {0x41, 0xCC});        // P-slice

    BitstreamParser p;
    auto nalus = p.parse_all({bs.data(), bs.size()});
    ASSERT_EQ(nalus.size(), 4u);
    EXPECT_EQ(nalus[0].type, nimrtc::video_payload::h264::NaluType::kSPS);
    EXPECT_EQ(nalus[1].type, nimrtc::video_payload::h264::NaluType::kPPS);
    EXPECT_EQ(nalus[2].type, nimrtc::video_payload::h264::NaluType::kSliceIDR);
    EXPECT_EQ(nalus[3].type, nimrtc::video_payload::h264::NaluType::kSliceNonIDR);
}

TEST(BitstreamParser, ThreeByteStartCode) {
    std::vector<std::uint8_t> bs = {0x00, 0x00, 0x01, 0x65, 0xAA};
    BitstreamParser p;
    p.reset({bs.data(), bs.size()});
    ParsedNalu n;
    ASSERT_TRUE(p.next(n));
    EXPECT_EQ(n.start_code_len, 3u);
    EXPECT_EQ(n.type, nimrtc::video_payload::h264::NaluType::kSliceIDR);
}

// -----------------------------------------------------------------------------
// find_sps / find_pps / find_idr_slice
// -----------------------------------------------------------------------------

TEST(BitstreamFind, FindSps) {
    std::vector<std::uint8_t> bs;
    push_nalu(bs, {0x67, 0x42});
    push_nalu(bs, {0x65, 0xAA});
    auto n = find_sps({bs.data(), bs.size()});
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(n->type, nimrtc::video_payload::h264::NaluType::kSPS);
    EXPECT_EQ(n->data[0], 0x67);
}

TEST(BitstreamFind, FindPps) {
    std::vector<std::uint8_t> bs;
    push_nalu(bs, {0x67, 0x42});
    push_nalu(bs, {0x68, 0xCE});
    auto n = find_pps({bs.data(), bs.size()});
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(n->type, nimrtc::video_payload::h264::NaluType::kPPS);
}

TEST(BitstreamFind, FindIdrSlice) {
    std::vector<std::uint8_t> bs;
    push_nalu(bs, {0x67, 0x42});
    push_nalu(bs, {0x68, 0xCE});
    push_nalu(bs, {0x65, 0xAA, 0xBB});
    auto n = find_idr_slice({bs.data(), bs.size()});
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(n->type, nimrtc::video_payload::h264::NaluType::kSliceIDR);
    EXPECT_EQ(n->data.size(), 3u);
}

TEST(BitstreamFind, MissingReturnsEmpty) {
    std::vector<std::uint8_t> bs;
    push_nalu(bs, {0x41, 0xCC});    // P-slice only
    EXPECT_FALSE(find_sps({bs.data(), bs.size()}).has_value());
    EXPECT_FALSE(find_idr_slice({bs.data(), bs.size()}).has_value());
}

// -----------------------------------------------------------------------------
// build_annex_b
// -----------------------------------------------------------------------------

TEST(BitstreamBuild, Roundtrip) {
    const std::uint8_t sps[] = {0x67, 0x42};
    const std::uint8_t pps[] = {0x68, 0xCE};
    const std::uint8_t idr[] = {0x65, 0xAA};

    core::ByteSpan nalu_ptrs[3] = {
        core::ByteSpan{sps, sizeof(sps)},
        core::ByteSpan{pps, sizeof(pps)},
        core::ByteSpan{idr, sizeof(idr)},
    };

    std::uint8_t out[64];
    std::size_t n = build_annex_b(nalu_ptrs, 3, out, sizeof(out));
    EXPECT_GT(n, 0u);
    EXPECT_EQ(n, 4 * 3 + 2 + 2 + 2);    // 3 start codes + nalu sizes

    BitstreamParser p;
    auto nalus = p.parse_all({out, n});
    ASSERT_EQ(nalus.size(), 3u);
    EXPECT_EQ(nalus[0].type, nimrtc::video_payload::h264::NaluType::kSPS);
    EXPECT_EQ(nalus[1].type, nimrtc::video_payload::h264::NaluType::kPPS);
    EXPECT_EQ(nalus[2].type, nimrtc::video_payload::h264::NaluType::kSliceIDR);
}

TEST(BitstreamBuild, EmptyReturnsZero) {
    std::uint8_t out[8];
    EXPECT_EQ(build_annex_b(nullptr, 0, out, sizeof(out)), 0u);
}

// -----------------------------------------------------------------------------
// StubDecoder
// -----------------------------------------------------------------------------

TEST(StubDecoder, IsStub) {
    auto d = create_stub_decoder();
    ASSERT_NE(d, nullptr);
    EXPECT_TRUE(d->is_stub());
    EXPECT_EQ(d->open(), nimrtc::plugins::kOk);
}

TEST(StubDecoder, DecodeIdrMarksKeyframe) {
    auto d = create_stub_decoder();
    d->open();

    std::vector<std::uint8_t> bs;
    push_nalu(bs, {0x67, 0x42});          // SPS
    push_nalu(bs, {0x68, 0xCE});          // PPS
    push_nalu(bs, {0x65, 0xAA, 0xBB});    // IDR

    nimrtc::video_frame::VideoFrameInfo info;
    info.capture_ts_us = 12345;
    info.frame_seq     = 42;
    info.rtp_timestamp = 9999;

    auto out = d->decode({bs.data(), bs.size()}, info);
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(out->is_keyframe);
    EXPECT_EQ(out->capture_ts_us, 12345);
    EXPECT_EQ(out->frame_seq,     42u);
    EXPECT_EQ(out->rtp_timestamp, 9999u);
    // Stub: buffer is empty.
    EXPECT_FALSE(out->buffer.valid());
}

TEST(StubDecoder, DecodeWithoutIdrIsNotKeyframe) {
    auto d = create_stub_decoder();
    d->open();

    std::vector<std::uint8_t> bs;
    push_nalu(bs, {0x41, 0xAA, 0xBB});   // P-slice, no SPS/PPS

    nimrtc::video_frame::VideoFrameInfo info;
    auto out = d->decode({bs.data(), bs.size()}, info);
    ASSERT_TRUE(out.has_value());
    EXPECT_FALSE(out->is_keyframe);
}

TEST(StubDecoder, RejectsEmpty) {
    auto d = create_stub_decoder();
    d->open();
    nimrtc::video_frame::VideoFrameInfo info;
    auto out = d->decode({}, info);
    EXPECT_FALSE(out.has_value());
}

// -----------------------------------------------------------------------------
// CodecPluginAdapter
// -----------------------------------------------------------------------------

TEST(CodecPluginAdapter, EncodeProducesSyntheticBitstream) {
    // The shipped H.264 plugin is a *stub* encoder that emits a synthetic
    // Annex B bitstream (SPS + PPS + IDR) suitable for exercising the
    // send_video() → RTP → SRTP → ICE → recv → FU-A reassembly → decode
    // → render pipeline without dragging in libopenh264/x264.  A real
    // codec plugin should replace this via a different id.
    nimrtc::plugins::VideoCodecConfig cfg;
    cfg.width  = 320;
    cfg.height = 240;
    nimrtc::h264::CodecPluginAdapter adapter(cfg);
    EXPECT_EQ(adapter.open(), nimrtc::plugins::kOk);
    EXPECT_TRUE(adapter.can_encode());
    EXPECT_TRUE(adapter.can_decode());

    nimrtc::plugins::VideoFrame raw;
    raw.info.frame_seq = 7;

    std::uint8_t buf[2048] = {0};
    nimrtc::plugins::EncodedVideoFrame out;
    EXPECT_EQ(adapter.encode(raw, buf, sizeof(buf), out),
              nimrtc::plugins::kOk);
    EXPECT_GT(out.payload.size(), 0u);
    EXPECT_TRUE(out.is_keyframe);
    EXPECT_EQ(out.codec, nimrtc::plugins::VideoCodecKind::kH264);
    EXPECT_EQ(out.nalu_format,
              nimrtc::plugins::NaluFormat::kAnnexBStartCode);
    // Bitstream must contain at least SPS, PPS and IDR NAL units.
    h264::BitstreamParser parser;
    auto nalus = parser.parse_all(out.payload);
    ASSERT_GE(nalus.size(), 3u);
    bool saw_sps = false, saw_pps = false, saw_idr = false;
    for (const auto& n : nalus) {
        if (n.type == nimrtc::video_payload::h264::NaluType::kSPS)  saw_sps = true;
        if (n.type == nimrtc::video_payload::h264::NaluType::kPPS)  saw_pps = true;
        if (n.type == nimrtc::video_payload::h264::NaluType::kSliceIDR) saw_idr = true;
    }
    EXPECT_TRUE(saw_sps);
    EXPECT_TRUE(saw_pps);
    EXPECT_TRUE(saw_idr);

    auto s = adapter.stats();
    EXPECT_EQ(s.frames_encoded, 1u);
    EXPECT_GT(s.bytes_encoded,  out.payload.size() - 1u);

    // Missing dimensions still yields kErrInvalidParam.
    nimrtc::h264::CodecPluginAdapter empty_adapter({});
    ASSERT_EQ(empty_adapter.open(), nimrtc::plugins::kOk);
    nimrtc::plugins::VideoFrame raw_empty;     // width=0, height=0
    std::uint8_t small_buf[64] = {0};
    nimrtc::plugins::EncodedVideoFrame empty_out;
    EXPECT_EQ(empty_adapter.encode(raw_empty, small_buf, sizeof(small_buf), empty_out),
              nimrtc::plugins::kErrInvalidParam);

    adapter.close();
}

TEST(CodecPluginAdapter, DecodePaintsSyntheticI420) {
    // The shipped H.264 decoder is a *stub*: it consumes the bitstream,
    // propagates the metadata, and paints each output frame with a
    // synthetic I420 pattern (3 horizontal bands whose brightness
    // depends on frame_seq).  This is enough to validate the full
    // pipeline and make round-tripped frames visually identifiable.
    nimrtc::plugins::VideoCodecConfig cfg;
    cfg.width  = 320;
    cfg.height = 240;
    nimrtc::h264::CodecPluginAdapter adapter(cfg);
    ASSERT_EQ(adapter.open(), nimrtc::plugins::kOk);

    std::vector<std::uint8_t> bs;
    push_nalu(bs, {0x67, 0x42});
    push_nalu(bs, {0x68, 0xCE});
    push_nalu(bs, {0x65, 0xAA, 0xBB});

    nimrtc::plugins::EncodedVideoFrame encoded;
    encoded.codec         = nimrtc::plugins::VideoCodecKind::kH264;
    encoded.nalu_format   = nimrtc::plugins::NaluFormat::kAnnexBStartCode;
    encoded.payload       = core::ByteSpan{bs.data(), bs.size()};
    encoded.is_keyframe   = true;
    encoded.payload_type  = 102;
    encoded.info.frame_seq      = 1;
    encoded.info.capture_ts_us  = 1234;
    encoded.info.rtp_timestamp  = 9999;

    nimrtc::plugins::VideoFrame raw_out;
    const std::uint32_t w = cfg.width;
    const std::uint32_t h = cfg.height;
    std::vector<std::uint8_t> y_plane(static_cast<std::size_t>(w) * h);
    std::vector<std::uint8_t> u_plane(static_cast<std::size_t>(w / 2) * (h / 2));
    std::vector<std::uint8_t> v_plane(static_cast<std::size_t>(w / 2) * (h / 2));
    std::uint8_t* buffers[3] = {y_plane.data(), u_plane.data(), v_plane.data()};

    EXPECT_EQ(adapter.decode(encoded, raw_out, buffers), nimrtc::plugins::kOk);
    EXPECT_EQ(raw_out.format(), nimrtc::plugins::VideoPixelFormat::kI420);
    EXPECT_EQ(raw_out.info.frame_seq,     1u);
    EXPECT_EQ(raw_out.info.capture_ts_us, 1234);
    EXPECT_EQ(raw_out.info.rtp_timestamp, 9999u);
    EXPECT_EQ(raw_out.width(),  w);
    EXPECT_EQ(raw_out.height(), h);
    // The frame buffer is type-erased; check it carries CPU pixel data
    // via a VideoFrameBuffer rather than accessing the removed plane_y/u/v
    // fields directly.
    auto cpu = raw_out.cpu_buffer();
    EXPECT_NE(cpu, nullptr);
    EXPECT_NE(cpu->plane(0), nullptr);
    EXPECT_NE(cpu->plane(1), nullptr);
    EXPECT_NE(cpu->plane(2), nullptr);
    // paint_i420() writes non-zero bytes to every Y pixel (ramp derived
    // from frame_seq) — verify a few samples are not pristine zero.
    EXPECT_NE(y_plane[0], 0u);
    EXPECT_NE(y_plane[(w * h) / 2], 0u);

    auto s = adapter.stats();
    EXPECT_EQ(s.frames_decoded, 1u);
    EXPECT_GT(s.bytes_decoded,  0u);

    adapter.close();
}

TEST(CodecPluginAdapter, DecodeEmptyFails) {
    nimrtc::h264::CodecPluginAdapter adapter({});
    ASSERT_EQ(adapter.open(), nimrtc::plugins::kOk);

    nimrtc::plugins::EncodedVideoFrame encoded;
    nimrtc::plugins::VideoFrame raw_out;
    std::uint8_t dummy[1] = {};
    std::uint8_t* buffers[3] = {dummy, dummy, dummy};
    EXPECT_EQ(adapter.decode(encoded, raw_out, buffers),
              nimrtc::plugins::kErrInvalidParam);
    adapter.close();
}

TEST(CodecPluginAdapter, PayloadTypeDefaultsTo102) {
    nimrtc::h264::CodecPluginAdapter adapter({});
    EXPECT_EQ(adapter.payload_type(), 102);
}

TEST(CodecPluginAdapter, PayloadTypeRespectsConfig) {
    nimrtc::plugins::VideoCodecConfig cfg;
    cfg.payload_type = 99;
    nimrtc::h264::CodecPluginAdapter adapter(cfg);
    EXPECT_EQ(adapter.payload_type(), 99);
}

TEST(CodecPluginAdapter, ForceKeyframeIsNoOp) {
    nimrtc::h264::CodecPluginAdapter adapter({});
    EXPECT_EQ(adapter.force_keyframe(), nimrtc::plugins::kOk);
}

// -----------------------------------------------------------------------------
// Plugin registration
// -----------------------------------------------------------------------------

TEST(Registration, H264FactoryRegistered) {
    nimrtc::h264::register_default_plugins();
    auto* factory = nimrtc::core::PluginRegistry::instance().get_video_codec("h264");
    ASSERT_NE(factory, nullptr);
    EXPECT_EQ(factory->id(), "h264");
}

TEST(Registration, FactoryCreatesCodec) {
    nimrtc::h264::register_default_plugins();
    auto* factory = nimrtc::core::PluginRegistry::instance().get_video_codec("h264");
    ASSERT_NE(factory, nullptr);
    nimrtc::plugins::VideoCodecConfig cfg;
    auto* codec = factory->create(cfg);
    ASSERT_NE(codec, nullptr);
    EXPECT_EQ(codec->kind(), nimrtc::plugins::VideoCodecKind::kH264);
    EXPECT_EQ(std::string(codec->codec_name()), "h264");
    delete codec;
}
