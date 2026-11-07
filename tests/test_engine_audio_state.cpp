/**
 * @file tests/test_engine_audio_state.cpp
 * @brief v1.2.0 (audio pipeline, WP-6) — negotiation, payload-type
 *        routing and SDP completion.
 *
 * Three things are pinned down here, and they are the three things
 * WP-6 changed in the engine:
 *
 *   1. **The SDP the engine produces** (B-14).  The offer must carry
 *      `a=ptime` / `a=maxptime`, the Opus `a=fmtp` that describes what
 *      NimRTC actually does (in-band FEC on, the stream's channel
 *      count), the `ssrc-audio-level` `a=extmap` Chrome needs before it
 *      reports `audioLevel` in `inbound-rtp`, and the *per-engine*
 *      SSRC — the value the packet path actually stamps, so the
 *      description and the wire cannot disagree.
 *
 *   2. **The routing table** (B-4).  A payload type the peer never
 *      declared is not audio: it is counted and dropped with
 *      `kAudioPayloadTypeUnknown`.  This is defect F-8 — the old rule
 *      was "anything that is not the video payload type is audio",
 *      which handed telephone-event and RED packets to the audio
 *      decoder as if they were audio.
 *
 *   3. **The failure policy** (A-9 / spec Part D).  A peer that
 *      offers a codec we cannot decode is a *configuration* condition:
 *      `kAudioCodecUnsupported` is reported exactly once, the audio
 *      m-line is rejected in the answer, and no packet is decoded.
 *      A per-packet anomaly, by contrast, is counted and reported only
 *      on the first occurrence and every 50th one.
 *
 * The suite uses the "shell" transport strategy (the same sentinel
 * `test_engine_timeline` uses) so `open()` reaches the open state
 * without binding ICE UDP sockets: these cases are about negotiation
 * and SDP, not about the transport.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nimrtc/core/engine_errors.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/rtp/packet.hpp>

namespace {

using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

using namespace std::chrono_literals;

/** The payload type RFC 7587 assigns to Opus, and the one this engine
 *  advertises by default. */
constexpr std::uint8_t kOpusPt = 111;

/** A payload type no NimRTC session negotiates — the stand-in for the
 *  telephone-event / RED / unsupported-codec packets that used to be
 *  pushed into the audio pipeline.
 *
 *  126 is the classic example: RFC 4733's telephone-event is offered by
 *  every browser on the audio m-line and is *not* audio the receiver
 *  may decode.  (It must not collide with the video payload type 102
 *  the engine advertises on its own video m-line.) */
constexpr std::uint8_t kUnknownPt = 126;

/** The ssrc-audio-level extension URI (RFC 6464). */
constexpr std::string_view kAudioLevelUri =
    "urn:ietf:params:rtp-hdrext:ssrc-audio-level";

