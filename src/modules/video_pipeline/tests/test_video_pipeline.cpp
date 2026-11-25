/**
 * @file src/modules/video_pipeline/tests/test_video_pipeline.cpp
 * @brief VideoReceiver / VideoSender reference implementation tests.
 *
 * Covers:
 *   1-6.   Receiver (H.264 / FU-A / STAP-A / NACK / timeout / reset / VP8/VP9)
 *   7-12.  Sender (single NAL / STAP-A / FU-A / RTP ts / seq / force_keyframe)
 *   13-14. End-to-end Sender -> Receiver.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/video_pipeline/video_pipeline.hpp>
#include <nimrtc/video_payload/h264.hpp>
#include <nimrtc/video_payload/h265.hpp>
#include <nimrtc/video_payload/vp8.hpp>
#include <nimrtc/video_payload/vp9.hpp>
#include <nimrtc/video_frame/frame.hpp>
#include <nimrtc/video_jb/video_jitter_buffer.hpp>
#include <nimrtc/rtp/packet.hpp>

namespace vp = nimrtc::video_pipeline;
namespace h264 = nimrtc::video_payload::h264;
namespace h265 = nimrtc::video_payload::h265;
namespace core = nimrtc::core;
namespace video_frame = nimrtc::video_frame;
namespace rtp = nimrtc::rtp;

using vp::VideoReceiver;
using vp::VideoSender;
using vp::ReceiverConfig;
using vp::SenderConfig;
using vp::ReceiverFrameCallback;
using vp::SenderPacketCallback;
using vp::PacketizationMode;
using h264::NaluType;
using h264::build_fu_a;
using h264::build_stap_a;

namespace {

constexpr std::uint32_t kSsrc = 0x12345678u;
constexpr std::uint8_t  kPt   = 102;        // WebRTC H264 default

// The Annex-B start code that the receiver reconstructs on every H.264 /
// H.265 NAL unit it emits (RFC 6184 §5.1 carries bare NAL units in RTP;
// the frame contract `video_frame::NaluFormat::kAnnexBStartCode` promises
// the decoder sees `00 00 00 01 | nalu`).
constexpr std::uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};

/// Build the expected Annex-B bitstream: start code + NAL unit.
std::vector<std::uint8_t> annex_b(const std::vector<std::uint8_t>& nalu) {
    std::vector<std::uint8_t> out;
    out.insert(out.end(), kStartCode, kStartCode + sizeof(kStartCode));
    out.insert(out.end(), nalu.begin(), nalu.end());
    return out;
}

std::vector<std::uint8_t> annex_b(std::initializer_list<std::uint8_t> nalu) {
    return annex_b(std::vector<std::uint8_t>(nalu));
}

// ---------------------------------------------------------------------------
// Minimal RTP packet holder.  We construct a 12-byte fixed header + payload
// in a contiguous buffer; the Pipeline parses the header via rtp::Parser.
// ---------------------------------------------------------------------------

struct RtpBytes {
    std::vector<std::uint8_t> raw;

    RtpBytes() = default;

    std::uint8_t*       payload_ptr()       noexcept { return raw.data() + 12; }
    const std::uint8_t* payload_ptr() const noexcept { return raw.data() + 12; }
    std::size_t         payload_size() const noexcept {
        return raw.size() > 12 ? raw.size() - 12 : 0;
    }
};

RtpBytes make_rtp(std::uint16_t seq,
                  std::uint32_t ts,
                  bool          marker,
                  std::uint8_t  pt,
                  std::uint32_t ssrc,
                  core::ByteSpan payload) {
    RtpBytes b;
    b.raw.resize(12 + payload.size());
    // RFC 3550 §5.1 fixed header:
    //   byte 0 : V(2) P(1) X(1) CC(4)   = 0x80 for V=2, others 0
    //   byte 1 : M(1) PT(7)
    //   bytes 2..3 : sequence number (big-endian)
    //   bytes 4..7 : timestamp          (big-endian)
    //   bytes 8..11: SSRC               (big-endian)
    b.raw[0]  = static_cast<std::uint8_t>(0x80);
    b.raw[1]  = static_cast<std::uint8_t>(
        (marker ? 0x80 : 0) | (pt & 0x7F));
    b.raw[2]  = static_cast<std::uint8_t>(seq >> 8);
    b.raw[3]  = static_cast<std::uint8_t>(seq & 0xFF);
    b.raw[4]  = static_cast<std::uint8_t>(ts   >> 24);
    b.raw[5]  = static_cast<std::uint8_t>(ts   >> 16);
    b.raw[6]  = static_cast<std::uint8_t>(ts   >>  8);
    b.raw[7]  = static_cast<std::uint8_t>(ts   & 0xFF);
    b.raw[8]  = static_cast<std::uint8_t>(ssrc >> 24);
    b.raw[9]  = static_cast<std::uint8_t>(ssrc >> 16);
    b.raw[10] = static_cast<std::uint8_t>(ssrc >>  8);
    b.raw[11] = static_cast<std::uint8_t>(ssrc & 0xFF);
    if (!payload.empty()) {
        std::memcpy(b.raw.data() + 12, payload.data(), payload.size());
    }
    return b;
}

rtp::PacketView parse_rtp(const RtpBytes& b) {
    nimrtc::rtp::PacketView v;
    v.raw     = core::ByteSpan{b.raw.data(), b.raw.size()};
    v.payload = core::ByteSpan{b.raw.data() + 12, b.raw.size() - 12};
    v.payload_type = static_cast<std::uint8_t>(b.raw[1] & 0x7F);
    v.marker       = (b.raw[1] & 0x80) != 0;
    v.seq     = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(b.raw[2]) << 8) |
         static_cast<std::uint16_t>(b.raw[3]));
    v.timestamp = (static_cast<std::uint32_t>(b.raw[4]) << 24) |
                  (static_cast<std::uint32_t>(b.raw[5]) << 16) |
                  (static_cast<std::uint32_t>(b.raw[6]) <<  8) |
                   static_cast<std::uint32_t>(b.raw[7]);
    v.ssrc       = (static_cast<std::uint32_t>(b.raw[8])  << 24) |
                   (static_cast<std::uint32_t>(b.raw[9])  << 16) |
                   (static_cast<std::uint32_t>(b.raw[10]) <<  8) |
                    static_cast<std::uint32_t>(b.raw[11]);
    return v;
}

// ---------------------------------------------------------------------------
// Frame record (for tests that capture user-callback results).
// ---------------------------------------------------------------------------

struct FrameRecord {
    video_frame::EncodedVideoFrame frame{};
    std::int64_t                   now_us = 0;
};

// Thread-safe capture of all callback invocations.
class FrameRecorder {
public:
    ReceiverFrameCallback as_callback() {
        return [this](video_frame::EncodedVideoFrame f, std::int64_t t) {
            std::lock_guard<std::mutex> lk(m_);
            FrameRecord rec;
            rec.frame.codec        = f.codec;
            rec.frame.nalu_format  = f.nalu_format;
            rec.frame.is_keyframe  = f.is_keyframe;
            rec.frame.payload_type = f.payload_type;
            rec.frame.info         = f.info;
            // The receiver's contract says the payload buffer is only valid
            // for the duration of the callback; we must deep-copy before
            // storing the frame so the assertion can inspect the bytes.
            const std::size_t off = copy_bytes_.size();
            copy_bytes_.resize(off + f.payload.size());
            if (f.payload.size() > 0) {
                std::memcpy(copy_bytes_.data() + off,
                            f.payload.data(), f.payload.size());
            }
            rec.frame.payload = core::ByteSpan{
                copy_bytes_.data() + off, f.payload.size()};
            rec.now_us = t;
            records_.push_back(std::move(rec));
        };
    }
    std::vector<FrameRecord> snapshot() {
        std::lock_guard<std::mutex> lk(m_);
        return records_;
    }
private:
    std::mutex              m_;
    std::vector<FrameRecord> records_;
    // Owns all payload bytes ever captured.  Each FrameRecord's payload
    // points into this buffer.
    std::vector<std::uint8_t> copy_bytes_;
};

// ---------------------------------------------------------------------------
// Packet recorder (sender callback side).
// ---------------------------------------------------------------------------

struct SentPacket {
    std::vector<std::uint8_t> payload;
    std::uint32_t              rtp_ts = 0;
    std::uint16_t              seq    = 0;
    bool                       marker = false;
};

class PacketRecorder {
public:
    SenderPacketCallback as_callback() {
        return [this](core::ByteSpan p, std::uint32_t ts,
                      std::uint16_t seq, bool m) {
            std::lock_guard<std::mutex> lk(m_);
            SentPacket sp;
            sp.payload.assign(p.begin(), p.end());
            sp.rtp_ts = ts;
            sp.seq    = seq;
            sp.marker = m;
            records_.push_back(std::move(sp));
        };
    }
    std::vector<SentPacket> snapshot() {
        std::lock_guard<std::mutex> lk(m_);
        return records_;
    }
private:
    std::mutex             m_;
    std::vector<SentPacket> records_;
};

} // namespace

// ===========================================================================
// 1. Receiver: H.264 single NALU (marker=1) -> callback fires once.
// ===========================================================================

TEST(VideoPipeline, ReceiverH264SingleNal) {
    ReceiverConfig rc;
    rc.codec        = video_frame::CodecKind::kH264;
    rc.ssrc         = kSsrc;
    rc.payload_type = kPt;

    VideoReceiver rx(rc);
    FrameRecorder rec;
    rx.set_frame_callback(rec.as_callback());

    std::uint8_t nalu[] = {0x65, 0xAA, 0xBB, 0xCC};   // IDR slice
    auto pkt = make_rtp(/*seq=*/100, /*ts=*/1000,
                        /*marker=*/true, kPt, kSsrc, {nalu, sizeof(nalu)});
    auto view = parse_rtp(pkt);
    EXPECT_TRUE(rx.push_rtp(view, /*now_us=*/1000));

    auto recs = rec.snapshot();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].frame.codec, video_frame::CodecKind::kH264);
    // RFC 6184 §5.1 carries a *bare* NAL unit in the RTP payload -- the
    // start code is not on the wire.  The receiver puts it back, because
    // the frame contract is Annex-B (`NaluFormat::kAnnexBStartCode`) and
    // every decoder NimRTC ships parses the Annex-B framing.  Without the
    // start code the decoder cannot find the NAL boundary and refuses
    // the picture.
    const auto expected = annex_b(std::vector<std::uint8_t>(nalu, nalu + sizeof(nalu)));
    ASSERT_EQ(recs[0].frame.payload.size(), expected.size());
    EXPECT_EQ(std::memcmp(recs[0].frame.payload.data(),
                          expected.data(), expected.size()), 0);
    EXPECT_EQ(recs[0].frame.nalu_format,
              video_frame::NaluFormat::kAnnexBStartCode);
    EXPECT_TRUE(recs[0].frame.is_keyframe);
    EXPECT_EQ(rx.stats().frames_emitted, 1u);
}

