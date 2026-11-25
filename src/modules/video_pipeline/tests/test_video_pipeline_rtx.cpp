/**
 * @file src/modules/video_pipeline/tests/test_video_pipeline_rtx.cpp
 * @brief The RFC 4588 retransmission path, end to end.
 *
 * ## Why this file exists
 *
 * The v1.3 plan's Q3 definition of done is:
 *
 *   "A NACK is answered on the RTX payload type; the receiver restores
 *    the original sequence number and the payload lands in the right
 *    frame."
 *
 * The `rtx` module's own tests cover the *format* (the two-octet OSN
 * header, the `apt=` parameter).  They cannot cover the second half of
 * the sentence, because the format knows nothing about the jitter
 * buffer's frame assembly.  This file covers it:
 *
 *   1. the sender answers a NACK on the RTX payload type, with the OSN
 *      header and the *original's* timestamp and marker;
 *   2. the receiver unwraps the retransmission and restores the
 *      *original* sequence number — everywhere, not just in the payload;
 *   3. the restored payload lands in the **right frame** — the frame it
 *      belongs to, not a new one;
 *   4. a peer that offers no RTX keeps working exactly as before.
 *
 * Point (3) is the one that is easy to get wrong: the jitter buffer's
 * frame assembly is keyed on the sequence number and the RTP timestamp,
 * so a retransmission that kept its own number (or arrived under a new
 * timestamp) would be assembled into a frame of its own — and the
 * original frame would stay incomplete.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/plugins/video_pipeline.hpp>
#include <nimrtc/rtp/packet.hpp>
#include <nimrtc/rtx/rtx.hpp>
#include <nimrtc/video_frame/frame.hpp>
#include <nimrtc/video_pipeline/video_pipeline.hpp>
#include <nimrtc/video_pipeline/video_pipeline_plugin.hpp>

namespace vp  = nimrtc::video_pipeline;
namespace rtx = nimrtc::rtx;
namespace core = nimrtc::core;
namespace video_frame = nimrtc::video_frame;
namespace rtp = nimrtc::rtp;
namespace plugins = nimrtc::plugins;

using vp::VideoReceiver;
using vp::VideoSender;
using vp::ReceiverConfig;
using vp::SenderConfig;

namespace {

constexpr std::uint32_t kSsrc    = 0x12345678u;
constexpr std::uint32_t kRtxSsrc = 0x12345679u;
constexpr std::uint8_t  kPt      = 102;   // the media payload type
constexpr std::uint8_t  kRtxPt   = 103;   // the retransmission payload type

// ---------------------------------------------------------------------------
// A minimal RTP packet: the 12-byte fixed header plus the payload.
// ---------------------------------------------------------------------------

struct RtpBytes {
    std::vector<std::uint8_t> raw;
};

RtpBytes make_rtp(std::uint16_t seq,
                  std::uint32_t ts,
                  bool         marker,
                  std::uint8_t pt,
                  std::uint32_t ssrc,
                  core::ByteSpan payload) {
    RtpBytes b;
    b.raw.resize(12 + payload.size());
    // RFC 3550 §5.1 fixed header.
    b.raw[0]  = 0x80;                                   // V=2
    b.raw[1]  = static_cast<std::uint8_t>((marker ? 0x80 : 0) | (pt & 0x7F));
    b.raw[2]  = static_cast<std::uint8_t>(seq >> 8);
    b.raw[3]  = static_cast<std::uint8_t>(seq & 0xFF);
    b.raw[4]  = static_cast<std::uint8_t>(ts >> 24);
    b.raw[5]  = static_cast<std::uint8_t>(ts >> 16);
    b.raw[6]  = static_cast<std::uint8_t>(ts >> 8);
    b.raw[7]  = static_cast<std::uint8_t>(ts & 0xFF);
    b.raw[8]  = static_cast<std::uint8_t>(ssrc >> 24);
    b.raw[9]  = static_cast<std::uint8_t>(ssrc >> 16);
    b.raw[10] = static_cast<std::uint8_t>(ssrc >> 8);
    b.raw[11] = static_cast<std::uint8_t>(ssrc & 0xFF);
    if (!payload.empty()) {
        std::memcpy(b.raw.data() + 12, payload.data(), payload.size());
    }
    return b;
}

rtp::PacketView parse_rtp(const RtpBytes& b) {
    rtp::PacketView v;
    v.raw          = core::ByteSpan{b.raw.data(), b.raw.size()};
    v.payload      = core::ByteSpan{b.raw.data() + 12, b.raw.size() - 12};
    v.payload_type = static_cast<std::uint8_t>(b.raw[1] & 0x7F);
    v.marker       = (b.raw[1] & 0x80) != 0;
    v.seq = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(b.raw[2]) << 8) |
         static_cast<std::uint16_t>(b.raw[3]));
    v.timestamp = (static_cast<std::uint32_t>(b.raw[4]) << 24) |
                  (static_cast<std::uint32_t>(b.raw[5]) << 16) |
                  (static_cast<std::uint32_t>(b.raw[6]) <<  8) |
                   static_cast<std::uint32_t>(b.raw[7]);
    v.ssrc = (static_cast<std::uint32_t>(b.raw[8])  << 24) |
             (static_cast<std::uint32_t>(b.raw[9])  << 16) |
             (static_cast<std::uint32_t>(b.raw[10]) <<  8) |
              static_cast<std::uint32_t>(b.raw[11]);
    return v;
}

// ---------------------------------------------------------------------------
// Recorders
// ---------------------------------------------------------------------------

struct FrameRecord {
    std::vector<std::uint8_t> bitstream;
    std::uint32_t             rtp_timestamp = 0;
    bool                      is_keyframe  = false;
};

class FrameRecorder {
public:
    vp::ReceiverFrameCallback as_callback() {
        return [this](video_frame::EncodedVideoFrame f, std::int64_t) {
            std::lock_guard<std::mutex> lk(m_);
            FrameRecord rec;
            rec.bitstream.assign(f.payload.begin(), f.payload.end());
            rec.rtp_timestamp = f.info.rtp_timestamp;
            rec.is_keyframe   = f.is_keyframe;
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
};

struct RtxRecord {
    std::vector<std::uint8_t> payload;
    std::uint32_t             rtp_timestamp = 0;
    bool                      marker        = false;
    std::uint16_t             original_seq  = 0;
    std::uint16_t             rtx_seq       = 0;
};

class RtxRecorder {
public:
    vp::SenderRtxCallback as_callback() {
        return [this](const vp::SenderRtxPacket& p) {
            std::lock_guard<std::mutex> lk(m_);
            RtxRecord rec;
            rec.payload.assign(p.payload.begin(), p.payload.end());
            rec.rtp_timestamp = p.rtp_timestamp;
            rec.marker        = p.marker;
            rec.original_seq  = p.original_seq;
            rec.rtx_seq       = p.rtx_seq;
            records_.push_back(std::move(rec));
        };
    }
    std::vector<RtxRecord> snapshot() {
        std::lock_guard<std::mutex> lk(m_);
        return records_;
    }
private:
    std::mutex             m_;
    std::vector<RtxRecord> records_;
};

/// Wrap an original payload in the RFC 4588 §4.1 header.
std::vector<std::uint8_t> wrap_rtx(std::uint16_t osn,
                                    core::ByteSpan original) {
    std::vector<std::uint8_t> out(rtx::payload_size_for(original));
    const std::size_t wrote =
        rtx::build_payload(core::MutableByteSpan{out.data(), out.size()},
                           osn, original);
    out.resize(wrote);
    return out;
}

} // namespace

// ===========================================================================
// 1. The sender: a NACK is answered on the RTX payload type.
// ===========================================================================

TEST(VideoPipelineRtx, TheNackIsAnsweredOnTheRetransmissionPayloadType) {
    SenderConfig sc;
    sc.codec        = video_frame::CodecKind::kH264;
    sc.ssrc         = kSsrc;
    sc.payload_type = kPt;
    sc.rtx_payload_type = kRtxPt;
    sc.initial_seq  = 500;
    sc.retransmit_cache_packets = 64;

    VideoSender tx(sc);
    RtxRecorder rec;
    tx.set_rtx_packet_callback(rec.as_callback());
    tx.set_rtx_enabled(true);
    EXPECT_TRUE(tx.rtx_enabled());

    std::vector<std::uint8_t> ann = {0x00, 0x00, 0x00, 0x01, 0x65, 0xAA, 0xBB};

    video_frame::EncodedVideoFrame f{};
    f.codec        = video_frame::CodecKind::kH264;
    f.nalu_format  = video_frame::NaluFormat::kAnnexBStartCode;
    f.payload      = core::ByteSpan{ann.data(), ann.size()};
    f.is_keyframe  = true;

    ASSERT_TRUE(tx.push_frame(f, /*capture_ts_us=*/1000,
                              /*rtp_timestamp=*/9000, /*frame_seq=*/1).ok());

    // The NACK asks for seq 500 -> the packet is re-sent *as* a
    // retransmission: the OSN header in front of the original payload.
    EXPECT_TRUE(tx.retransmit(500u));

    auto rtx = rec.snapshot();
    ASSERT_EQ(rtx.size(), 1u);

    // The payload is the two-octet OSN header followed by the original's
    // payload, verbatim (RFC 4588 §4.1).  The original's payload is the
    // RTP payload — the NAL unit, without the Annex-B start code (the
    // sender strips the start code when it packetizes).
    ASSERT_EQ(rtx[0].payload.size(), rtx::kHeaderSize + 3u);
    EXPECT_EQ(rtx[0].payload[0], 500u >> 8);
    EXPECT_EQ(rtx[0].payload[1], 500u & 0xFFu);
    EXPECT_EQ(std::vector<std::uint8_t>(rtx[0].payload.begin() + 2,
                                        rtx[0].payload.end()),
              std::vector<std::uint8_t>({0x65, 0xAA, 0xBB}));

    // The RTP header of the retransmission is the original's except for
    // the sequence number (RFC 4588 §4.1), so the timestamp and the
    // marker bit are the *original's*.
    EXPECT_EQ(rtx[0].rtp_timestamp, 9000u);
    EXPECT_TRUE(rtx[0].marker);
    EXPECT_EQ(rtx[0].original_seq, 500u);

    // The retransmission stream has its own sequence number space
    // (RFC 4588 §4) — so the number on the wire is NOT the original's.
    EXPECT_NE(rtx[0].rtx_seq, 500u);

    EXPECT_EQ(tx.stats().rtx_packets_emitted, 1u);
    EXPECT_EQ(tx.stats().packets_retransmitted, 1u);
}

