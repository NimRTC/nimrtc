/**
 * @file tests/test_video_negotiation.cpp
 * @brief C9 / C11 — the video payload type is the *negotiated* one.
 *
 * ## The defect this test pins down
 *
 * Before the negotiation work the engine had two independent, unsynchronised
 * ideas of "the video payload type":
 *
 *   - the SDP layer copied the offer's `a=rtpmap:` / `a=fmtp:` lines into
 *     the answer verbatim — which told the peer "I will decode every codec
 *     you offered", including the ones NimRTC has no decoder for (C9);
 *   - the packet path stamped a *locally configured* payload type into every
 *     outbound RTP header and demultiplexed inbound packets by a locally
 *     configured number — so a payload type that appeared in the offer but
 *     was never negotiated still reached the video pipeline (C11).
 *
 * The two numbers could differ, and then video simply stopped flowing: the
 * peer received the stream on a payload type the negotiated description
 * never mentioned, or sent it on one the engine was not listening to.
 *
 * ## What is asserted here
 *
 *   1. The answer names **only** the negotiated payload type (RFC 3264 §6.1)
 *      and the negotiated codec's own `a=rtpmap:` / `a=fmtp:` lines.
 *   2. A packet on a payload type the peer offered but that was **not**
 *      negotiated is dropped — it never reaches the video receiver.
 *   3. A packet on the negotiated payload type reaches the receiver.
 *   4. A peer's answer that keeps the offered payload type is the negotiated
 *      one (the offerer side of the exchange).
 *   5. An offer that names a codec NimRTC cannot decode rejects the video
 *      m-line (port 0, no format list) and reports the condition once.
 *   6. The offer names the **sender's** payload type, because that is the
 *      number the outbound packet path stamps.
 *
 * The suite uses the "shell" transport strategy (the same sentinel
 * `test_engine_audio_state` uses) so `open()` reaches the open state without
 * binding ICE UDP sockets: these cases are about negotiation, not about the
 * transport.  The peer's side is a second engine, so the offers and answers
 * under test are the real ones the engine produces.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/engine_errors.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/rtp/packet.hpp>

namespace {

using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

class VideoNegotiation : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
    }

    /** The configuration the cases share.
     *
     *  The "shell" transport strategy is the Slice 8 sentinel:
     *  `open()` succeeds without touching the network. */
    static EngineConfig make_test_config() {
        EngineConfig cfg;
        cfg.stun_server_host       = "";
        cfg.local_port_range_begin = 52600;
        cfg.local_port_range_end   = 52700;
        cfg.transport_strategy_name = "shell";
        cfg.video_codec_name    = "h264";
        cfg.video_sender_name   = "reference";
        cfg.video_receiver_name = "reference";
        cfg.video_source_name   = "memory";
        cfg.video_sink_name     = "headless";
        return cfg;
    }

    /** The video payload type a config advertises (the sender's). */
    static std::string video_pt(const EngineConfig& cfg) {
        return std::to_string(cfg.video_sender_tuning.payload_type);
    }

    /** Run a second engine as the peer and return its offer. */
    static std::string peer_offer(const EngineConfig& cfg) {
        NimRTCEngine peer(cfg);
        if (peer.open() != 0) return {};
        const std::string offer = peer.create_offer();
        peer.close();
        return offer;
    }

    /** The `m=video ...` line of @p sdp (without its line ending). */
    static std::string video_media_line(const std::string& sdp) {
        const std::size_t pos = sdp.find("m=video ");
        if (pos == std::string::npos) return {};
        std::size_t end = sdp.find("\r\n", pos);
        if (end == std::string::npos) end = sdp.size();
        return sdp.substr(pos, end - pos);
    }

    /** Replace the video m-line's format list with @p formats. */
    static std::string set_video_formats(std::string sdp,
                                         const std::string& formats) {
        const std::size_t pos = sdp.find("m=video ");
        if (pos == std::string::npos) return {};
        const std::size_t eol = sdp.find("\r\n", pos);
        if (eol == std::string::npos) return {};
        const std::size_t last_space = sdp.rfind(' ', eol);
        if (last_space == std::string::npos || last_space < pos) return {};
        sdp.replace(last_space + 1, eol - last_space - 1, formats);
        return sdp;
    }

    /** Insert @p line as a new line right after the one holding @p needle. */
    static std::string insert_line_after(std::string sdp,
                                         const std::string& needle,
                                         const std::string& line) {
        const std::size_t pos = sdp.find(needle);
        if (pos == std::string::npos) return {};
        const std::size_t eol = sdp.find("\r\n", pos);
        if (eol == std::string::npos) return {};
        sdp.insert(eol + 2, line + "\r\n");
        return sdp;
    }

    /** Remove the line holding @p needle. */
    static std::string remove_line(std::string sdp, const std::string& needle) {
        const std::size_t pos = sdp.find(needle);
        if (pos == std::string::npos) return {};
        const std::size_t eol = sdp.find("\r\n", pos);
        if (eol == std::string::npos) return {};
        sdp.erase(pos, eol - pos + 2);
        return sdp;
    }

    /** Rewrite the offer so that the video m-line offers VP8 as well. */
    static std::string with_vp8_offered(std::string offer,
                                         const EngineConfig& cfg) {
        const std::string pt = video_pt(cfg);
        offer = set_video_formats(offer, "96 " + pt);
        offer = insert_line_after(offer, "a=rtpmap:" + pt + " H264/90000",
                                  "a=rtpmap:96 VP8/90000");
        return offer;
    }

    /** Rewrite the offer so that the video codec is one NimRTC cannot
     *  decode (VP8 in place of H.264). */
    static std::string as_undecodable_video_offer(std::string offer,
                                                   const EngineConfig& cfg) {
        const std::string pt = video_pt(cfg);
        offer = remove_line(offer, "a=rtpmap:" + pt + " H264/90000");
        offer = insert_line_after(offer, "m=video ",
                                  "a=rtpmap:" + pt + " VP8/90000");
        // The H.264 format parameters describe the peer's encoder; VP8 has
        // no such parameters (RFC 6381 §6.1).
        offer = remove_line(offer, "a=fmtp:" + pt + " ");
        return offer;
    }

    /** Build a real RTP packet carrying one H.264 NAL unit. */
    static nimrtc::core::ByteBuffer make_rtp(std::uint32_t ssrc,
                                             std::uint8_t pt,
                                             std::uint16_t seq,
                                             std::uint32_t rtp_ts) {
        // One IDR NAL unit (RFC 6184 §5: a single-NAL-unit packet carries
        // the NAL unit without a start code).
        const std::uint8_t nal[] = {0x65, 0x88, 0x84, 0x00};
        nimrtc::rtp::PacketBuilder pb;
        pb.set_ssrc(ssrc)
          .set_seq(seq)
          .set_timestamp(rtp_ts)
          .set_payload_type(pt);
        pb.set_payload(nimrtc::core::ByteSpan(nal, sizeof(nal)));
        return pb.build();
    }
};