// ===========================================================================
// 1a. Receiver: the escape hatch -- a host whose decoder consumes the raw
//      NAL units gets them byte for byte.
//
// This is the shape RFC 6184 §5.1 puts on the wire, and it is the shape
// the frame contract does NOT promise.  It is opt-in because the decoder
// has to re-frame the picture itself; the default path is the Annex-B one
// above.
// ===========================================================================

TEST(VideoPipeline, ReceiverH264BareNalEscapeHatch) {
    ReceiverConfig rc;
    rc.codec        = video_frame::CodecKind::kH264;
    rc.ssrc         = kSsrc;
    rc.payload_type = kPt;
    rc.h264_bare_nalus = true;

    VideoReceiver rx(rc);
    FrameRecorder rec;
    rx.set_frame_callback(rec.as_callback());

    std::uint8_t nalu[] = {0x65, 0xAA, 0xBB, 0xCC};
    auto pkt = make_rtp(/*seq=*/100, /*ts=*/1000,
                        /*marker=*/true, kPt, kSsrc, {nalu, sizeof(nalu)});
    EXPECT_TRUE(rx.push_rtp(parse_rtp(pkt), /*now_us=*/1000));

    auto recs = rec.snapshot();
    ASSERT_EQ(recs.size(), 1u);
    ASSERT_EQ(recs[0].frame.payload.size(), sizeof(nalu));
    EXPECT_EQ(std::memcmp(recs[0].frame.payload.data(), nalu, sizeof(nalu)), 0);
}

// ===========================================================================
// 2. Receiver: FU-A start + middle + end -> reassembled single NALU.
// ===========================================================================

TEST(VideoPipeline, ReceiverFuAReassembly) {
    ReceiverConfig rc;
    rc.codec        = video_frame::CodecKind::kH264;
    rc.ssrc         = kSsrc;
    rc.payload_type = kPt;

    VideoReceiver rx(rc);
    FrameRecorder rec;
    rx.set_frame_callback(rec.as_callback());

    // Original NALU = header(0x65) + body(5 bytes).  Split as FU-A:
    std::uint8_t body[5] = {0x11, 0x22, 0x33, 0x44, 0x55};

    std::uint8_t f0[8], f1[8], f2[8];
    const std::size_t n0 = build_fu_a(NaluType::kSliceIDR,
                                      {body,     2},
                                      true,  false,
                                      {f0, sizeof(f0)});
    const std::size_t n1 = build_fu_a(NaluType::kSliceIDR,
                                      {body + 2, 2},
                                      false, false,
                                      {f1, sizeof(f1)});
    const std::size_t n2 = build_fu_a(NaluType::kSliceIDR,
                                      {body + 4, 1},
                                      false, true,
                                      {f2, sizeof(f2)});
    ASSERT_GT(n0, 0u);
    ASSERT_GT(n1, 0u);
    ASSERT_GT(n2, 0u);

    auto p0 = make_rtp(100, 2000, false, kPt, kSsrc, {f0, n0});
    auto p1 = make_rtp(101, 2000, false, kPt, kSsrc, {f1, n1});
    auto p2 = make_rtp(102, 2000, true,  kPt, kSsrc, {f2, n2});
    // Patch the FU-A indicator's NRI to 3 (matches the original 0x65 NALU).
    // build_fu_a() defaults to NRI=0; we restore it here so the
    // reassembled NALU's first byte matches the original.
    p0.raw[12] = (p0.raw[12] & ~0x60) | 0x60;   // F=0, NRI=3
    p1.raw[12] = (p1.raw[12] & ~0x60) | 0x60;
    p2.raw[12] = (p2.raw[12] & ~0x60) | 0x60;

    EXPECT_TRUE(rx.push_rtp(parse_rtp(p0), 1000));
    EXPECT_TRUE(rx.push_rtp(parse_rtp(p1), 1100));
    EXPECT_TRUE(rx.push_rtp(parse_rtp(p2), 1200));

    auto recs = rec.snapshot();
    ASSERT_EQ(recs.size(), 1u);
    // Reassembled NALU = synthesized header + body, re-framed in Annex-B.
    std::vector<std::uint8_t> expected_nalu;
    expected_nalu.push_back(0x65);
    expected_nalu.insert(expected_nalu.end(), body, body + sizeof(body));
    const auto expected = annex_b(expected_nalu);
    ASSERT_EQ(recs[0].frame.payload.size(), expected.size());
    EXPECT_EQ(std::memcmp(recs[0].frame.payload.data(),
                          expected.data(), expected.size()), 0);
    EXPECT_TRUE(recs[0].frame.is_keyframe);
    EXPECT_EQ(rx.stats().fua_fragments_seen, 3u);
}

// ===========================================================================
// 2a'. Receiver: a single NAL unit packet that ALREADY carries its Annex-B
//      start code is forwarded byte for byte -- exactly one start code in
//      the frame, not two.
//
// NVENC is handed `00 00 00 01 | nalu` access units, and the sender it is
// wired to takes the bitstream as it is: the single NAL unit that lands in
// the RTP payload still carries the start code.  Appending another one at
// the receiver framed every NAL unit twice, and the extra `00 00 00 01`
// bytes sent the decoder off the rails.
// ===========================================================================

TEST(VideoPipeline, ReceiverFramedSingleNalIsNotDoubleFramed) {
    ReceiverConfig rc;
    rc.codec        = video_frame::CodecKind::kH264;
    rc.ssrc         = kSsrc;
    rc.payload_type = kPt;

    VideoReceiver rx(rc);
    FrameRecorder rec;
    rx.set_frame_callback(rec.as_callback());

    const std::uint8_t framed[] = {0x00, 0x00, 0x00, 0x01, 0x61,
                                   0xE0, 0x42, 0x1D, 0xFF, 0xFE};
    auto p = make_rtp(200, 9000, true, kPt, kSsrc,
                      {framed, sizeof(framed)});
    EXPECT_TRUE(rx.push_rtp(parse_rtp(p), 1000));

    auto recs = rec.snapshot();
    ASSERT_EQ(recs.size(), 1u);
    ASSERT_EQ(recs[0].frame.payload.size(), sizeof(framed));
    EXPECT_EQ(std::memcmp(recs[0].frame.payload.data(), framed,
                          sizeof(framed)), 0);
}

// ===========================================================================
// 2b. Receiver: SPS + PPS + IDR in one picture, in the shape NVENC emits
//     it -> every NAL unit carries its own Annex-B start code, and the
//     slice survives the parameter sets that arrive *after* it started.
//
// The packet layout is the one a hardware encoder produces: the small
// parameter sets are aggregated into a STAP-A packet, the oversized
// slice is fragmented into FU-A packets, and the STAP-A and the slice's
// first fragments share the one picture.  An in-flight slice must not be
// framed as a complete NAL unit behind its (still missing) real header,
// or the parameter sets spliced in afterwards would land *inside* it.
// ===========================================================================