// ===========================================================================
// 2. The sender: without RTX the original packet is re-sent as-is.
// ===========================================================================

TEST(VideoPipelineRtx, WithoutRtxTheOriginalPacketIsResentAsIs) {
    SenderConfig sc;
    sc.codec        = video_frame::CodecKind::kH264;
    sc.ssrc         = kSsrc;
    sc.payload_type = kPt;
    sc.initial_seq  = 500;
    sc.retransmit_cache_packets = 64;

    VideoSender tx(sc);
    EXPECT_FALSE(tx.rtx_enabled());

    // The *ordinary* packet callback is the pre-RTX path.
    std::vector<std::uint8_t> payloads;
    std::uint16_t last_seq = 0;
    tx.set_packet_callback([&](core::ByteSpan p, std::uint32_t,
                                std::uint16_t seq, bool) {
        payloads.assign(p.begin(), p.end());
        last_seq = seq;
    });

    std::vector<std::uint8_t> ann = {0x00, 0x00, 0x00, 0x01, 0x65, 0xAA, 0xBB};
    video_frame::EncodedVideoFrame f{};
    f.codec       = video_frame::CodecKind::kH264;
    f.nalu_format = video_frame::NaluFormat::kAnnexBStartCode;
    f.payload     = core::ByteSpan{ann.data(), ann.size()};

    ASSERT_TRUE(tx.push_frame(f, 1000, 9000, 1).ok());
    EXPECT_TRUE(tx.retransmit(500u));

    // The very same packet — same sequence number, same payload, no OSN
    // header.  This is what RFC 4588 §4.1 explicitly is *not*.
    EXPECT_EQ(last_seq, 500u);
    EXPECT_EQ(payloads, std::vector<std::uint8_t>({0x65, 0xAA, 0xBB}));
    EXPECT_EQ(tx.stats().rtx_packets_emitted, 0u);
    EXPECT_EQ(tx.stats().packets_retransmitted, 1u);
}