class EngineAudioState : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
    }

    static EngineConfig make_test_config() {
        EngineConfig cfg;
        cfg.stun_server_host       = "";
        cfg.local_port_range_begin = 52600;
        cfg.local_port_range_end   = 52700;
        // "shell" is the Slice 8 sentinel strategy: open() succeeds
        // without touching the network (see test_engine_timeline.cpp).
        cfg.transport_strategy_name = "shell";
        return cfg;
    }

    /** Extract the value of the first `a=ssrc:<n>` line of @p sdp.
     *
     *  Returns 0 when the description carries no `a=ssrc:` line. */
    static std::uint32_t first_ssrc(const std::string& sdp) {
        const std::string key = "a=ssrc:";
        std::size_t pos = sdp.find(key);
        if (pos == std::string::npos) return 0;
        pos += key.size();
        std::uint32_t value = 0;
        while (pos < sdp.size() && sdp[pos] >= '0' && sdp[pos] <= '9') {
            value = value * 10 + static_cast<std::uint32_t>(sdp[pos] - '0');
            ++pos;
        }
        return value;
    }

    /** The `m=audio ...` line of @p sdp (without its line ending). */
    static std::string audio_media_line(const std::string& sdp) {
        std::size_t pos = sdp.find("m=audio");
        if (pos == std::string::npos) return {};
        std::size_t end = sdp.find("\r\n", pos);
        if (end == std::string::npos) end = sdp.size();
        return sdp.substr(pos, end - pos);
    }

    /** Build a real RTP packet carrying @p payload_floats as its
     *  payload, on payload type @p pt. */
    static nimrtc::core::ByteBuffer make_rtp(std::uint32_t ssrc,
                                             std::uint8_t pt,
                                             std::uint32_t seq,
                                             std::uint32_t rtp_ts,
                                             std::size_t payload_floats) {
        std::vector<float> pcm(payload_floats, 0.25f);
        nimrtc::rtp::PacketBuilder pb;
        pb.set_ssrc(ssrc)
          .set_seq(static_cast<std::uint16_t>(seq))
          .set_timestamp(rtp_ts)
          .set_payload_type(pt);
        pb.set_payload(nimrtc::core::ByteSpan(
            reinterpret_cast<const std::uint8_t*>(pcm.data()),
            pcm.size() * sizeof(float)));
        return pb.build();
    }

    /** Run the SDP negotiation a real session performs: a second engine
     *  stands in for the peer and supplies its offer. */
    static bool negotiate_with_peer(NimRTCEngine& rx, const EngineConfig& cfg) {
        NimRTCEngine tx(cfg);
        if (tx.open() != 0) return false;
        const std::string offer = tx.create_offer();
        if (offer.empty()) return false;
        return rx.process_remote_sdp(offer).has_value();
    }

    /** Rewrite a real offer so that its audio payload type names an
     *  encoding NimRTC cannot decode (PCMU), and drop the `a=fmtp:`
     *  line that belonged to the original codec.
     *
     *  The m-line's format list is left alone — the payload type stays
     *  111 — so the peer still offers exactly one audio format, it just
     *  offers a different codec for it. */
    static std::string as_unsupported_codec_offer(std::string offer) {
        const std::string from = "a=rtpmap:111 opus/48000/2";
        const std::string to   = "a=rtpmap:111 PCMU/8000";
        std::size_t pos = offer.find(from);
        if (pos == std::string::npos) return {};
        offer.replace(pos, from.size(), to);
        // Drop the fmtp line of the original codec (it describes Opus,
        // and PCMU has no such parameters).
        pos = offer.find("a=fmtp:111 ");
        if (pos != std::string::npos) {
            const std::size_t end = offer.find("\r\n", pos);
            if (end != std::string::npos) {
                offer.erase(pos, end - pos + 2);
            }
        }
        return offer;
    }
};

// ============================================================================
// 1. The SDP the engine produces (B-14)
// ============================================================================

TEST_F(EngineAudioState, OfferCarriesTheCompletedAudioMline) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::string offer = engine.create_offer();
    ASSERT_FALSE(offer.empty());

    // a=ptime / a=maxptime — the frame duration the engine works with
    // (B-5: one Opus frame of `audio_ptime_ms` per packet).
    EXPECT_NE(offer.find("a=ptime:"), std::string::npos)
        << "the offer must advertise the packet duration:\n" << offer;
    EXPECT_NE(offer.find("a=maxptime:"), std::string::npos)
        << "the offer must advertise the maximum packet duration:\n" << offer;

    // a=fmtp — the parameters describe what NimRTC actually does:
    // in-band FEC is enabled (the decoder recovers a lost frame from
    // the redundancy the sender embeds, B-8), and the channel count is
    // the *stream's* (pcm_channels = 1 by default), not the encoding's.
    EXPECT_NE(offer.find("a=fmtp:111 "), std::string::npos)
        << "the offer must carry the Opus format parameters:\n" << offer;
    EXPECT_NE(offer.find("minptime=10"), std::string::npos)
        << "the smallest Opus frame must be advertised:\n" << offer;
    EXPECT_NE(offer.find("useinbandfec=1"), std::string::npos)
        << "in-band FEC is used by the decoder, so it must be signalled:\n"
        << offer;
    EXPECT_NE(offer.find("stereo=0"), std::string::npos)
        << "the stream is mono by default, and the format parameters — not "
           "the rtpmap channel field — carry the stream's channel count:\n"
        << offer;

    // a=extmap — the ssrc-audio-level extension (RFC 6464).  Without
    // it Chrome never reports `audioLevel` in `inbound-rtp` (F-22).
    EXPECT_NE(offer.find(kAudioLevelUri), std::string::npos)
        << "the offer must advertise the ssrc-audio-level extension:\n"
        << offer;

    // a=ssrc — the per-engine SSRC.  The value the packet path stamps
    // is the same member the description quotes, so the two cannot
    // drift apart (A-6).
    EXPECT_NE(first_ssrc(offer), 0u)
        << "the offer must carry an a=ssrc line for the audio stream:\n"
        << offer;
    EXPECT_NE(first_ssrc(offer), 0xDEADBEEFu)
        << "the SSRC must be the per-engine one, not the duplicated "
           "constant two engines in one process used to share";
}