TEST(VideoPipeline, ReceiverMultiNalFrameIsAnnexBFramed) {
    ReceiverConfig rc;
    rc.codec        = video_frame::CodecKind::kH264;
    rc.ssrc         = kSsrc;
    rc.payload_type = kPt;

    VideoReceiver rx(rc);
    FrameRecorder rec;
    rx.set_frame_callback(rec.as_callback());

    // SPS + PPS aggregated into one STAP-A packet.
    const std::uint8_t sps[] = {0x67, 0x42, 0x00, 0x1E};
    const std::uint8_t pps[] = {0x68, 0xCE, 0x38, 0x80};
    std::uint8_t stap[64];
    core::ByteSpan nalus[2] = {{sps, sizeof(sps)}, {pps, sizeof(pps)}};
    const std::size_t stap_n = build_stap_a(nalus, 2, {stap, sizeof(stap)});
    ASSERT_GT(stap_n, 0u);

    // An IDR slice fragmented into three FU-A packets.
    const std::uint8_t body[] = {0x88, 0x84, 0x21, 0x43, 0x65};
    std::uint8_t f0[64], f1[64], f2[64];
    const std::size_t n0 = build_fu_a(NaluType::kSliceIDR,
                                      {body, 2},
                                      true, false,
                                      {f0, sizeof(f0)});
    const std::size_t n1 = build_fu_a(NaluType::kSliceIDR,
                                      {body + 2, 2},
                                      false, false,
                                      {f1, sizeof(f1)});
    const std::size_t n2 = build_fu_a(NaluType::kSliceIDR,
                                      {body + 4, 1},
                                      false, true,
                                      {f2, sizeof(f2)});
    ASSERT_GT(n0, 0u);
    ASSERT_GT(n1, 0u);
    ASSERT_GT(n2, 0u);

    auto p0 = make_rtp(100, 5000, false, kPt, kSsrc, {stap, stap_n});
    auto p1 = make_rtp(101, 5000, false, kPt, kSsrc, {f0, n0});
    auto p2 = make_rtp(102, 5000, false, kPt, kSsrc, {f1, n1});
    auto p3 = make_rtp(103, 5000, true,  kPt, kSsrc, {f2, n2});
    // The FU-A indicator's NRI must match the original 0x65 NALU header
    // (`build_fu_a` defaults to NRI=0).
    p1.raw[12] = (p1.raw[12] & ~0x60) | 0x60;
    p2.raw[12] = (p2.raw[12] & ~0x60) | 0x60;
    p3.raw[12] = (p3.raw[12] & ~0x60) | 0x60;

    EXPECT_TRUE(rx.push_rtp(parse_rtp(p0), 1000));
    EXPECT_TRUE(rx.push_rtp(parse_rtp(p1), 1100));
    EXPECT_TRUE(rx.push_rtp(parse_rtp(p2), 1200));
    EXPECT_TRUE(rx.push_rtp(parse_rtp(p3), 1300));

    auto recs = rec.snapshot();
    ASSERT_EQ(recs.size(), 1u);

    // The frame is the concatenation of the three NAL units, each in
    // Annex-B framing — in the order the sender emitted them.
    std::vector<std::uint8_t> rectified;
    rectified.push_back(0x65);
    rectified.insert(rectified.end(), body, body + sizeof(body));

    std::vector<std::uint8_t> expected;
    const std::uint8_t* nals[3] = {sps, pps, rectified.data()};
    const std::size_t   lens[3] = {sizeof(sps), sizeof(pps), rectified.size()};
    for (int i = 0; i < 3; ++i) {
        expected.insert(expected.end(), kStartCode, kStartCode + sizeof(kStartCode));
        expected.insert(expected.end(), nals[i], nals[i] + lens[i]);
    }

    ASSERT_EQ(recs[0].frame.payload.size(), expected.size());
    EXPECT_EQ(std::memcmp(recs[0].frame.payload.data(),
                          expected.data(), expected.size()), 0);
    EXPECT_EQ(recs[0].frame.nalu_format, video_frame::NaluFormat::kAnnexBStartCode);
    EXPECT_TRUE(recs[0].frame.is_keyframe);
}

// ===========================================================================
// 2b'. Receiver: a P-frame assembled from SEVERAL bare single-NAL packets
//      is still an Annex-B frame.
//
// This is the case that cost the live demo ~81% of its frames.
//
// The sender's picture is `SPS | PPS | slice`, and the slice is small
// enough to fit in one packet -- so the sender emits the parameter sets
// as a STAP-A (which the receiver unpacks, correctly framing each unit)
// and the slice as a *single NAL unit packet*, which RFC 6184 §5.1 puts
// on the wire bare.
//
// The jitter buffer only *concatenates* the picture's payloads.  So
// before the fix the picture reached the decoder as
//
//     00 00 00 01 | SPS  00 00 00 01 | PPS  <bare slice>
//
// -- the slice with no start code in front of it.  The decoder cannot
// find that NAL boundary, refuses the picture, and the engine counts it
// as a decoder drop and asks for a key frame.  Only the IDR frames (whose
// slice is large enough to be FU-A fragmented, and therefore reassembled
// *and* framed) ever decoded -- hence the observed ~1-decoded-per-6
// ratio.
//
// The frame the decoder receives must therefore carry a start code ahead
// of EVERY NAL unit, including the one that arrived bare.
// ===========================================================================

TEST(VideoPipeline, ReceiverPFrameOfBareSingleNalPacketsIsAnnexBFramed) {
    ReceiverConfig rc;
    rc.codec        = video_frame::CodecKind::kH264;
    rc.ssrc         = kSsrc;
    rc.payload_type = kPt;

    VideoReceiver rx(rc);
    FrameRecorder rec;
    rx.set_frame_callback(rec.as_callback());

    // The parameter sets, aggregated into one STAP-A packet.
    const std::uint8_t sps[] = {0x67, 0x42, 0x00, 0x1E};
    const std::uint8_t pps[] = {0x68, 0xCE, 0x38, 0x80};
    std::uint8_t stap[64];
    core::ByteSpan nalus[2] = {{sps, sizeof(sps)}, {pps, sizeof(pps)}};
    const std::size_t stap_n = build_stap_a(nalus, 2, {stap, sizeof(stap)});
    ASSERT_GT(stap_n, 0u);

    // The P-slice: one bare NAL unit, small enough to fit a single packet.
    const std::uint8_t slice[] = {0x61, 0xE0, 0x22, 0x33, 0x44};

    auto p0 = make_rtp(100, 5000, false, kPt, kSsrc, {stap, stap_n});
    auto p1 = make_rtp(101, 5000, true,  kPt, kSsrc, {slice, sizeof(slice)});

    EXPECT_TRUE(rx.push_rtp(parse_rtp(p0), 1000));
    EXPECT_TRUE(rx.push_rtp(parse_rtp(p1), 1100));

    auto recs = rec.snapshot();
    ASSERT_EQ(recs.size(), 1u);

    // Every NAL unit carries its own start code -- the slice included.
    std::vector<std::uint8_t> expected;
    const std::uint8_t* nals[3] = {sps, pps, slice};
    const std::size_t   lens[3] = {sizeof(sps), sizeof(pps), sizeof(slice)};
    for (int i = 0; i < 3; ++i) {
        expected.insert(expected.end(), kStartCode, kStartCode + sizeof(kStartCode));
        expected.insert(expected.end(), nals[i], nals[i] + lens[i]);
    }

    ASSERT_EQ(recs[0].frame.payload.size(), expected.size());
    EXPECT_EQ(std::memcmp(recs[0].frame.payload.data(),
                          expected.data(), expected.size()), 0)
        << "the bare P-slice reached the decoder without a start code";
    EXPECT_EQ(recs[0].frame.nalu_format,
              video_frame::NaluFormat::kAnnexBStartCode);
}

// ===========================================================================
// 2c. Receiver: STAP-A aggregation -> each unpacked NAL unit is Annex-B
//     framed.
// ===========================================================================

TEST(VideoPipeline, ReceiverStapAIsAnnexBFramed) {
    ReceiverConfig rc;
    rc.codec        = video_frame::CodecKind::kH264;
    rc.ssrc         = kSsrc;
    rc.payload_type = kPt;

    VideoReceiver rx(rc);
    FrameRecorder rec;
    rx.set_frame_callback(rec.as_callback());

    const std::uint8_t sps[] = {0x67, 0x42, 0x00, 0x1E};
    const std::uint8_t pps[] = {0x68, 0xCE, 0x38, 0x80};

    std::uint8_t stap[64];
    core::ByteSpan nalus[2] = {{sps, sizeof(sps)}, {pps, sizeof(pps)}};
    const std::size_t n = build_stap_a(nalus, 2, {stap, sizeof(stap)});
    ASSERT_GT(n, 0u);

    auto p = make_rtp(100, 6000, true, kPt, kSsrc, {stap, n});
    EXPECT_TRUE(rx.push_rtp(parse_rtp(p), 1000));

    auto recs = rec.snapshot();
    ASSERT_EQ(recs.size(), 1u);

    std::vector<std::uint8_t> expected;
    expected.insert(expected.end(), kStartCode, kStartCode + sizeof(kStartCode));
    expected.insert(expected.end(), sps, sps + sizeof(sps));
    expected.insert(expected.end(), kStartCode, kStartCode + sizeof(kStartCode));
    expected.insert(expected.end(), pps, pps + sizeof(pps));

    ASSERT_EQ(recs[0].frame.payload.size(), expected.size());
    EXPECT_EQ(std::memcmp(recs[0].frame.payload.data(),
                          expected.data(), expected.size()), 0);
}

// ===========================================================================
// 2d. Receiver: H.265 single NAL unit packet -> Annex-B framed.
//
// RFC 7798 §4.3.1: the Single NAL unit packet carries the two-byte NAL
// unit header, so the re-framing must cover *both* header bytes.
// ===========================================================================

TEST(VideoPipeline, ReceiverH265SingleNalIsAnnexBFramed) {
    ReceiverConfig rc;
    rc.codec        = video_frame::CodecKind::kH265;
    rc.ssrc         = kSsrc;
    rc.payload_type = kPt;

    VideoReceiver rx(rc);
    FrameRecorder rec;
    rx.set_frame_callback(rec.as_callback());

    // IDR_W_RADL (type 19): byte0 = F(0) | Type(19) | LayerId MSB(0)
    //                       byte1 = LayerId low(0) | TID(1)
    const std::uint8_t nalu[] = {0x26, 0x01, 0xAA, 0xBB};

    auto p = make_rtp(100, 7000, true, kPt, kSsrc, {nalu, sizeof(nalu)});
    EXPECT_TRUE(rx.push_rtp(parse_rtp(p), 1000));

    auto recs = rec.snapshot();
    ASSERT_EQ(recs.size(), 1u);

    // RFC 7798 §4.3.1 carries a bare NAL unit in the RTP payload too, and
    // the re-framing must cover *both* header bytes -- so the receiver
    // puts the start code back, exactly like the H.264 path.
    const std::vector<std::uint8_t> nalu_vec(nalu, nalu + sizeof(nalu));
    const auto expected = annex_b(nalu_vec);
    ASSERT_EQ(recs[0].frame.payload.size(), expected.size());
    EXPECT_EQ(std::memcmp(recs[0].frame.payload.data(),
                          expected.data(), expected.size()), 0);
    EXPECT_EQ(recs[0].frame.codec, video_frame::CodecKind::kH265);
    EXPECT_EQ(recs[0].frame.nalu_format,
              video_frame::NaluFormat::kAnnexBStartCode);
}

// ===========================================================================
// 2e. Receiver: H.265 AP aggregation -> every unpacked NAL unit carries
//     its own Annex-B start code.
// ===========================================================================