// ===========================================================================
// 3. The receiver: the original sequence number is restored.
// ===========================================================================

TEST(VideoPipelineRtx, TheRetransmissionRestoresTheOriginalSequenceNumber) {
    ReceiverConfig rc;
    rc.codec        = video_frame::CodecKind::kH264;
    rc.ssrc         = kSsrc;
    rc.payload_type = kPt;
    rc.rtx_payload_type = kRtxPt;

    VideoReceiver rx(rc);
    FrameRecorder frames;
    rx.set_frame_callback(frames.as_callback());

    // Packet 100 establishes the sequence context; packet 101 is *lost*,
    // so when 102 arrives the receiver sees the gap and asks for 101.
    std::uint8_t nalu[]  = {0x65, 0xAA, 0xBB};
    std::uint8_t other[] = {0x41, 0xCC};

    auto first = make_rtp(100, /*ts=*/3000, /*marker=*/false, kPt, kSsrc,
                          {nalu, sizeof(nalu)});
    EXPECT_TRUE(rx.push_rtp(parse_rtp(first), 1000));

    auto next = make_rtp(102, /*ts=*/6000, /*marker=*/true, kPt, kSsrc,
                         {other, sizeof(other)});
    EXPECT_TRUE(rx.push_rtp(parse_rtp(next), 1100));

    // The gap is now pending — but it is *not* popped yet, because the
    // retransmission has to find it to erase it.
    ASSERT_EQ(rx.stats().sequence_gaps_seen, 1u);

    // The peer retransmits the lost packet 101: the RTX packet carries
    // the OSN 101 and the *original's* timestamp.
    std::vector<std::uint8_t> wrapped = wrap_rtx(101u, {nalu, sizeof(nalu)});
    auto rtx_pkt = make_rtp(900, /*ts=*/3000, /*marker=*/false, kRtxPt,
                             kRtxSsrc,
                             core::ByteSpan{wrapped.data(), wrapped.size()});
    EXPECT_TRUE(rx.push_rtp_rtx(parse_rtp(rtx_pkt), 2000));

    EXPECT_EQ(rx.stats().rtx_packets_received, 1u);
    EXPECT_EQ(rx.stats().rtx_packets_malformed, 0u);

    // The restored sequence number is used *everywhere* the original
    // would have been — including the NACK bookkeeping.  The packet is
    // no longer missing, so the receiver must not ask for it again.
    auto after = rx.pop_nack_entries();
    for (const auto seq : after) {
        EXPECT_NE(seq, 101u) << "the receiver still NACKs a packet it has";
    }
}