// ============================================================================
// 1. The answer names only the negotiated payload type (C9)
// ============================================================================

TEST_F(VideoNegotiation, AnswerNamesOnlyTheNegotiatedPayloadType) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine rx(cfg);
    ASSERT_EQ(rx.open(), 0u);

    // The peer offers VP8 (96) AND H.264 (102).  NimRTC implements only
    // H.264, so the answer must name 102 — and only 102.
    const std::string offer = with_vp8_offered(peer_offer(cfg), cfg);
    ASSERT_FALSE(offer.empty()) << "the peer must produce an offer";
    ASSERT_NE(video_media_line(offer).find(" 96"), std::string::npos)
        << "the rewritten offer must offer two video payload types:\n"
        << offer;

    const auto answer = rx.process_remote_sdp(offer);
    ASSERT_TRUE(answer.has_value()) << "the engine must answer a valid offer";

    const std::string m_line = video_media_line(*answer);
    ASSERT_FALSE(m_line.empty()) << "the answer must carry a video m-line";
    EXPECT_EQ(m_line, "m=video 9 UDP/TLS/RTP/SAVPF 102")
        << "the answer must name exactly the negotiated payload type";

    EXPECT_NE(answer->find("a=rtpmap:102 H264/90000"), std::string::npos)
        << "the negotiated codec's own rtpmap must survive into the answer";
    EXPECT_EQ(answer->find("a=rtpmap:96"), std::string::npos)
        << "the answer must not advertise a codec that was not negotiated";

    rx.close();
}