TEST(VideoPipeline, ReceiverH265ApIsAnnexBFramed) {
    ReceiverConfig rc;
    rc.codec        = video_frame::CodecKind::kH265;
    rc.ssrc         = kSsrc;
    rc.payload_type = kPt;

    VideoReceiver rx(rc);
    FrameRecorder rec;
    rx.set_frame_callback(rec.as_callback());

    // VPS (type 32) and SPS (type 33), each with its two-byte header.
    const std::uint8_t vps[] = {0x40, 0x00, 0x0C};
    const std::uint8_t sps[] = {0x42, 0x00, 0x01};
    std::vector<h265::Nalu> nalus;
    nalus.push_back({h265::NaluType::kVPS, {vps, sizeof(vps)}, false});
    nalus.push_back({h265::NaluType::kSPS, {sps, sizeof(sps)}, false});

    std::uint8_t ap[64] = {0};
    const std::size_t n = h265::build_ap(nalus, /*layer_id=*/0, /*tid=*/1,
                                         {ap, sizeof(ap)});
    ASSERT_GT(n, 0u);

    auto p = make_rtp(100, 8000, true, kPt, kSsrc, {ap, n});
    EXPECT_TRUE(rx.push_rtp(parse_rtp(p), 1000));

    auto recs = rec.snapshot();
    ASSERT_EQ(recs.size(), 1u);

    std::vector<std::uint8_t> expected;
    expected.insert(expected.end(), kStartCode, kStartCode + sizeof(kStartCode));
    expected.insert(expected.end(), vps, vps + sizeof(vps));
    expected.insert(expected.end(), kStartCode, kStartCode + sizeof(kStartCode));
    expected.insert(expected.end(), sps, sps + sizeof(sps));

    ASSERT_EQ(recs[0].frame.payload.size(), expected.size());
    EXPECT_EQ(std::memcmp(recs[0].frame.payload.data(),
                          expected.data(), expected.size()), 0);
}

// ===========================================================================
// 3. Receiver: seq gaps -> NACK list contains missing seqs.
// ===========================================================================

TEST(VideoPipeline, ReceiverNackOnGap) {
    ReceiverConfig rc;
    rc.codec        = video_frame::CodecKind::kH264;
    rc.ssrc         = kSsrc;
    rc.payload_type = kPt;

    VideoReceiver rx(rc);
    FrameRecorder rec;
    rx.set_frame_callback(rec.as_callback());

    std::uint8_t n1[] = {0x65, 0xAA};
    std::uint8_t n3[] = {0xCC, 0xDD};   // seq 101 + 102 missing

    auto p1 = make_rtp(100, 3000, true, kPt, kSsrc, {n1, sizeof(n1)});
    auto p3 = make_rtp(103, 3000, true, kPt, kSsrc, {n3, sizeof(n3)});

    EXPECT_TRUE(rx.push_rtp(parse_rtp(p1), 1000));
    EXPECT_TRUE(rx.push_rtp(parse_rtp(p3), 1200));

    auto nacks = rx.pop_nack_entries();
    std::sort(nacks.begin(), nacks.end());
    // We expect: 101, 102.
    EXPECT_GE(nacks.size(), 2u);
    EXPECT_EQ(nacks[0], 101u);
    EXPECT_EQ(nacks[1], 102u);
    EXPECT_EQ(rx.stats().sequence_gaps_seen, 2u);
}

// ===========================================================================
// 4. Receiver: tick past max_wait_ms -> incomplete frame emitted.
// ===========================================================================

TEST(VideoPipeline, ReceiverTimeoutIncomplete) {
    ReceiverConfig rc;
    rc.codec                  = video_frame::CodecKind::kH264;
    rc.ssrc                   = kSsrc;
    rc.payload_type           = kPt;
    rc.jitter_buffer_config.max_wait_ms = 50;

    VideoReceiver rx(rc);
    FrameRecorder rec;
    rx.set_frame_callback(rec.as_callback());

    std::uint8_t n[] = {0x41, 0xAA};    // P-frame (no marker -> incomplete)
    auto p = make_rtp(100, 4000, false, kPt, kSsrc, {n, sizeof(n)});
    EXPECT_TRUE(rx.push_rtp(parse_rtp(p), 0));

    rx.tick(/*now_us=*/60 * 1000);

    auto recs = rec.snapshot();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_FALSE(recs[0].frame.is_keyframe);
    EXPECT_GE(rx.stats().frames_incomplete, 1u);
}

// ===========================================================================
// 5. Receiver: reset() clears state, new RTP ts can be processed.
// ===========================================================================

TEST(VideoPipeline, ReceiverReset) {
    ReceiverConfig rc;
    rc.codec        = video_frame::CodecKind::kH264;
    rc.ssrc         = kSsrc;
    rc.payload_type = kPt;

    VideoReceiver rx(rc);
    FrameRecorder rec;
    rx.set_frame_callback(rec.as_callback());

    std::uint8_t n1[] = {0x65, 0xAA};
    auto p1 = make_rtp(100, 1000, true, kPt, kSsrc, {n1, sizeof(n1)});
    EXPECT_TRUE(rx.push_rtp(parse_rtp(p1), 1000));
    ASSERT_EQ(rec.snapshot().size(), 1u);

    rx.reset();
    EXPECT_EQ(rx.stats().packets_pushed, 0u);
    EXPECT_EQ(rx.stats().frames_emitted, 0u);

    std::uint8_t n2[] = {0x65, 0xBB};
    auto p2 = make_rtp(200, 5000, true, kPt, kSsrc, {n2, sizeof(n2)});
    EXPECT_TRUE(rx.push_rtp(parse_rtp(p2), 5000));
    EXPECT_EQ(rec.snapshot().size(), 2u);
    EXPECT_EQ(rec.snapshot()[1].frame.info.rtp_timestamp, 5000u);
}

// ===========================================================================
// 6. Receiver: VP8 -- push 3 partition packets, callback fires once.
// ===========================================================================

TEST(VideoPipeline, ReceiverVP8Partition) {
    ReceiverConfig rc;
    rc.codec        = video_frame::CodecKind::kVP8;
    rc.ssrc         = kSsrc;
    rc.payload_type = 96;        // WebRTC VP8 default

    VideoReceiver rx(rc);
    FrameRecorder rec;
    rx.set_frame_callback(rec.as_callback());

    // Build three VP8 payloads (descriptor + bitstream).  We use the
    // minimal base descriptor (1 byte): partition_index + S bit + N bit.
    auto build_vp8_pkt = [](std::uint8_t pid, bool start,
                            core::ByteSpan bs) {
        std::uint8_t desc = static_cast<std::uint8_t>(
            (start ? 0x10 : 0) | (pid & 0x07));
        std::vector<std::uint8_t> out;
        out.reserve(1 + bs.size());
        out.push_back(desc);
        out.insert(out.end(), bs.begin(), bs.end());
        return out;
    };

    std::uint8_t bs0[] = {0x9D, 0x01, 0x2A};   // VP8 keyframe tag
    std::uint8_t bs1[] = {0xAA, 0xBB};
    std::uint8_t bs2[] = {0xCC, 0xDD};

    auto p0 = build_vp8_pkt(0, /*start=*/true,  {bs0, sizeof(bs0)});
    auto p1 = build_vp8_pkt(1, /*start=*/false, {bs1, sizeof(bs1)});
    auto p2 = build_vp8_pkt(2, /*start=*/false, {bs2, sizeof(bs2)});

    auto r0 = make_rtp(100, 7000, false, 96, kSsrc, {p0.data(), p0.size()});
    auto r1 = make_rtp(101, 7000, false, 96, kSsrc, {p1.data(), p1.size()});
    auto r2 = make_rtp(102, 7000, true,  96, kSsrc, {p2.data(), p2.size()});

    EXPECT_TRUE(rx.push_rtp(parse_rtp(r0), 1000));
    EXPECT_TRUE(rx.push_rtp(parse_rtp(r1), 1100));
    EXPECT_TRUE(rx.push_rtp(parse_rtp(r2), 1200));

    auto recs = rec.snapshot();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].frame.codec, video_frame::CodecKind::kVP8);
    EXPECT_TRUE(recs[0].frame.is_keyframe);
}

// ===========================================================================
// 7. Sender: small NAL -> single-NAL packet.
// ===========================================================================

TEST(VideoPipeline, SenderSingleNal) {
    SenderConfig sc;
    sc.codec        = video_frame::CodecKind::kH264;
    sc.ssrc         = kSsrc;
    sc.payload_type = kPt;
    sc.mtu          = 1200;

    VideoSender tx(sc);
    PacketRecorder pr;
    tx.set_packet_callback(pr.as_callback());

    // Build a small IDR NALU with Annex B framing.
    std::uint8_t nalu[] = {0x65, 0xAA, 0xBB, 0xCC};
    std::vector<std::uint8_t> ann;
    ann.insert(ann.end(), {0x00, 0x00, 0x00, 0x01});
    ann.insert(ann.end(), nalu, nalu + sizeof(nalu));

    video_frame::EncodedVideoFrame f{};
    f.codec        = video_frame::CodecKind::kH264;
    f.nalu_format  = video_frame::NaluFormat::kAnnexBStartCode;
    f.payload      = core::ByteSpan{ann.data(), ann.size()};
    f.is_keyframe  = true;

    auto r = tx.push_frame(f, /*capture_ts_us=*/1000,
                           /*rtp_ts=*/1000, /*frame_seq=*/1);
    ASSERT_TRUE(r.ok());

    auto pkts = pr.snapshot();
    ASSERT_EQ(pkts.size(), 1u);
    EXPECT_EQ(pkts[0].payload.size(), sizeof(nalu));
    EXPECT_EQ(std::memcmp(pkts[0].payload.data(), nalu, sizeof(nalu)), 0);
    EXPECT_TRUE(pkts[0].marker);
    EXPECT_EQ(pkts[0].seq, sc.initial_seq);
    EXPECT_EQ(pkts[0].rtp_ts, 1000u);
    EXPECT_EQ(tx.stats().single_nal_frames_packetized, 1u);
}