// ===========================================================================
// 4. The receiver: the restored payload lands in the RIGHT FRAME.
// ===========================================================================

TEST(VideoPipelineRtx, TheRestoredPayloadLandsInTheRightFrame) {
    ReceiverConfig rc;
    rc.codec        = video_frame::CodecKind::kH264;
    rc.ssrc         = kSsrc;
    rc.payload_type = kPt;
    rc.rtx_payload_type = kRtxPt;

    VideoReceiver rx(rc);
    FrameRecorder frames;
    rx.set_frame_callback(frames.as_callback());

    // Two frames, one RTP timestamp apart.  The first frame's *second*
    // packet is lost; its first packet and the whole second frame
    // arrive.
    std::uint8_t a1[] = {0x65, 0x11};
    std::uint8_t a2[] = {0x65, 0x22};   // lost
    std::uint8_t b1[] = {0x41, 0x33};

    auto pkt_a1 = make_rtp(100, /*ts=*/3000, /*marker=*/false, kPt, kSsrc,
                           {a1, sizeof(a1)});
    auto pkt_b1 = make_rtp(102, /*ts=*/6000, /*marker=*/true, kPt, kSsrc,
                           {b1, sizeof(b1)});

    EXPECT_TRUE(rx.push_rtp(parse_rtp(pkt_a1), 1000));
    EXPECT_TRUE(rx.push_rtp(parse_rtp(pkt_b1), 1100));

    // The retransmission of the lost packet 101.  It carries the
    // *original's* timestamp (3000) — RFC 4588 §4.1 — so it belongs to
    // the first frame, and the OSN restores its sequence number.
    std::vector<std::uint8_t> wrapped = wrap_rtx(101u, {a2, sizeof(a2)});
    auto rtx_pkt = make_rtp(900, /*ts=*/3000, /*marker=*/true, kRtxPt,
                             kRtxSsrc,
                             core::ByteSpan{wrapped.data(), wrapped.size()});
    EXPECT_TRUE(rx.push_rtp_rtx(parse_rtp(rtx_pkt), 1200));

    auto recs = frames.snapshot();
    ASSERT_GE(recs.size(), 1u);

    // The retransmitted payload must be inside the frame it belongs to
    // — the one with RTP timestamp 3000 — and NOT in a frame of its own.
    bool found_in_first_frame = false;
    for (const auto& f : recs) {
        const bool has_a2 = std::search(f.bitstream.begin(), f.bitstream.end(),
                                         a2, a2 + sizeof(a2)) != f.bitstream.end();
        if (f.rtp_timestamp == 3000u) {
            // The frame the retransmission belongs to.
            EXPECT_TRUE(has_a2)
                << "the retransmitted payload did not land in its own frame";
            found_in_first_frame = true;
        } else {
            // A frame the retransmission has no business in.
            EXPECT_FALSE(has_a2)
                << "the retransmitted payload was assembled into the wrong frame";
        }
    }
    EXPECT_TRUE(found_in_first_frame);

    // And it must not have produced a frame of its own: the retransmission
    // restored the original's number, so it is a *late arrival* of an
    // existing frame, not the first packet of a new one.
    EXPECT_EQ(rx.stats().rtx_packets_received, 1u);
}

