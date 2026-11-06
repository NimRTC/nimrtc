/**
 * @file tests/test_video_feedback_loop.cpp
 * @brief v1.1 Q3 — closed loop: packet loss -> NACK -> retransmission.
 *
 * The test drives the whole feedback path across three modules:
 *
 *   nimrtc::video_pipeline        : VideoReceiver (loss detection) +
 *                                  VideoSender (retransmission cache)
 *   nimrtc::video_rtcp_feedback  : NACK / PLI wire format
 *
 * Scenario (NACK):
 *   1. The sender emits three RTP packets; the middle one is "lost".
 *   2. The receiver detects the sequence-number gap and reports it
 *      through the feedback callback.
 *   3. The report is encoded into RTCP bytes (a NACK).
 *   4. The NACK is applied to the sender, which retransmits the exact
 *      packet that was lost (same sequence number, timestamp, payload).
 *   5. The retransmitted packet is delivered to the receiver.
 *
 * Scenario (PLI):
 *   1. The receiver loses picture data and asks for a key frame.
 *   2. The request is encoded into RTCP bytes (a PLI).
 *   3. The PLI is applied to the sender, which arms its key-frame marker;
 *      the next pushed frame is flagged as the key frame.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/rtp/packet.hpp>
#include <nimrtc/video_frame/frame.hpp>
#include <nimrtc/video_pipeline/video_pipeline.hpp>
#include <nimrtc/video_rtcp_feedback/video_rtcp_feedback.hpp>

namespace {

namespace vp      = nimrtc::video_pipeline;
namespace vrf     = nimrtc::video_rtcp_feedback;
namespace core    = nimrtc::core;
namespace rtp     = nimrtc::rtp;
namespace vframe  = nimrtc::video_frame;

constexpr std::uint32_t kMediaSsrc    = 0xAABBCCDDu;  // the video stream
constexpr std::uint32_t kFeedbackSsrc = 0x11112222u;  // the receiver
constexpr std::uint8_t  kPt           = 102;          // WebRTC H.264

struct SentPacket {
    std::vector<std::uint8_t> payload;
    std::uint32_t              rtp_ts = 0;
    std::uint16_t              seq    = 0;
    bool                       marker = false;
};

/** Build a one-NALU Annex B frame (IDR). */
vframe::EncodedVideoFrame make_idr_frame(std::vector<std::uint8_t>& storage) {
    storage.clear();
    storage.insert(storage.end(), {0x00, 0x00, 0x00, 0x01});
    storage.insert(storage.end(), {0x65, 0xAA, 0xBB, 0xCC});

    vframe::EncodedVideoFrame f{};
    f.codec       = vframe::CodecKind::kH264;
    f.nalu_format = vframe::NaluFormat::kAnnexBStartCode;
    f.payload     = core::ByteSpan{storage.data(), storage.size()};
    f.is_keyframe = true;
    return f;
}

/** Wrap an RTP payload into the PacketView the receiver expects. */
rtp::PacketView make_view(const SentPacket& p) {
    rtp::PacketView v;
    v.payload      = core::ByteSpan{p.payload.data(), p.payload.size()};
    v.seq          = p.seq;
    v.timestamp    = p.rtp_ts;
    v.marker       = p.marker;
    v.payload_type = kPt;
    v.ssrc         = kMediaSsrc;
    return v;
}

} // namespace

// ===========================================================================
// 1. Loss -> NACK -> retransmission -> re-delivery.
// ===========================================================================