// ===========================================================================
// 8. Sender: two small NALUs -> STAP-A packet.
// ===========================================================================

TEST(VideoPipeline, SenderStapA) {
    SenderConfig sc;
    sc.codec              = video_frame::CodecKind::kH264;
    sc.ssrc               = kSsrc;
    sc.payload_type       = kPt;
    sc.mtu                = 1200;
    sc.packetization_mode = PacketizationMode::kNonInterleaved;

    VideoSender tx(sc);
    PacketRecorder pr;
    tx.set_packet_callback(pr.as_callback());

    std::uint8_t s[]  = {0x67, 0xAA};   // SPS
    std::uint8_t p[]  = {0x68, 0xBB};   // PPS
    std::vector<std::uint8_t> ann;
    ann.insert(ann.end(), {0x00, 0x00, 0x00, 0x01});
    ann.insert(ann.end(), s, s + sizeof(s));
    ann.insert(ann.end(), {0x00, 0x00, 0x00, 0x01});
    ann.insert(ann.end(), p, p + sizeof(p));

    video_frame::EncodedVideoFrame f{};
    f.codec        = video_frame::CodecKind::kH264;
    f.nalu_format  = video_frame::NaluFormat::kAnnexBStartCode;
    f.payload      = core::ByteSpan{ann.data(), ann.size()};
    f.is_keyframe  = true;

    ASSERT_TRUE(tx.push_frame(f, 0, 1000, 1).ok());

    auto pkts = pr.snapshot();
    // We expect ONE STAP-A packet (small NALs fit together).
    ASSERT_GE(pkts.size(), 1u);
    // Check the first packet is STAP-A: indicator byte type=24.
    EXPECT_EQ(pkts[0].payload[0] & 0x1F, 24);
    EXPECT_TRUE(pkts[0].marker);
    EXPECT_GE(tx.stats().stapa_frames_packetized, 1u);
}

// ===========================================================================
// 9. Sender: NALU > MTU -> FU-A with multiple fragments.
// ===========================================================================

TEST(VideoPipeline, SenderFuA) {
    SenderConfig sc;
    sc.codec        = video_frame::CodecKind::kH264;
    sc.ssrc         = kSsrc;
    sc.payload_type = kPt;
    sc.mtu          = 100;          // tiny MTU to force fragmentation

    VideoSender tx(sc);
    PacketRecorder pr;
    tx.set_packet_callback(pr.as_callback());

    // Original NALU = 1 byte header + 250 bytes body = 251 bytes total.
    // With mtu=100, FU-A fragment body size = 98 bytes.  Number of
    // fragments = ceil(250/98) = 3.
    std::vector<std::uint8_t> ann;
    std::vector<std::uint8_t> nalu;
    nalu.push_back(0x65);        // IDR header
    for (int i = 0; i < 250; ++i) nalu.push_back(static_cast<std::uint8_t>(i));

    ann.insert(ann.end(), {0x00, 0x00, 0x00, 0x01});
    ann.insert(ann.end(), nalu.begin(), nalu.end());

    video_frame::EncodedVideoFrame f{};
    f.codec        = video_frame::CodecKind::kH264;
    f.nalu_format  = video_frame::NaluFormat::kAnnexBStartCode;
    f.payload      = core::ByteSpan{ann.data(), ann.size()};

    ASSERT_TRUE(tx.push_frame(f, 0, 1000, 1).ok());

    auto pkts = pr.snapshot();
    EXPECT_GE(pkts.size(), 3u);
    // Verify each packet is FU-A (type=28 in indicator).
    for (const auto& p : pkts) {
        ASSERT_GE(p.payload.size(), 2u);
        EXPECT_EQ(p.payload[0] & 0x1F, 28);
    }
    // Only the last fragment has marker=true.
    for (std::size_t i = 0; i + 1 < pkts.size(); ++i) {
        EXPECT_FALSE(pkts[i].marker);
    }
    EXPECT_TRUE(pkts.back().marker);
    EXPECT_EQ(tx.stats().fua_frames_packetized, 1u);
}

// ===========================================================================
// 10. Sender: RTP timestamp is the caller-supplied value (advance test).
// ===========================================================================

TEST(VideoPipeline, SenderRtpTimestampAdvancing) {
    SenderConfig sc;
    sc.codec        = video_frame::CodecKind::kH264;
    sc.ssrc         = kSsrc;
    sc.payload_type = kPt;
    sc.mtu          = 1200;

    VideoSender tx(sc);
    PacketRecorder pr;
    tx.set_packet_callback(pr.as_callback());

    std::uint8_t nalu[] = {0x65, 0x01};
    std::vector<std::uint8_t> ann;
    ann.insert(ann.end(), {0x00, 0x00, 0x00, 0x01});
    ann.insert(ann.end(), nalu, nalu + sizeof(nalu));

    video_frame::EncodedVideoFrame f{};
    f.codec       = video_frame::CodecKind::kH264;
    f.nalu_format = video_frame::NaluFormat::kAnnexBStartCode;
    f.payload     = core::ByteSpan{ann.data(), ann.size()};

    constexpr std::uint32_t kFps   = 30;
    constexpr std::uint32_t kStep  = 90000 / kFps;   // 3000

    std::uint32_t ts = 1000;
    for (int frame = 0; frame < 5; ++frame) {
        ASSERT_TRUE(tx.push_frame(f, /*cap_us=*/frame * 33'333,
                                  /*rtp_ts=*/ts,
                                  /*frame_seq=*/static_cast<std::uint32_t>(frame))
                        .ok());
        ts = ts + kStep;        // simulate caller increment
    }

    auto pkts = pr.snapshot();
    ASSERT_EQ(pkts.size(), 5u);
    for (std::size_t i = 0; i < pkts.size(); ++i) {
        EXPECT_EQ(pkts[i].rtp_ts, 1000u + static_cast<std::uint32_t>(i) * kStep);
    }
}

// ===========================================================================
// 11. Sender: seq numbers are monotonically increasing.
// ===========================================================================

TEST(VideoPipeline, SenderMonotonicSeq) {
    SenderConfig sc;
    sc.codec        = video_frame::CodecKind::kH264;
    sc.ssrc         = kSsrc;
    sc.payload_type = kPt;
    sc.mtu          = 80;          // tiny MTU -> multi-fragment FU-A
    sc.initial_seq   = 50000;

    VideoSender tx(sc);
    PacketRecorder pr;
    tx.set_packet_callback(pr.as_callback());

    std::vector<std::uint8_t> ann;
    ann.insert(ann.end(), {0x00, 0x00, 0x00, 0x01});
    std::vector<std::uint8_t> nalu;
    nalu.push_back(0x65);
    for (int i = 0; i < 400; ++i) nalu.push_back(static_cast<std::uint8_t>(i));
    ann.insert(ann.end(), nalu.begin(), nalu.end());

    video_frame::EncodedVideoFrame f{};
    f.codec       = video_frame::CodecKind::kH264;
    f.nalu_format = video_frame::NaluFormat::kAnnexBStartCode;
    f.payload     = core::ByteSpan{ann.data(), ann.size()};

    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(tx.push_frame(f, 0,
                                  static_cast<std::uint32_t>(1000 + i * 3000),
                                  static_cast<std::uint32_t>(i))
                        .ok());
    }

    auto pkts = pr.snapshot();
    ASSERT_GE(pkts.size(), 3u);
    for (std::size_t i = 1; i < pkts.size(); ++i) {
        // seq must strictly increase (wrap is allowed).
        EXPECT_EQ(static_cast<std::uint16_t>(pkts[i - 1].seq + 1),
                  pkts[i].seq);
    }
}

// ===========================================================================
// 12. Sender: force_keyframe() sets the flag, stats reflect it on next push.
// ===========================================================================

TEST(VideoPipeline, SenderForceKeyframe) {
    SenderConfig sc;
    sc.codec        = video_frame::CodecKind::kH264;
    sc.ssrc         = kSsrc;
    sc.payload_type = kPt;
    sc.mtu          = 1200;

    VideoSender tx(sc);
    PacketRecorder pr;
    tx.set_packet_callback(pr.as_callback());

    tx.force_keyframe();
    EXPECT_EQ(tx.stats().force_keyframe_calls, 1u);
    EXPECT_EQ(tx.stats().frames_marked_as_keyframe, 0u);

    std::uint8_t nalu[] = {0x65, 0xAA};
    std::vector<std::uint8_t> ann;
    ann.insert(ann.end(), {0x00, 0x00, 0x00, 0x01});
    ann.insert(ann.end(), nalu, nalu + sizeof(nalu));

    video_frame::EncodedVideoFrame f{};
    f.codec       = video_frame::CodecKind::kH264;
    f.nalu_format = video_frame::NaluFormat::kAnnexBStartCode;
    f.payload     = core::ByteSpan{ann.data(), ann.size()};
    f.is_keyframe = true;        // caller responsibility

    ASSERT_TRUE(tx.push_frame(f, 0, 1000, 1).ok());
    EXPECT_EQ(tx.stats().frames_marked_as_keyframe, 1u);
}

// ===========================================================================
// 13. End-to-end: Sender -> Receiver (H.264, single NAL, MTU large).
// ===========================================================================