TEST_F(EngineAudioState, TwoEnginesUseDifferentAudioSsrcs) {
    EngineConfig cfg = make_test_config();

    NimRTCEngine a(cfg);
    ASSERT_EQ(a.open(), 0u);
    NimRTCEngine b(cfg);
    ASSERT_EQ(b.open(), 0u);

    const std::uint32_t ssrc_a = first_ssrc(a.create_offer());
    const std::uint32_t ssrc_b = first_ssrc(b.create_offer());

    EXPECT_NE(ssrc_a, 0u);
    EXPECT_NE(ssrc_b, 0u);
    EXPECT_NE(ssrc_a, ssrc_b)
        << "two engines in one process must not share an SSRC (F-12)";
}

// ============================================================================
// 2. The routing table (B-4)
// ============================================================================

TEST_F(EngineAudioState, NegotiatedPayloadTypeIsAccepted) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine rx(cfg);
    ASSERT_EQ(rx.open(), 0u);

    int unknown_pt_errors = 0;
    rx.set_on_error([&](std::uint32_t code, std::string_view) {
        if (code == nimrtc::core::kAudioPayloadTypeUnknown) ++unknown_pt_errors;
    });

    ASSERT_TRUE(negotiate_with_peer(rx, cfg))
        << "the SDP handshake a real session performs must succeed";

    // A packet on the negotiated payload type is the audio path: it
    // reaches the jitter buffer and is played out.
    int frames = 0;
    rx.set_on_audio_frame([&](const float*, std::size_t) { ++frames; });

    constexpr std::uint32_t kSsrc = 0x0B0B0B0Bu;
    const auto first = make_rtp(kSsrc, kOpusPt, 1, 0, 960);
    ASSERT_FALSE(first.empty());
    rx.feed_media_inbound(first.data(), first.size());

    // The jitter buffer holds the packet for its initial delay; the
    // next packet after that delay pops it.
    std::this_thread::sleep_for(120ms);
    const auto second = make_rtp(kSsrc, kOpusPt, 2, 960, 960);
    ASSERT_FALSE(second.empty());
    rx.feed_media_inbound(second.data(), second.size());

    EXPECT_EQ(frames, 1)
        << "the negotiated payload type must reach the audio pipeline";
    EXPECT_EQ(unknown_pt_errors, 0)
        << "the negotiated payload type is in the routing table and must "
           "not be reported as unknown";
}

TEST_F(EngineAudioState, UnknownPayloadTypeIsRejectedAndNotDecoded) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine rx(cfg);
    ASSERT_EQ(rx.open(), 0u);

    std::vector<std::uint32_t> error_codes;
    rx.set_on_error([&](std::uint32_t code, std::string_view) {
        error_codes.push_back(code);
    });

    ASSERT_TRUE(negotiate_with_peer(rx, cfg))
        << "the SDP handshake a real session performs must succeed";

    int frames = 0;
    rx.set_on_audio_frame([&](const float*, std::size_t) { ++frames; });

    constexpr std::uint32_t kSsrc = 0x0C0C0C0Cu;
    const auto packet = make_rtp(kSsrc, kUnknownPt, 1, 0, 960);
    ASSERT_FALSE(packet.empty());
    rx.feed_media_inbound(packet.data(), packet.size());

    ASSERT_EQ(error_codes.size(), 1u)
        << "an unnegotiated payload type must be reported exactly once";
    EXPECT_EQ(error_codes[0], nimrtc::core::kAudioPayloadTypeUnknown)
        << "the report must carry kAudioPayloadTypeUnknown, not a generic "
           "engine error";
    EXPECT_EQ(frames, 0)
        << "a packet on an unnegotiated payload type must never be decoded "
           "(defect F-8: it used to be handed to the audio pipeline as if "
           "it were audio)";
}