TEST(VideoFeedbackLoop, NackLoopRetransmitsLostPacket) {
    vp::SenderConfig sc;
    sc.codec                     = vframe::CodecKind::kH264;
    sc.ssrc                      = kMediaSsrc;
    sc.payload_type              = kPt;
    sc.initial_seq               = 1000;
    sc.retransmit_cache_packets  = 128;

    vp::VideoSender tx(sc);

    std::vector<SentPacket> sent;
    tx.set_packet_callback([&sent](core::ByteSpan p, std::uint32_t ts,
                                   std::uint16_t seq, bool marker) {
        SentPacket sp;
        sp.payload.assign(p.begin(), p.end());
        sp.rtp_ts = ts;
        sp.seq    = seq;
        sp.marker = marker;
        sent.push_back(std::move(sp));
    });

    // Three frames -> three RTP packets.
    std::vector<std::uint8_t> storage;
    for (int i = 0; i < 3; ++i) {
        auto frame = make_idr_frame(storage);
        auto r = tx.push_frame(frame, /*capture_ts_us=*/1000 * i,
                               /*rtp_timestamp=*/9000u * static_cast<std::uint32_t>(i),
                               /*frame_seq=*/static_cast<std::uint32_t>(i));
        ASSERT_TRUE(r.ok());
    }
    ASSERT_EQ(sent.size(), 3u);
    EXPECT_EQ(sent[1].seq, 1001u);

    // ---- Receiver: the middle packet never arrives. ----
    vp::ReceiverConfig rc;
    rc.codec        = vframe::CodecKind::kH264;
    rc.ssrc         = kMediaSsrc;
    rc.payload_type = kPt;

    vp::VideoReceiver rx(rc);

    std::vector<vp::ReceiverFeedback> reports;
    rx.set_feedback_callback([&reports](const vp::ReceiverFeedback& fb) {
        reports.push_back(fb);
    });

    EXPECT_TRUE(rx.push_rtp(make_view(sent[0]), 1000));
    EXPECT_TRUE(rx.push_rtp(make_view(sent[2]), 3000));

    ASSERT_EQ(reports.size(), 1u);
    ASSERT_EQ(reports[0].missing_packets.size(), 1u);
    EXPECT_EQ(reports[0].missing_packets[0], 1001u);

    // ---- Encode the report as RTCP. ----
    const std::size_t needed = vp::feedback_rtcp_size(reports[0]);
    ASSERT_GT(needed, 0u);
    std::vector<std::uint8_t> rtcp(needed);
    const std::size_t written = vp::build_feedback_rtcp(
        core::MutableByteSpan{rtcp.data(), rtcp.size()},
        reports[0], kFeedbackSsrc, kMediaSsrc);
    ASSERT_EQ(written, needed);

    // The bytes really are a NACK for the lost sequence number.
    auto parsed = vrf::parse_nack(core::ByteSpan{rtcp.data(), written});
    ASSERT_TRUE(parsed.ok());
    ASSERT_EQ(parsed.value().entries.size(), 1u);
    EXPECT_EQ(parsed.value().entries[0].pid, 1001u);
    EXPECT_EQ(parsed.value().sender_ssrc, kFeedbackSsrc);
    EXPECT_EQ(parsed.value().media_ssrc, kMediaSsrc);

    // ---- The sender answers the NACK. ----
    const std::size_t sent_before = sent.size();
    auto applied = vp::apply_feedback_rtcp(
        tx, core::ByteSpan{rtcp.data(), written});
    ASSERT_TRUE(applied.ok());
    EXPECT_EQ(applied.value(), 1);
    ASSERT_EQ(sent.size(), sent_before + 1u);

    const SentPacket& repeat = sent.back();
    EXPECT_EQ(repeat.seq, sent[1].seq);
    EXPECT_EQ(repeat.rtp_ts, sent[1].rtp_ts);
    EXPECT_EQ(repeat.marker, sent[1].marker);
    EXPECT_EQ(repeat.payload, sent[1].payload);
    EXPECT_EQ(tx.stats().packets_retransmitted, 1u);

    // ---- The retransmitted packet reaches the receiver. ----
    EXPECT_TRUE(rx.push_rtp(make_view(repeat), 4000));
    EXPECT_EQ(rx.stats().packets_pushed, 3u);
}

// ===========================================================================
// 2. Lost picture data -> PLI -> the next frame is the key frame.
// ===========================================================================

TEST(VideoFeedbackLoop, PliLoopForcesKeyframe) {
    vp::SenderConfig sc;
    sc.codec        = vframe::CodecKind::kH264;
    sc.ssrc         = kMediaSsrc;
    sc.payload_type = kPt;
    sc.initial_seq  = 2000;

    vp::VideoSender tx(sc);

    vp::ReceiverConfig rc;
    rc.codec                           = vframe::CodecKind::kH264;
    rc.ssrc                            = kMediaSsrc;
    rc.payload_type                    = kPt;
    rc.jitter_buffer_config.max_wait_ms = 50;

    vp::VideoReceiver rx(rc);

    std::vector<vp::ReceiverFeedback> reports;
    rx.set_feedback_callback([&reports](const vp::ReceiverFeedback& fb) {
        reports.push_back(fb);
    });

    // A packet without the marker bit -> the frame never completes.
    SentPacket partial;
    partial.payload  = {0x41, 0xAA};
    partial.rtp_ts   = 7000;
    partial.seq      = 2000;
    partial.marker   = false;

    EXPECT_TRUE(rx.push_rtp(make_view(partial), 0));
    rx.tick(/*now_us=*/60 * 1000);

    bool requested = false;
    for (const auto& fb : reports) {
        if (fb.keyframe_requested) requested = true;
    }
    ASSERT_TRUE(requested);

    // ---- Encode the key-frame request as RTCP. ----
    vp::ReceiverFeedback pli_request;
    pli_request.keyframe_requested = true;

    const std::size_t needed = vp::feedback_rtcp_size(pli_request);
    ASSERT_EQ(needed, vrf::kPliPacketSize);
    std::vector<std::uint8_t> rtcp(needed);
    const std::size_t written = vp::build_feedback_rtcp(
        core::MutableByteSpan{rtcp.data(), rtcp.size()},
        pli_request, kFeedbackSsrc, kMediaSsrc);
    ASSERT_EQ(written, needed);

    auto parsed = vrf::parse_pli(core::ByteSpan{rtcp.data(), written});
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.value().media_ssrc, kMediaSsrc);

    // ---- The sender arms its key-frame marker. ----
    EXPECT_FALSE(tx.keyframe_pending());
    auto applied = vp::apply_feedback_rtcp(
        tx, core::ByteSpan{rtcp.data(), written});
    ASSERT_TRUE(applied.ok());
    EXPECT_EQ(applied.value(), 1);
    EXPECT_TRUE(tx.keyframe_pending());

    std::vector<std::uint8_t> storage;
    auto frame = make_idr_frame(storage);
    auto r = tx.push_frame(frame, 2000, 7000, 1);
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(tx.keyframe_pending());
    EXPECT_EQ(tx.stats().frames_marked_as_keyframe, 1u);
}