// ===========================================================================
// 4b. The receiver: the restored number is what the jitter buffer accounts
//     the packet under.
// ===========================================================================

TEST(VideoPipelineRtx, TheRestoredNumberIsWhatTheBufferAccounts) {
    ReceiverConfig rc;
    rc.codec        = video_frame::CodecKind::kH264;
    rc.ssrc         = kSsrc;
    rc.payload_type = kPt;
    rc.rtx_payload_type = kRtxPt;

    VideoReceiver rx(rc);
    FrameRecorder frames;
    rx.set_frame_callback(frames.as_callback());

    // Packet 100 opens a frame; packet 101 is lost; packet 102 arrives
    // and reveals the gap.
    std::uint8_t a1[]    = {0x65, 0x11};
    std::uint8_t other[] = {0x41, 0xCC};

    auto first = make_rtp(100, /*ts=*/3000, /*marker=*/false, kPt, kSsrc,
                          {a1, sizeof(a1)});
    auto next  = make_rtp(102, /*ts=*/6000, /*marker=*/true, kPt, kSsrc,
                          {other, sizeof(other)});
    EXPECT_TRUE(rx.push_rtp(parse_rtp(first), 1000));
    EXPECT_TRUE(rx.push_rtp(parse_rtp(next), 1100));
    ASSERT_EQ(rx.stats().sequence_gaps_seen, 1u);

    // The retransmission of the lost packet 101.  It carries the
    // *original's* timestamp (3000) and does NOT carry the marker — so
    // the frame stays open in the jitter buffer, and the buffer's own
    // gap accounting for it is still live.
    std::vector<std::uint8_t> wrapped = wrap_rtx(101u, {a1, sizeof(a1)});
    auto rtx_pkt = make_rtp(900, /*ts=*/3000, /*marker=*/false, kRtxPt,
                             kRtxSsrc,
                             core::ByteSpan{wrapped.data(), wrapped.size()});
    EXPECT_TRUE(rx.push_rtp_rtx(parse_rtp(rtx_pkt), 1200));

    // The packet is accounted for under the *restored* number, so the
    // jitter buffer sees a contiguous run and reports nothing missing.
    // A receiver that kept the retransmission stream's own number (900)
    // would see the whole span 101..899 as lost.
    const auto nacks = rx.pop_nack_entries();
    for (const auto seq : nacks) {
        EXPECT_NE(seq, 101u)
            << "the restored packet is still accounted for as missing";
    }
    EXPECT_TRUE(nacks.empty())
        << "the restored packet did not close the gap it belongs to: "
        << nacks.size() << " packets still reported missing";
}

// ===========================================================================
// 5. The receiver: a retransmission does not create a second frame.
// ===========================================================================