TEST_F(EngineAudioState, UnknownPayloadTypeIsThrottled) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine rx(cfg);
    ASSERT_EQ(rx.open(), 0u);

    int unknown_pt_errors = 0;
    rx.set_on_error([&](std::uint32_t code, std::string_view) {
        if (code == nimrtc::core::kAudioPayloadTypeUnknown) ++unknown_pt_errors;
    });

    ASSERT_TRUE(negotiate_with_peer(rx, cfg));

    constexpr std::uint32_t kSsrc = 0x0D0D0D0Du;
    for (std::uint32_t seq = 1; seq <= 5; ++seq) {
        const auto packet = make_rtp(kSsrc, kUnknownPt, seq, seq * 960, 960);
        ASSERT_FALSE(packet.empty());
        rx.feed_media_inbound(packet.data(), packet.size());
    }

    // Spec Part D: per-packet errors are counted but not reported
    // individually — the first occurrence and every 50th one are.
    EXPECT_EQ(unknown_pt_errors, 1)
        << "only the first occurrence may be reported; reporting each drop "
           "would flood the host when a peer keeps sending a stream we "
           "never negotiated";
}

TEST_F(EngineAudioState, AnswerListsOnlyTheNegotiatedPayloadType) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine rx(cfg);
    ASSERT_EQ(rx.open(), 0u);

    NimRTCEngine tx(cfg);
    ASSERT_EQ(tx.open(), 0u);
    const std::string offer = tx.create_offer();
    ASSERT_FALSE(offer.empty());

    const auto answer = rx.process_remote_sdp(offer);
    ASSERT_TRUE(answer.has_value());
    const std::string sdp = answer.value();

    // RFC 3264 §6.1: the answer's format list is a subset of the
    // offer's, and it names the payload type the peer will send on.
    const std::string m_line = audio_media_line(sdp);
    ASSERT_FALSE(m_line.empty()) << "the answer must carry an audio m-line";
    EXPECT_NE(m_line.find(" 111"), std::string::npos)
        << "the answer must keep the negotiated payload type:\n" << m_line;
    EXPECT_EQ(m_line.find(" 0"), std::string::npos)
        << "the answer must not offer a payload type that was not "
           "negotiated:\n" << m_line;

    // The answer echoes the negotiated format parameters (B-4) and the
    // ssrc-audio-level extension (B-14).
    EXPECT_NE(sdp.find("a=fmtp:111 "), std::string::npos)
        << "the answer must echo the negotiated format parameters:\n" << sdp;
    EXPECT_NE(sdp.find(kAudioLevelUri), std::string::npos)
        << "the answer must echo the ssrc-audio-level extension:\n" << sdp;
}

// ============================================================================
// 3. The failure policy (A-9 / spec Part D)
// ============================================================================

TEST_F(EngineAudioState, UnsupportedRemoteCodecIsReportedOnceAndRejected) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine rx(cfg);
    ASSERT_EQ(rx.open(), 0u);

    int unsupported_errors = 0;
    rx.set_on_error([&](std::uint32_t code, std::string_view) {
        if (code == nimrtc::core::kAudioCodecUnsupported) ++unsupported_errors;
    });

    NimRTCEngine tx(cfg);
    ASSERT_EQ(tx.open(), 0u);
    std::string offer = tx.create_offer();
    ASSERT_FALSE(offer.empty());
    offer = as_unsupported_codec_offer(offer);
    ASSERT_FALSE(offer.empty())
        << "the helper must find the Opus rtpmap line in a real offer";

    const auto answer = rx.process_remote_sdp(offer);
    ASSERT_TRUE(answer.has_value());

    EXPECT_EQ(unsupported_errors, 1)
        << "a peer that offers a codec we cannot decode is a configuration "
           "condition: it is reported exactly once, not once per packet";

    // RFC 3264 §6.1: an m-line the answerer cannot serve is rejected
    // with port 0.
    const std::string m_line = audio_media_line(answer.value());
    ASSERT_FALSE(m_line.empty());
    EXPECT_EQ(m_line.find("m=audio 0 "), 0u)
        << "the audio m-line must be rejected:\n" << m_line;

    // And nothing is decoded.
    int frames = 0;
    rx.set_on_audio_frame([&](const float*, std::size_t) { ++frames; });
    const auto packet = make_rtp(0x0E0E0E0Eu, kOpusPt, 1, 0, 960);
    ASSERT_FALSE(packet.empty());
    rx.feed_media_inbound(packet.data(), packet.size());
    EXPECT_EQ(frames, 0)
        << "no audio may be decoded when the negotiated codec is not "
           "supported";
}

} // namespace