TEST(VideoPipeline, EndToEndSingleNal) {
    // Sender with large MTU so it stays single-NAL.
    SenderConfig sc;
    sc.codec        = video_frame::CodecKind::kH264;
    sc.ssrc         = kSsrc;
    sc.payload_type = kPt;
    sc.mtu          = 1200;

    VideoSender tx(sc);
    PacketRecorder pr;
    tx.set_packet_callback(pr.as_callback());

    // Random-looking IDR NALU.
    std::vector<std::uint8_t> nalu = {0x65, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    std::vector<std::uint8_t> ann;
    ann.insert(ann.end(), {0x00, 0x00, 0x00, 0x01});
    ann.insert(ann.end(), nalu.begin(), nalu.end());

    video_frame::EncodedVideoFrame f{};
    f.codec        = video_frame::CodecKind::kH264;
    f.nalu_format  = video_frame::NaluFormat::kAnnexBStartCode;
    f.payload      = core::ByteSpan{ann.data(), ann.size()};
    f.is_keyframe  = true;

    ASSERT_TRUE(tx.push_frame(f, /*cap=*/1000, /*rtp_ts=*/90000,
                              /*frame_seq=*/1)
                    .ok());

    auto sent = pr.snapshot();
    ASSERT_EQ(sent.size(), 1u);

    // Now feed the same packet to a Receiver.
    ReceiverConfig rc;
    rc.codec        = video_frame::CodecKind::kH264;
    rc.ssrc         = kSsrc;
    rc.payload_type = kPt;
    VideoReceiver rx(rc);
    FrameRecorder rec;
    rx.set_frame_callback(rec.as_callback());

    auto rtp_bytes = make_rtp(sent[0].seq, sent[0].rtp_ts, sent[0].marker,
                              kPt, kSsrc,
                              core::ByteSpan{sent[0].payload.data(),
                                             sent[0].payload.size()});
    auto view = parse_rtp(rtp_bytes);
    EXPECT_TRUE(rx.push_rtp(view, /*now_us=*/1000));

    auto recs = rec.snapshot();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].frame.codec, video_frame::CodecKind::kH264);
    // The sender strips the start code (RFC 6184 §5.1 carries bare NAL
    // units in the RTP payload); the receiver puts it back, because the
    // frame contract is Annex-B.
    const auto expected = annex_b(nalu);
    ASSERT_EQ(recs[0].frame.payload.size(), expected.size());
    EXPECT_EQ(std::memcmp(recs[0].frame.payload.data(),
                          expected.data(), expected.size()), 0);
    EXPECT_TRUE(recs[0].frame.is_keyframe);
}

// ===========================================================================
// 14. End-to-end: Sender -> Receiver (H.264, FU-A required).
// ===========================================================================

TEST(VideoPipeline, EndToEndFuA) {
    SenderConfig sc;
    sc.codec        = video_frame::CodecKind::kH264;
    sc.ssrc         = kSsrc;
    sc.payload_type = kPt;
    sc.mtu          = 80;          // tiny -> forces fragmentation

    VideoSender tx(sc);
    PacketRecorder pr;
    tx.set_packet_callback(pr.as_callback());

    // Original NALU = 1 byte header + 250 bytes body.
    std::vector<std::uint8_t> nalu;
    nalu.push_back(0x65);
    for (int i = 0; i < 250; ++i) {
        nalu.push_back(static_cast<std::uint8_t>(i ^ 0xA5));
    }
    std::vector<std::uint8_t> ann;
    ann.insert(ann.end(), {0x00, 0x00, 0x00, 0x01});
    ann.insert(ann.end(), nalu.begin(), nalu.end());

    video_frame::EncodedVideoFrame f{};
    f.codec       = video_frame::CodecKind::kH264;
    f.nalu_format = video_frame::NaluFormat::kAnnexBStartCode;
    f.payload     = core::ByteSpan{ann.data(), ann.size()};
    f.is_keyframe = true;

    ASSERT_TRUE(tx.push_frame(f, 0, 90000, 1).ok());

    auto sent = pr.snapshot();
    ASSERT_GE(sent.size(), 2u);

    ReceiverConfig rc;
    rc.codec        = video_frame::CodecKind::kH264;
    rc.ssrc         = kSsrc;
    rc.payload_type = kPt;
    VideoReceiver rx(rc);
    FrameRecorder rec;
    rx.set_frame_callback(rec.as_callback());

    // Feed each emitted packet to the receiver in order.
    std::uint32_t arrival_us = 0;
    for (const auto& p : sent) {
        auto rtp_bytes = make_rtp(p.seq, p.rtp_ts, p.marker, kPt, kSsrc,
                                  core::ByteSpan{p.payload.data(),
                                                 p.payload.size()});
        auto view = parse_rtp(rtp_bytes);
        EXPECT_TRUE(rx.push_rtp(view, arrival_us));
        arrival_us += 1000;
    }

    auto recs = rec.snapshot();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].frame.codec, video_frame::CodecKind::kH264);
    const auto expected = annex_b(nalu);
    ASSERT_EQ(recs[0].frame.payload.size(), expected.size());
    EXPECT_EQ(std::memcmp(recs[0].frame.payload.data(),
                          expected.data(), expected.size()), 0);
    EXPECT_TRUE(recs[0].frame.is_keyframe);
    EXPECT_EQ(rx.stats().fua_fragments_seen, sent.size());
}
// ===========================================================================
// 15. Receiver: feedback callback reports the lost sequence numbers.
// ===========================================================================

TEST(VideoPipeline, ReceiverFeedbackReportsMissingPackets) {
    ReceiverConfig rc;
    rc.codec        = video_frame::CodecKind::kH264;
    rc.ssrc         = kSsrc;
    rc.payload_type = kPt;

    VideoReceiver rx(rc);

    std::vector<vp::ReceiverFeedback> reports;
    rx.set_feedback_callback([&reports](const vp::ReceiverFeedback& fb) {
        reports.push_back(fb);
    });

    std::uint8_t n1[] = {0x65, 0xAA};
    std::uint8_t n4[] = {0xCC, 0xDD};    // seq 101..103 missing

    auto p1 = make_rtp(100, 5000, true, kPt, kSsrc, {n1, sizeof(n1)});
    auto p4 = make_rtp(104, 5000, true, kPt, kSsrc, {n4, sizeof(n4)});

    EXPECT_TRUE(rx.push_rtp(parse_rtp(p1), 1000));
    EXPECT_TRUE(rx.push_rtp(parse_rtp(p4), 1200));

    ASSERT_EQ(reports.size(), 1u);
    ASSERT_EQ(reports[0].missing_packets.size(), 3u);
    EXPECT_EQ(reports[0].missing_packets[0], 101u);
    EXPECT_EQ(reports[0].missing_packets[1], 102u);
    EXPECT_EQ(reports[0].missing_packets[2], 103u);
    EXPECT_FALSE(reports[0].keyframe_requested);
}

// ===========================================================================
// 16. Receiver: an incomplete frame raises a key-frame request.
// ===========================================================================

TEST(VideoPipeline, ReceiverFeedbackRequestsKeyframe) {
    ReceiverConfig rc;
    rc.codec                           = video_frame::CodecKind::kH264;
    rc.ssrc                            = kSsrc;
    rc.payload_type                    = kPt;
    rc.jitter_buffer_config.max_wait_ms = 50;

    VideoReceiver rx(rc);

    std::vector<vp::ReceiverFeedback> reports;
    rx.set_feedback_callback([&reports](const vp::ReceiverFeedback& fb) {
        reports.push_back(fb);
    });

    std::uint8_t n[] = {0x41, 0xAA};    // P-frame without marker
    auto p = make_rtp(100, 6000, false, kPt, kSsrc, {n, sizeof(n)});
    EXPECT_TRUE(rx.push_rtp(parse_rtp(p), 0));

    rx.tick(/*now_us=*/60 * 1000);

    bool saw_keyframe_request = false;
    for (const auto& fb : reports) {
        if (fb.keyframe_requested) saw_keyframe_request = true;
    }
    EXPECT_TRUE(saw_keyframe_request);
    EXPECT_GE(rx.stats().keyframe_requests_emitted, 1u);
}

// ===========================================================================
// 17. Sender: retransmission cache answers a NACK with the same packet.
// ===========================================================================

TEST(VideoPipeline, SenderRetransmitFromCache) {
    SenderConfig sc;
    sc.codec        = video_frame::CodecKind::kH264;
    sc.ssrc         = kSsrc;
    sc.payload_type = kPt;
    sc.initial_seq  = 500;
    sc.retransmit_cache_packets = 64;

    VideoSender tx(sc);
    PacketRecorder rec;
    tx.set_packet_callback(rec.as_callback());

    std::uint8_t nalu[] = {0x65, 0xAA, 0xBB};
    std::vector<std::uint8_t> ann;
    ann.insert(ann.end(), {0x00, 0x00, 0x00, 0x01});
    ann.insert(ann.end(), nalu, nalu + sizeof(nalu));

    video_frame::EncodedVideoFrame frame{};
    frame.codec       = video_frame::CodecKind::kH264;
    frame.nalu_format = video_frame::NaluFormat::kAnnexBStartCode;
    frame.payload     = core::ByteSpan{ann.data(), ann.size()};

    auto ok = tx.push_frame(frame, /*capture_ts_us=*/1000,
                            /*rtp_timestamp=*/9000, /*frame_seq=*/1);
    EXPECT_TRUE(ok.ok());

    auto sent = rec.snapshot();
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].seq, 500u);
    EXPECT_EQ(tx.cached_packet_count(), 1u);

    // The NACK asks for seq 500 again -> the very same packet is re-sent.
    EXPECT_TRUE(tx.retransmit(500u));
    auto after = rec.snapshot();
    ASSERT_EQ(after.size(), 2u);
    EXPECT_EQ(after[1].seq, 500u);
    EXPECT_EQ(after[1].rtp_ts, sent[0].rtp_ts);
    EXPECT_EQ(after[1].marker, sent[0].marker);
    EXPECT_EQ(after[1].payload, sent[0].payload);
    EXPECT_EQ(tx.stats().packets_retransmitted, 1u);

    // An unknown sequence number is a miss, not a crash.
    EXPECT_FALSE(tx.retransmit(1234u));
    EXPECT_EQ(tx.stats().packets_retransmit_missed, 1u);
}