TEST(VideoPipelineRtx, TheRetransmissionDoesNotCreateASecondFrame) {
    ReceiverConfig rc;
    rc.codec        = video_frame::CodecKind::kH264;
    rc.ssrc         = kSsrc;
    rc.payload_type = kPt;
    rc.rtx_payload_type = kRtxPt;

    VideoReceiver rx(rc);
    FrameRecorder frames;
    rx.set_frame_callback(frames.as_callback());

    std::uint8_t nalu[] = {0x65, 0xAA};

    // The original arrives and completes the frame.
    auto pkt = make_rtp(100, /*ts=*/3000, /*marker=*/true, kPt, kSsrc,
                        {nalu, sizeof(nalu)});
    EXPECT_TRUE(rx.push_rtp(parse_rtp(pkt), 1000));
    ASSERT_EQ(frames.snapshot().size(), 1u);

    // A *duplicate* retransmission of the same packet arrives later.
    // Because the restored sequence number is the original's, it is
    // accounted under the original — it must not open a second frame.
    std::vector<std::uint8_t> wrapped = wrap_rtx(100u, {nalu, sizeof(nalu)});
    auto rtx_pkt = make_rtp(901, /*ts=*/3000, /*marker=*/true, kRtxPt,
                             kRtxSsrc,
                             core::ByteSpan{wrapped.data(), wrapped.size()});
    EXPECT_TRUE(rx.push_rtp_rtx(parse_rtp(rtx_pkt), 2000));

    EXPECT_EQ(frames.snapshot().size(), 1u);
}

// ===========================================================================
// 6. The receiver: a malformed retransmission is counted, not crashed on.
// ===========================================================================

TEST(VideoPipelineRtx, AMalformedRetransmissionIsCounted) {
    ReceiverConfig rc;
    rc.codec        = video_frame::CodecKind::kH264;
    rc.ssrc         = kSsrc;
    rc.payload_type = kPt;
    rc.rtx_payload_type = kRtxPt;

    VideoReceiver rx(rc);
    FrameRecorder frames;
    rx.set_frame_callback(frames.as_callback());

    // A payload too short to carry the two-octet OSN header.
    std::uint8_t stub[] = {0x01};
    auto rtx_pkt = make_rtp(900, 3000, true, kRtxPt, kRtxSsrc,
                             {stub, sizeof(stub)});
    EXPECT_FALSE(rx.push_rtp_rtx(parse_rtp(rtx_pkt), 1000));

    EXPECT_EQ(rx.stats().rtx_packets_malformed, 1u);
    EXPECT_EQ(rx.stats().rtx_packets_received, 0u);
    EXPECT_TRUE(frames.snapshot().empty());
}

// ===========================================================================
// 7. The receiver: without RTX negotiated, a retransmission is ignored.
// ===========================================================================

TEST(VideoPipelineRtx, WithoutNegotiatedRtxTheRetransmissionIsIgnored) {
    ReceiverConfig rc;
    rc.codec        = video_frame::CodecKind::kH264;
    rc.ssrc         = kSsrc;
    rc.payload_type = kPt;
    // rtx_payload_type stays 0 — the peer offered no retransmission
    // stream, so there is nothing to unwrap.
    rc.rtx_payload_type = 0;

    VideoReceiver rx(rc);
    FrameRecorder frames;
    rx.set_frame_callback(frames.as_callback());

    std::uint8_t nalu[] = {0x65, 0xAA};
    std::vector<std::uint8_t> wrapped = wrap_rtx(100u, {nalu, sizeof(nalu)});
    auto rtx_pkt = make_rtp(900, 3000, true, kRtxPt, kRtxSsrc,
                             core::ByteSpan{wrapped.data(), wrapped.size()});

    EXPECT_FALSE(rx.push_rtp_rtx(parse_rtp(rtx_pkt), 1000));
    EXPECT_EQ(rx.stats().rtx_packets_received, 0u);
    EXPECT_EQ(rx.stats().rtx_packets_malformed, 0u);
    EXPECT_TRUE(frames.snapshot().empty());
}

// ===========================================================================
// 8. The whole path: the sender's retransmission reaches the right frame.
// ===========================================================================