// ============================================================================
// 2. A payload type that was offered but not negotiated is dropped (C11)
// ============================================================================

TEST_F(VideoNegotiation, UnnegotiatedPayloadTypeIsDropped) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine rx(cfg);
    ASSERT_EQ(rx.open(), 0u);

    const std::string offer = with_vp8_offered(peer_offer(cfg), cfg);
    ASSERT_FALSE(offer.empty());
    const auto answer = rx.process_remote_sdp(offer);
    ASSERT_TRUE(answer.has_value());

    auto* receiver = rx.video_receiver();
    ASSERT_NE(receiver, nullptr);
    ASSERT_EQ(receiver->stats().packets_pushed, 0u);

    // ---- A packet on the OFFERED-BUT-NOT-NEGOTIATED payload type --------
    //
    // The peer declared 96 on its video m-line, so the old routing rule
    // ("anything the peer declared on the video m-line is video") would
    // have pushed it into the H.264 pipeline.  It must not.
    {
        const auto pkt = make_rtp(0x11223344u, /*pt=*/96, /*seq=*/1000,
                                  /*rtp_ts=*/90000);
        ASSERT_FALSE(pkt.empty());
        rx.feed_media_inbound(pkt.data(), pkt.size());
    }
    EXPECT_EQ(receiver->stats().packets_pushed, 0u)
        << "a packet on a payload type that was not negotiated must be "
           "dropped, not decoded";

    // ---- A packet on the NEGOTIATED payload type -------------------------
    {
        const auto pkt = make_rtp(0x11223344u, /*pt=*/102, /*seq=*/1001,
                                  /*rtp_ts=*/93000);
        ASSERT_FALSE(pkt.empty());
        rx.feed_media_inbound(pkt.data(), pkt.size());
    }
    EXPECT_EQ(receiver->stats().packets_pushed, 1u)
        << "a packet on the negotiated payload type must reach the receiver";

    rx.close();
}

// ============================================================================
// 3. The offerer side: the negotiated payload type is the one the peer kept
// ============================================================================

TEST_F(VideoNegotiation, PeerAnswerSelectsTheNegotiatedPayloadType) {
    EngineConfig cfg = make_test_config();
    cfg.video_sender_tuning.payload_type   = 120;
    cfg.video_receiver_tuning.payload_type = 120;

    NimRTCEngine tx(cfg);
    ASSERT_EQ(tx.open(), 0u);

    // We are the OFFERER: our offer names the sender's payload type (120).
    const std::string offer = tx.create_offer();
    ASSERT_FALSE(offer.empty());
    ASSERT_NE(video_media_line(offer).find(" 120"), std::string::npos)
        << "the offer must name the payload type the packet path stamps";

    // The peer's answer echoes our session id (RFC 3264 §5.1), which is how
    // the engine recognises an answer, and it keeps payload type 120
    // (RFC 3264 §6.1: the answer may only use a payload type the offer
    // contained).  An answer also commits the DTLS role, so it must not
    // say "actpass" (RFC 5763 §5).
    std::string answer = offer;
    {
        const std::string from = "a=setup:actpass";
        const std::string to   = "a=setup:passive";
        std::size_t pos = answer.find(from);
        ASSERT_NE(pos, std::string::npos);
        while (pos != std::string::npos) {
            answer.replace(pos, from.size(), to);
            pos = answer.find(from, pos + to.size());
        }
    }

    // An answer is applied, not answered (RFC 3264 §5.1): the call
    // succeeds but produces no SDP.  The engine signals that with an
    // *engaged* optional holding an empty string — an engaged optional
    // means "the remote description was accepted", the empty string means
    // "there is nothing to send back".  A `nullopt` would mean "the
    // description was rejected", which is not what happened here.
    const auto reply = tx.process_remote_sdp(answer);
    ASSERT_TRUE(reply.has_value())
        << "a valid answer must be accepted, not rejected";
    EXPECT_TRUE(reply->empty())
        << "an answer must not be answered (RFC 3264 §5.1)";

    auto* receiver = tx.video_receiver();
    ASSERT_NE(receiver, nullptr);

    const auto wrong = make_rtp(0x55667788u, /*pt=*/96, /*seq=*/2000,
                                /*rtp_ts=*/90000);
    ASSERT_FALSE(wrong.empty());
    tx.feed_media_inbound(wrong.data(), wrong.size());
    EXPECT_EQ(receiver->stats().packets_pushed, 0u)
        << "a payload type that is not the negotiated one must be dropped";

    const auto right = make_rtp(0x55667788u, /*pt=*/120, /*seq=*/2001,
                                /*rtp_ts=*/93000);
    ASSERT_FALSE(right.empty());
    tx.feed_media_inbound(right.data(), right.size());
    EXPECT_EQ(receiver->stats().packets_pushed, 1u)
        << "the negotiated payload type must reach the receiver";

    tx.close();
}