// ===========================================================================
// 18. Sender: force_keyframe() arms the one-shot key-frame marker.
// ===========================================================================

TEST(VideoPipeline, SenderKeyframePendingIsOneShot) {
    SenderConfig sc;
    sc.codec        = video_frame::CodecKind::kH264;
    sc.ssrc         = kSsrc;
    sc.payload_type = kPt;

    VideoSender tx(sc);
    EXPECT_FALSE(tx.keyframe_pending());

    tx.force_keyframe();
    EXPECT_TRUE(tx.keyframe_pending());
    EXPECT_EQ(tx.stats().force_keyframe_calls, 1u);

    std::uint8_t nalu[] = {0x65, 0xAA, 0xBB};
    std::vector<std::uint8_t> ann;
    ann.insert(ann.end(), {0x00, 0x00, 0x00, 0x01});
    ann.insert(ann.end(), nalu, nalu + sizeof(nalu));

    video_frame::EncodedVideoFrame frame{};
    frame.codec       = video_frame::CodecKind::kH264;
    frame.nalu_format = video_frame::NaluFormat::kAnnexBStartCode;
    frame.payload     = core::ByteSpan{ann.data(), ann.size()};

    auto ok = tx.push_frame(frame, 1000, 9000, 1);
    EXPECT_TRUE(ok.ok());
    EXPECT_FALSE(tx.keyframe_pending());
    EXPECT_EQ(tx.stats().frames_marked_as_keyframe, 1u);
}

// ===========================================================================
// 19. Sender: the marker bit marks the LAST packet of the frame, not the
//     last packet of every NAL group.
//
// The regression this guards is the one that broke the hardware path
// end-to-end: an NVENC IDR frame carries `SPS | PPS | <oversized IDR
// slice>`.  The SPS and the PPS are aggregated into one STAP-A packet,
// the slice is fragmented into FU-A packets.  A marker on the STAP-A made
// the receiver believe the frame was complete after "SPS + PPS", so the
// IDR slice that followed was discarded and the decoder never saw a
// picture.
// ===========================================================================

TEST(VideoPipeline, SenderMarkerOnlyOnLastPacketOfFrame) {
    SenderConfig sc;
    sc.codec        = video_frame::CodecKind::kH264;
    sc.ssrc         = kSsrc;
    sc.payload_type = kPt;
    sc.mtu          = 1200;

    VideoSender tx(sc);
    PacketRecorder pr;
    tx.set_packet_callback(pr.as_callback());

    // SPS (small) + PPS (small) + an IDR slice that exceeds the MTU.
    std::vector<std::uint8_t> sps = {0x67, 0x42, 0x00, 0x1E};
    std::vector<std::uint8_t> pps = {0x68, 0xCE, 0x38, 0x80};
    std::vector<std::uint8_t> slice;
    slice.push_back(0x65);                       // IDR header
    for (int i = 0; i < 4000; ++i) {
        slice.push_back(static_cast<std::uint8_t>(i & 0xFF));
    }

    std::vector<std::uint8_t> ann;
    const std::uint8_t sc4[4] = {0x00, 0x00, 0x00, 0x01};
    ann.insert(ann.end(), sc4, sc4 + 4);
    ann.insert(ann.end(), sps.begin(), sps.end());
    ann.insert(ann.end(), sc4, sc4 + 4);
    ann.insert(ann.end(), pps.begin(), pps.end());
    ann.insert(ann.end(), sc4, sc4 + 4);
    ann.insert(ann.end(), slice.begin(), slice.end());

    video_frame::EncodedVideoFrame f{};
    f.codec        = video_frame::CodecKind::kH264;
    f.nalu_format  = video_frame::NaluFormat::kAnnexBStartCode;
    f.payload      = core::ByteSpan{ann.data(), ann.size()};
    f.is_keyframe  = true;

    ASSERT_TRUE(tx.push_frame(f, 1000, 90000, 1).ok());

    auto pkts = pr.snapshot();
    // The frame is a STAP-A (SPS + PPS) followed by several FU-A fragments.
    ASSERT_GE(pkts.size(), 2u);
    // The first packet is the STAP-A (indicator type 24) and it is NOT
    // the last packet of the frame, so it must NOT carry the marker.
    EXPECT_EQ(pkts[0].payload[0] & 0x1F, 24);
    EXPECT_FALSE(pkts[0].marker);
    // Only the very last packet carries the marker.
    for (std::size_t i = 0; i + 1 < pkts.size(); ++i) {
        EXPECT_FALSE(pkts[i].marker);
    }
    EXPECT_TRUE(pkts.back().marker);
}

// ===========================================================================
// 20. End-to-end: the NVENC-shaped IDR frame (SPS | PPS | big slice)
//     reaches the receiver as ONE complete frame that contains the slice.
// ===========================================================================

TEST(VideoPipeline, EndToEndNvEncShapedIdrFrame) {
    SenderConfig sc;
    sc.codec        = video_frame::CodecKind::kH264;
    sc.ssrc         = kSsrc;
    sc.payload_type = kPt;
    sc.mtu          = 1200;

    VideoSender tx(sc);
    PacketRecorder pr;
    tx.set_packet_callback(pr.as_callback());

    std::vector<std::uint8_t> sps = {0x67, 0x42, 0x00, 0x1E};
    std::vector<std::uint8_t> pps = {0x68, 0xCE, 0x38, 0x80};
    std::vector<std::uint8_t> slice;
    slice.push_back(0x65);
    for (int i = 0; i < 4000; ++i) {
        slice.push_back(static_cast<std::uint8_t>(i & 0xFF));
    }

    std::vector<std::uint8_t> ann;
    const std::uint8_t sc4[4] = {0x00, 0x00, 0x00, 0x01};
    ann.insert(ann.end(), sc4, sc4 + 4);
    ann.insert(ann.end(), sps.begin(), sps.end());
    ann.insert(ann.end(), sc4, sc4 + 4);
    ann.insert(ann.end(), pps.begin(), pps.end());
    ann.insert(ann.end(), sc4, sc4 + 4);
    ann.insert(ann.end(), slice.begin(), slice.end());

    video_frame::EncodedVideoFrame f{};
    f.codec        = video_frame::CodecKind::kH264;
    f.nalu_format  = video_frame::NaluFormat::kAnnexBStartCode;
    f.payload      = core::ByteSpan{ann.data(), ann.size()};
    f.is_keyframe  = true;

    ASSERT_TRUE(tx.push_frame(f, 1000, 90000, 1).ok());
    auto sent = pr.snapshot();
    ASSERT_GE(sent.size(), 2u);

    ReceiverConfig rc;
    rc.codec        = video_frame::CodecKind::kH264;
    rc.ssrc         = kSsrc;
    rc.payload_type = kPt;
    VideoReceiver rx(rc);
    FrameRecorder rec;
    rx.set_frame_callback(rec.as_callback());

    std::uint32_t arrival_us = 0;
    for (const auto& p : sent) {
        auto rtp_bytes = make_rtp(p.seq, p.rtp_ts, p.marker, kPt, kSsrc,
                                  core::ByteSpan{p.payload.data(),
                                                 p.payload.size()});
        EXPECT_TRUE(rx.push_rtp(parse_rtp(rtp_bytes), arrival_us));
        arrival_us += 1000;
    }

    // Exactly one frame, and it carries the whole picture: the SPS, the
    // PPS *and* the IDR slice.  Before the marker fix the receiver
    // emitted "SPS + PPS" as the frame and threw the slice away.
    auto recs = rec.snapshot();
    ASSERT_EQ(recs.size(), 1u);

    std::vector<std::uint8_t> expected;
    expected.insert(expected.end(), sc4, sc4 + 4);
    expected.insert(expected.end(), sps.begin(), sps.end());
    expected.insert(expected.end(), sc4, sc4 + 4);
    expected.insert(expected.end(), pps.begin(), pps.end());
    expected.insert(expected.end(), sc4, sc4 + 4);
    expected.insert(expected.end(), slice.begin(), slice.end());

    ASSERT_EQ(recs[0].frame.payload.size(), expected.size());
    EXPECT_EQ(std::memcmp(recs[0].frame.payload.data(),
                          expected.data(), expected.size()), 0);
    EXPECT_TRUE(recs[0].frame.is_keyframe);
}

// ===========================================================================
// 21. Sender: the H.265 marker bit marks the LAST packet of the frame.
//
// This is the H.264 regression of test 19, restated for RFC 7798.  The
// NVENC HEVC output has exactly the same shape as the H.264 one: an IDR
// frame that carries `VPS | SPS | PPS | <oversized IDR slice>`.  The three
// parameter sets are small, so they are aggregated into one AP packet, and
// the slice is fragmented into FU packets.
//
// Before the fix the AP packet carried the marker unconditionally, so the
// receiver's jitter buffer saw the frame as complete after "VPS + SPS +
// PPS", emitted that as the picture, and dropped the IDR slice that
// followed.  The decoder therefore never saw a decodable picture.
//
// The FU path had the mirror-image bug: the marker was set on the last
// fragment of *every* fragmented NAL unit, not only on the frame's last
// NAL unit.
// ===========================================================================