// ===========================================================================
// 3. A compound RTCP packet (NACK + PLI) is processed as a whole.
// ===========================================================================

TEST(VideoFeedbackLoop, CompoundFeedbackAppliesNackAndPli) {
    vp::SenderConfig sc;
    sc.codec                    = vframe::CodecKind::kH264;
    sc.ssrc                     = kMediaSsrc;
    sc.payload_type             = kPt;
    sc.initial_seq              = 3000;
    sc.retransmit_cache_packets = 64;

    vp::VideoSender tx(sc);

    std::vector<SentPacket> sent;
    tx.set_packet_callback([&sent](core::ByteSpan p, std::uint32_t ts,
                                   std::uint16_t seq, bool marker) {
        SentPacket sp;
        sp.payload.assign(p.begin(), p.end());
        sp.rtp_ts = ts;
        sp.seq    = seq;
        sp.marker = marker;
        sent.push_back(std::move(sp));
    });

    std::vector<std::uint8_t> storage;
    auto frame = make_idr_frame(storage);
    ASSERT_TRUE(tx.push_frame(frame, 1000, 3000, 1).ok());
    ASSERT_EQ(sent.size(), 1u);

    // NACK for the packet that was just sent, plus a key-frame request.
    vp::ReceiverFeedback fb;
    fb.missing_packets    = {sent[0].seq};
    fb.keyframe_requested = true;

    const std::size_t needed = vp::feedback_rtcp_size(fb);
    std::vector<std::uint8_t> rtcp(needed);
    const std::size_t written = vp::build_feedback_rtcp(
        core::MutableByteSpan{rtcp.data(), rtcp.size()},
        fb, kFeedbackSsrc, kMediaSsrc);
    ASSERT_EQ(written, needed);

    // The NACK comes first, the PLI second.
    auto nack = vrf::parse_nack(core::ByteSpan{rtcp.data(), written});
    ASSERT_TRUE(nack.ok());
    const std::size_t nack_size =
        vrf::kNackMinSize + nack.value().entries.size() * vrf::kNackEntrySize;
    auto pli = vrf::parse_pli(core::ByteSpan{
        rtcp.data() + nack_size, written - nack_size});
    ASSERT_TRUE(pli.ok());

    auto applied = vp::apply_feedback_rtcp(
        tx, core::ByteSpan{rtcp.data(), written});
    ASSERT_TRUE(applied.ok());
    EXPECT_EQ(applied.value(), 2);
    EXPECT_EQ(tx.stats().packets_retransmitted, 1u);
    EXPECT_TRUE(tx.keyframe_pending());
}

// ===========================================================================
// 4. Buffer sizing: an undersized destination yields no bytes.
// ===========================================================================

TEST(VideoFeedbackLoop, FeedbackRtcpRespectsOutputSize) {
    vp::ReceiverFeedback fb;
    fb.missing_packets    = {1, 2, 3};
    fb.keyframe_requested = true;

    const std::size_t needed = vp::feedback_rtcp_size(fb);
    ASSERT_GT(needed, 0u);

    std::vector<std::uint8_t> small(needed - 1, 0xCDu);
    const std::size_t written = vp::build_feedback_rtcp(
        core::MutableByteSpan{small.data(), small.size()},
        fb, kFeedbackSsrc, kMediaSsrc);
    EXPECT_EQ(written, 0u);
    for (auto b : small) EXPECT_EQ(b, 0xCDu);

    // An empty report needs no bytes at all.
    vp::ReceiverFeedback empty;
    EXPECT_EQ(vp::feedback_rtcp_size(empty), 0u);
}