// ============================================================================
// 4. A codec NimRTC cannot decode rejects the video m-line (C9)
// ============================================================================

TEST_F(VideoNegotiation, UndecodableCodecRejectsTheVideoMediaSection) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine rx(cfg);
    ASSERT_EQ(rx.open(), 0u);

    std::atomic<std::uint32_t> first_error{0};
    std::atomic<int> error_count{0};
    rx.set_on_error([&](std::uint32_t code, std::string_view) {
        if (first_error.load() == 0) first_error.store(code);
        error_count.fetch_add(1);
    });

    // The peer offers VP8 only — NimRTC has no VP8 decoder.
    const std::string offer = as_undecodable_video_offer(peer_offer(cfg), cfg);
    ASSERT_FALSE(offer.empty()) << "the rewritten offer must be well-formed";

    const auto answer = rx.process_remote_sdp(offer);
    ASSERT_TRUE(answer.has_value());

    const std::string m_line = video_media_line(*answer);
    ASSERT_FALSE(m_line.empty()) << "the answer must still carry a video m-line";
    EXPECT_EQ(m_line, "m=video 0 UDP/TLS/RTP/SAVPF")
        << "a media section we cannot decode must be rejected with port 0 "
           "and no format list (RFC 3264 §6.1)";

    EXPECT_EQ(first_error.load(), nimrtc::core::kVideoCodecUnsupported)
        << "the host must be told that the peer's video codec is not "
           "decodable";
    EXPECT_EQ(error_count.load(), 1)
        << "the condition is reported once per negotiation, not once per "
           "packet";

    // The receive track is disabled: even a packet on the offered payload
    // type must not reach the receiver.
    auto* receiver = rx.video_receiver();
    ASSERT_NE(receiver, nullptr);
    const auto pkt = make_rtp(0x99AABBCCu, /*pt=*/102, /*seq=*/3000,
                              /*rtp_ts=*/90000);
    ASSERT_FALSE(pkt.empty());
    rx.feed_media_inbound(pkt.data(), pkt.size());
    EXPECT_EQ(receiver->stats().packets_pushed, 0u)
        << "the video track is disabled, so nothing may reach the receiver";

    rx.close();
}

// ============================================================================
// 5. The offer names the SENDER's payload type (C9 / C11)
// ============================================================================

TEST_F(VideoNegotiation, OfferNamesTheSenderPayloadType) {
    EngineConfig cfg = make_test_config();
    // Deliberately different numbers: the offer must name the one the
    // outbound packet path stamps, which is the sender's.
    cfg.video_sender_tuning.payload_type   = 120;
    cfg.video_receiver_tuning.payload_type = 121;

    NimRTCEngine tx(cfg);
    ASSERT_EQ(tx.open(), 0u);

    const std::string offer = tx.create_offer();
    ASSERT_FALSE(offer.empty());

    EXPECT_EQ(video_media_line(offer), "m=video 9 UDP/TLS/RTP/SAVPF 120")
        << "the offer must name the sender's payload type";
    EXPECT_NE(offer.find("a=rtpmap:120 H264/90000"), std::string::npos);
    // Match the *SDP line forms* of the receiver's payload type, not the
    // bare digits: the DTLS fingerprint is colon-separated hex, so a chunk
    // like "12:1F" contains the literal substring "121".  A raw
    // find("121") would then flake depending on the random fingerprint.
    EXPECT_EQ(offer.find("a=rtpmap:121"), std::string::npos)
        << "the offer must not name the receiver's payload type — the "
           "packet path never stamps it";
    EXPECT_EQ(offer.find("a=fmtp:121"), std::string::npos);
    EXPECT_EQ(offer.find("a=rtcp-fb:121"), std::string::npos);

    tx.close();
}

} // namespace