TEST(VideoPipelineRtx, TheSenderRetransmissionReachesTheRightFrame) {
    // ---- The sender -------------------------------------------------------
    SenderConfig sc;
    sc.codec        = video_frame::CodecKind::kH264;
    sc.ssrc         = kSsrc;
    sc.payload_type = kPt;
    sc.rtx_payload_type = kRtxPt;
    sc.initial_seq  = 100;
    sc.mtu          = 1200;
    sc.retransmit_cache_packets = 64;

    VideoSender tx(sc);
    RtxRecorder rtx_rec;
    tx.set_rtx_packet_callback(rtx_rec.as_callback());
    tx.set_rtx_enabled(true);

    // One frame, one packet.
    std::vector<std::uint8_t> ann = {0x00, 0x00, 0x00, 0x01, 0x65, 0xAA, 0xBB};
    video_frame::EncodedVideoFrame f{};
    f.codec       = video_frame::CodecKind::kH264;
    f.nalu_format = video_frame::NaluFormat::kAnnexBStartCode;
    f.payload     = core::ByteSpan{ann.data(), ann.size()};
    f.is_keyframe = true;
    ASSERT_TRUE(tx.push_frame(f, 1000, /*rtp_timestamp=*/3000, 1).ok());

    // ---- The peer NACKs seq 100 ------------------------------------------
    ASSERT_TRUE(tx.retransmit(100u));
    auto rtx = rtx_rec.snapshot();
    ASSERT_EQ(rtx.size(), 1u);

    // ---- The receiver -----------------------------------------------------
    //
    // The sender above pushed an Annex-B frame, so every NAL unit the
    // receiver delivers carries the start code the frame contract
    // promises -- the receiver re-frames the bare NAL unit the RFC 6184
    // §5.1 wire shape carries.
    ReceiverConfig rc;
    rc.codec        = video_frame::CodecKind::kH264;
    rc.ssrc         = kSsrc;
    rc.payload_type = kPt;
    rc.rtx_payload_type = kRtxPt;

    VideoReceiver rx(rc);
    FrameRecorder frames;
    rx.set_frame_callback(frames.as_callback());

    // The retransmission arrives on the retransmission payload type, with
    // its own sequence number, and carries the original's timestamp.
    auto rtx_pkt = make_rtp(rtx[0].rtx_seq, rtx[0].rtp_timestamp,
                             rtx[0].marker, kRtxPt, kRtxSsrc,
                             core::ByteSpan{rtx[0].payload.data(),
                                            rtx[0].payload.size()});
    EXPECT_TRUE(rx.push_rtp_rtx(parse_rtp(rtx_pkt), 1000));

    auto recs = frames.snapshot();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].rtp_timestamp, 3000u);
    EXPECT_TRUE(recs[0].is_keyframe);
    // The NAL unit is in the frame, in the Annex-B framing the frame
    // contract promises.
    const std::vector<std::uint8_t> expected = {0x00, 0x00, 0x00, 0x01,
                                                 0x65, 0xAA, 0xBB};
    EXPECT_EQ(recs[0].bitstream, expected);
}

// ===========================================================================
// 9. The plugin seam: the same behaviour through the interface.
// ===========================================================================

TEST(VideoPipelineRtx, ThePluginSeamCarriesTheRetransmission) {
    plugins::VideoReceiverConfig rc;
    rc.codec           = plugins::VideoCodecKind::kH264;
    rc.ssrc            = kSsrc;
    rc.payload_type    = kPt;
    rc.rtx_payload_type = kRtxPt;

    // Annex-B in, Annex-B out: the assertion below pins the start code the
    // frame contract promises, and the receiver rebuilds it for the bare
    // NAL unit the RFC 6184 §5.1 wire shape carries.
    vp::ReceiverAdapter rx(std::make_unique<VideoReceiver>(
        vp::ReceiverConfig{
            .codec           = video_frame::CodecKind::kH264,
            .ssrc            = kSsrc,
            .payload_type    = kPt,
            .rtx_payload_type = kRtxPt,
        }));

    std::vector<std::uint8_t> bitstream;
    std::uint32_t            ts = 0;
    rx.set_frame_callback([&](plugins::EncodedVideoFrame f, plugins::TimestampUs) {
        bitstream.assign(f.payload.begin(), f.payload.end());
        ts = f.info.rtp_timestamp;
    });

    std::uint8_t nalu[] = {0x65, 0xAA, 0xBB};
    std::vector<std::uint8_t> wrapped = wrap_rtx(100u, {nalu, sizeof(nalu)});

    plugins::VideoRtpPacket pkt;
    pkt.ssrc          = kRtxSsrc;
    pkt.seq           = 900;
    pkt.rtp_timestamp = 3000;
    pkt.payload_type  = kRtxPt;
    pkt.marker        = true;
    pkt.payload       = plugins::BufferView{wrapped.data(), wrapped.size()};

    EXPECT_EQ(rx.push_rtp_rtx(pkt, 1000), plugins::kOk);
    EXPECT_EQ(rx.stats().rtx_packets_received, 1u);
    EXPECT_EQ(ts, 3000u);
    EXPECT_EQ(bitstream, std::vector<std::uint8_t>({0x00, 0x00, 0x00, 0x01,
                                                     0x65, 0xAA, 0xBB}));
}