TEST(VideoPipeline, SenderH265MarkerOnlyOnLastPacketOfFrame) {
    SenderConfig sc;
    sc.codec        = video_frame::CodecKind::kH265;
    sc.ssrc         = kSsrc;
    sc.payload_type = kPt;
    sc.mtu          = 1200;

    VideoSender tx(sc);
    PacketRecorder pr;
    tx.set_packet_callback(pr.as_callback());

    // VPS (type 32) / SPS (type 33) / PPS (type 34), each with its
    // two-byte NAL unit header, plus an IDR slice (type 19) that exceeds
    // the MTU and is therefore fragmented.
    //
    // The two-byte header layout is RFC 7798 §1.1.4:
    //   byte 0: F(1) | Type(6) | LayerId MSB(1)
    //   byte 1: LayerId low 5 bits(5) | TID(3)
    const std::uint8_t vps[]   = {0x40, 0x00, 0x0C, 0x01};   // type 32
    const std::uint8_t sps[]   = {0x42, 0x00, 0x01, 0x02};   // type 33
    const std::uint8_t pps[]   = {0x44, 0x00, 0x03, 0x04};   // type 34
    std::vector<std::uint8_t> slice;
    slice.push_back(0x26);                       // IDR_W_RADL (type 19)
    slice.push_back(0x01);
    for (int i = 0; i < 4000; ++i) {
        slice.push_back(static_cast<std::uint8_t>(i & 0xFF));
    }

    std::vector<std::uint8_t> ann;
    const std::uint8_t sc4[4] = {0x00, 0x00, 0x00, 0x01};
    ann.insert(ann.end(), sc4, sc4 + 4);
    ann.insert(ann.end(), vps, vps + sizeof(vps));
    ann.insert(ann.end(), sc4, sc4 + 4);
    ann.insert(ann.end(), sps, sps + sizeof(sps));
    ann.insert(ann.end(), sc4, sc4 + 4);
    ann.insert(ann.end(), pps, pps + sizeof(pps));
    ann.insert(ann.end(), sc4, sc4 + 4);
    ann.insert(ann.end(), slice.begin(), slice.end());

    video_frame::EncodedVideoFrame f{};
    f.codec        = video_frame::CodecKind::kH265;
    f.nalu_format  = video_frame::NaluFormat::kAnnexBStartCode;
    f.payload      = core::ByteSpan{ann.data(), ann.size()};
    f.is_keyframe  = true;

    ASSERT_TRUE(tx.push_frame(f, 1000, 90000, 1).ok());

    auto pkts = pr.snapshot();
    // The frame is an AP (VPS + SPS + PPS) followed by several FU
    // fragments of the IDR slice.
    ASSERT_GE(pkts.size(), 2u);

    // The first packet is the AP aggregation packet (payload type 48).
    EXPECT_EQ(static_cast<int>(h265::peek_payload_type(
                  core::ByteSpan{pkts[0].payload.data(),
                                 pkts[0].payload.size()})),
              48);
    // It is NOT the last packet of the frame, so it must NOT carry the
    // marker.  Before the fix it always did.
    EXPECT_FALSE(pkts[0].marker);

    // Only the very last packet carries the marker.
    for (std::size_t i = 0; i + 1 < pkts.size(); ++i) {
        EXPECT_FALSE(pkts[i].marker);
    }
    EXPECT_TRUE(pkts.back().marker);
}

// ===========================================================================
// 22. End-to-end: the NVENC-shaped H.265 IDR frame (VPS | SPS | PPS | big
//     slice) reaches the receiver as ONE complete frame that contains the
//     slice.
//
// This is the test that actually failed on the hardware path: the frame
// the receiver got was "VPS + SPS + PPS" and the IDR slice was gone.
// ===========================================================================

TEST(VideoPipeline, EndToEndNvEncShapedH265IdrFrame) {
    SenderConfig sc;
    sc.codec        = video_frame::CodecKind::kH265;
    sc.ssrc         = kSsrc;
    sc.payload_type = kPt;
    sc.mtu          = 1200;

    VideoSender tx(sc);
    PacketRecorder pr;
    tx.set_packet_callback(pr.as_callback());

    const std::uint8_t vps[] = {0x40, 0x00, 0x0C, 0x01};
    const std::uint8_t sps[] = {0x42, 0x00, 0x01, 0x02};
    const std::uint8_t pps[] = {0x44, 0x00, 0x03, 0x04};
    std::vector<std::uint8_t> slice;
    slice.push_back(0x26);                       // IDR_W_RADL (type 19)
    slice.push_back(0x01);
    for (int i = 0; i < 4000; ++i) {
        slice.push_back(static_cast<std::uint8_t>(i & 0xFF));
    }

    std::vector<std::uint8_t> ann;
    const std::uint8_t sc4[4] = {0x00, 0x00, 0x00, 0x01};
    ann.insert(ann.end(), sc4, sc4 + 4);
    ann.insert(ann.end(), vps, vps + sizeof(vps));
    ann.insert(ann.end(), sc4, sc4 + 4);
    ann.insert(ann.end(), sps, sps + sizeof(sps));
    ann.insert(ann.end(), sc4, sc4 + 4);
    ann.insert(ann.end(), pps, pps + sizeof(pps));
    ann.insert(ann.end(), sc4, sc4 + 4);
    ann.insert(ann.end(), slice.begin(), slice.end());

    video_frame::EncodedVideoFrame f{};
    f.codec        = video_frame::CodecKind::kH265;
    f.nalu_format  = video_frame::NaluFormat::kAnnexBStartCode;
    f.payload      = core::ByteSpan{ann.data(), ann.size()};
    f.is_keyframe  = true;

    ASSERT_TRUE(tx.push_frame(f, 1000, 90000, 1).ok());
    auto sent = pr.snapshot();
    ASSERT_GE(sent.size(), 2u);

    ReceiverConfig rc;
    rc.codec        = video_frame::CodecKind::kH265;
    rc.ssrc         = kSsrc;
    rc.payload_type = kPt;
    VideoReceiver rx(rc);
    FrameRecorder rec;
    rx.set_frame_callback(rec.as_callback());

    std::uint32_t arrival_us = 0;
    for (const auto& p : sent) {
        auto rtp_bytes = make_rtp(p.seq, p.rtp_ts, p.marker, kPt, kSsrc,
                                  core::ByteSpan{p.payload.data(),
                                                 p.payload.size()});
        EXPECT_TRUE(rx.push_rtp(parse_rtp(rtp_bytes), arrival_us));
        arrival_us += 1000;
    }

    // Exactly one frame, and it carries the whole picture: the VPS, the
    // SPS, the PPS *and* the IDR slice.  Before the marker fix the
    // receiver emitted "VPS + SPS + PPS" as the frame and threw the slice
    // away.
    auto recs = rec.snapshot();
    ASSERT_EQ(recs.size(), 1u);

    std::vector<std::uint8_t> expected;
    expected.insert(expected.end(), sc4, sc4 + 4);
    expected.insert(expected.end(), vps, vps + sizeof(vps));
    expected.insert(expected.end(), sc4, sc4 + 4);
    expected.insert(expected.end(), sps, sps + sizeof(sps));
    expected.insert(expected.end(), sc4, sc4 + 4);
    expected.insert(expected.end(), pps, pps + sizeof(pps));
    expected.insert(expected.end(), sc4, sc4 + 4);
    expected.insert(expected.end(), slice.begin(), slice.end());

    ASSERT_EQ(recs[0].frame.payload.size(), expected.size());
    EXPECT_EQ(std::memcmp(recs[0].frame.payload.data(),
                          expected.data(), expected.size()), 0);
    EXPECT_TRUE(recs[0].frame.is_keyframe);
}

// ===========================================================================
// 23. Sender: a NAL unit that fits neither the AP budget nor the FU
//     threshold is still emitted.
//
// `use_fua` fires only above the MTU, but an aggregated NAL unit costs a
// two-byte length prefix on top of its own bytes, so a NAL unit whose
// size lies in `(mtu - 2, mtu]` fits a Single NAL unit packet and yet
// does not fit an AP packet.  Such a unit used to be dropped on the
// floor: the aggregation walk produced an empty group and the loop
// advanced past the unit without emitting it.
//
// The result was a *corrupt* frame: the parameter sets went out in the AP
// packet and the slice never went out at all.
// ===========================================================================

TEST(VideoPipeline, SenderH265SingleNalInTheApBudgetGapIsNotDropped) {
    SenderConfig sc;
    sc.codec        = video_frame::CodecKind::kH265;
    sc.ssrc         = kSsrc;
    sc.payload_type = kPt;
    sc.mtu          = 1200;

    VideoSender tx(sc);
    PacketRecorder pr;
    tx.set_packet_callback(pr.as_callback());

    // A NAL unit of exactly `mtu` bytes: it does not fit an AP packet
    // (which needs two extra bytes for the length prefix) but it is not
    // larger than the MTU, so it is not fragmented either.
    const std::size_t nalu_size = 1200;
    std::vector<std::uint8_t> nalu(nalu_size);
    nalu[0] = 0x26;                              // IDR_W_RADL (type 19)
    nalu[1] = 0x01;
    for (std::size_t i = 2; i < nalu.size(); ++i) {
        nalu[i] = static_cast<std::uint8_t>(i & 0xFF);
    }

    std::vector<std::uint8_t> ann;
    const std::uint8_t sc4[4] = {0x00, 0x00, 0x00, 0x01};
    ann.insert(ann.end(), sc4, sc4 + 4);
    ann.insert(ann.end(), nalu.begin(), nalu.end());

    video_frame::EncodedVideoFrame f{};
    f.codec        = video_frame::CodecKind::kH265;
    f.nalu_format  = video_frame::NaluFormat::kAnnexBStartCode;
    f.payload      = core::ByteSpan{ann.data(), ann.size()};
    f.is_keyframe  = true;

    ASSERT_TRUE(tx.push_frame(f, 1000, 90000, 1).ok());

    auto pkts = pr.snapshot();
    // Exactly one packet: the NAL unit as a Single NAL unit packet.
    ASSERT_EQ(pkts.size(), 1u);
    EXPECT_EQ(pkts[0].payload.size(), nalu.size());
    EXPECT_EQ(std::memcmp(pkts[0].payload.data(), nalu.data(),
                          nalu.size()), 0);
    // It is the frame's only packet, so it carries the marker.
    EXPECT_TRUE(pkts[0].marker);
}
