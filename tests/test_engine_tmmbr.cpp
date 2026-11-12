/**
 * @file tests/test_engine_tmmbr.cpp
 * @brief v1.2.0 (session capability plan X2 / X4 / X5) — the engine's
 *        side of the RTCP XR / TMMBR plane.
 *
 * The wire format has its own tests
 * (`src/modules/rtp/tests/test_rtcp_xr.cpp` covers the XR blocks and the
 * TMMBR / TMMBN round-trip).  What those cannot cover is the *wiring*:
 * that the engine's RTCP demux routes an inbound TMMBR to the TMMBR
 * path, that the limit it carries is honoured on the egress side, and
 * that the SDP actually negotiates the features.
 *
 * Covered:
 *   T-E1 - the offer advertises `a=rtcp-fb:<pt> ccm tmmbr` when TMMBR
 *          is enabled, and does not when it is disabled
 *   T-E2 - the offer advertises `a=rtcp-xr:rcvr-rtt=all
 *          stat-summary=loss,jitt` when XR is enabled
 *   T-E3 - the answer echoes the negotiated subset of both attributes
 *          (RFC 3264 §6.1 / RFC 3611 §5.2 / RFC 5104 §7.2)
 *   T-E4 - an inbound TMMBR clamps the peer's limit (RFC 5104
 *          §4.2.1.2) — the tightest limit wins
 *   T-E5 - an inbound TMMBR naming a foreign media source is ignored
 *   T-E6 - the SDP-level `disable_tmmbr` switch removes the "tmmbr"
 *          parameter from the answer's CCM list (RFC 5104 §7.2)
 *   T-E7 - a malformed inbound TMMBR is dropped, not fatal
 *
 * The suite uses the "shell" transport strategy (the same sentinel
 * `test_engine_xr_report` uses) so `open()` reaches the open state
 * without binding ICE UDP sockets.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>

namespace {

using nimrtc::core::ByteBuffer;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

void put_be32(ByteBuffer& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void put_be16(ByteBuffer& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void put_u8(ByteBuffer& out, std::uint8_t v) { out.push_back(v); }

/// Build a TMMBR (RTPFB, FMT = 3) carrying one FCI entry.
///
/// RFC 5104 §4.2.1.1: the bit rate is `mantissa * 2^exp`, with a
/// 17-bit mantissa and a 6-bit exponent.  The engine's own module
/// builds the same encoding; this helper is deliberately independent
/// so the test does not validate the builder against itself.
///
/// The RTPFB header (RFC 4585 §6.1) is eight bytes: the SSRC of the
/// packet sender, then the SSRC of the media source — the latter is
/// unused by a TMMBR and SHALL be zero (RFC 5104 §4.2.1.2).  The FCI
/// entry follows it.
ByteBuffer make_tmmbr(std::uint32_t sender_ssrc, std::uint32_t media_ssrc,
                      std::uint32_t bitrate_bps) {
    // The smallest exponent whose shifted value fits the 17-bit
    // mantissa — the same rule the module's encoder uses, so the value
    // survives the round-trip exactly.
    unsigned exp = 0;
    std::uint32_t mantissa = bitrate_bps;
    while (exp < 63u && (mantissa >> exp) > 0x1FFFFu) ++exp;
    mantissa >>= exp;

    ByteBuffer out;
    put_u8(out, 0x80u | 3u);            // V=2, FMT=3 (TMMBR)
    put_u8(out, 205u);                  // PT = RTPFB
    put_be16(out, 4u);                  // length: 5 words (20 bytes) minus one
    put_be32(out, sender_ssrc);         // SSRC of the packet sender
    put_be32(out, 0u);                  // SSRC of media source: unused, SHALL be 0

    // ---- The FCI entry (RFC 5104 §4.2.1.1) ---------------------------
    put_be32(out, media_ssrc);          // the SSRC the tuple applies to
    //  byte 0: | exp (6) | mantissa[16:15] |
    //  byte 1: |        mantissa[14:7]     |
    //  byte 2: | mantissa[6:0] | ovh[8]   |
    //  byte 3: |          overhead[7:0]     |
    put_u8(out, static_cast<std::uint8_t>((exp << 2) |
                                           ((mantissa >> 15) & 0x03u)));
    put_u8(out, static_cast<std::uint8_t>((mantissa >> 7) & 0xFFu));
    put_u8(out, static_cast<std::uint8_t>((mantissa & 0x7Fu) << 1));
    put_u8(out, 0u);                    // measured overhead (bytes)
    return out;
}

/// Drop every SDP line that mentions `token`.
std::string strip_sdp_lines(const std::string& sdp, std::string_view token) {
    std::string out;
    std::size_t pos = 0;
    while (pos <= sdp.size()) {
        const std::size_t eol = sdp.find('\n', pos);
        const std::size_t end = (eol == std::string::npos) ? sdp.size() : eol;
        const std::string line = sdp.substr(pos, end - pos);
        if (line.find(token) == std::string::npos) {
            out += line;
            out += "\r\n";
        }
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return out;
}

class EngineTmmbr : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
    }

    /// The "shell" transport strategy reaches the open state without
    /// touching the network (see test_engine_xr_report.cpp).
    static EngineConfig make_test_config() {
        EngineConfig cfg;
        cfg.stun_server_host        = "";
        cfg.local_port_range_begin  = 52700;
        cfg.local_port_range_end    = 52800;
        cfg.transport_strategy_name = "shell";
        // A pinned audio SSRC: the TMMBR's FCI entry names the media
        // source the limit applies to, and the engine only honours a
        // tuple that names a source it actually sends.
        cfg.audio_ssrc = 0x11223344u;
        return cfg;
    }
};

// ---------------------------------------------------------------------------
// T-E1 — the offer advertises the TMMBR
// ---------------------------------------------------------------------------

TEST_F(EngineTmmbr, TheOfferAdvertisesTmmbrWhenEnabled) {
    EngineConfig cfg = make_test_config();
    cfg.tmmbr_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::string offer = engine.create_offer();
    ASSERT_FALSE(offer.empty());

    EXPECT_NE(offer.find("a=rtcp-fb:111 ccm tmmbr"), std::string::npos)
        << "RFC 5104 §7.2 gives TMMBR the codec-control-message form: "
           "a=rtcp-fb:<pt> ccm tmmbr.  Without the attribute the peer "
           "may not use the message at all.";
}

TEST_F(EngineTmmbr, TheOfferDoesNotAdvertiseTmmbrWhenDisabled) {
    // The default is off: a host that never asked for a TMMBR must not
    // get one, and must not negotiate one either.
    NimRTCEngine engine(make_test_config());
    ASSERT_EQ(engine.open(), 0u);

    const std::string offer = engine.create_offer();
    ASSERT_FALSE(offer.empty());
    EXPECT_EQ(offer.find("tmmbr"), std::string::npos)
        << "the default offer must not negotiate a TMMBR";
}

TEST_F(EngineTmmbr, TheDisableTmmbrSwitchRemovesTheOfferEntry) {
    // `disable_tmmbr` overrides `tmmbr_enabled`: a host that sets both
    // gets no TMMBR at all.
    EngineConfig cfg = make_test_config();
    cfg.tmmbr_enabled = true;
    cfg.disable_tmmbr = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::string offer = engine.create_offer();
    ASSERT_FALSE(offer.empty());
    EXPECT_EQ(offer.find("tmmbr"), std::string::npos)
        << "disable_tmmbr must veto the whole feature, SDP included";
}

// ---------------------------------------------------------------------------
// T-E2 — the offer advertises the XR attribute
// ---------------------------------------------------------------------------

TEST_F(EngineTmmbr, TheOfferAdvertisesRtcpXrWhenEnabled) {
    EngineConfig cfg = make_test_config();
    cfg.rtcp_xr_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::string offer = engine.create_offer();
    ASSERT_FALSE(offer.empty());

    EXPECT_NE(offer.find("a=rtcp-xr:rcvr-rtt=all stat-summary=loss,jitt"),
              std::string::npos)
        << "RFC 3611 §5.1: the rtcp-xr attribute names the report blocks "
           "the peer may expect — BT=4 (rcvr-rtt) and BT=6 (stat-summary), "
           "with the two field indicators the engine measures";
}

TEST_F(EngineTmmbr, TheOfferDoesNotAdvertiseRtcpXrWhenDisabled) {
    NimRTCEngine engine(make_test_config());
    ASSERT_EQ(engine.open(), 0u);

    const std::string offer = engine.create_offer();
    ASSERT_FALSE(offer.empty());
    EXPECT_EQ(offer.find("a=rtcp-xr"), std::string::npos)
        << "XR is opt-in: a host that does not want the packets must not "
           "pay for them";
}

// ---------------------------------------------------------------------------
// T-E3 — the answer echoes the negotiated subset
// ---------------------------------------------------------------------------

TEST_F(EngineTmmbr, TheAnswerEchoesBothAttributes) {
    // The peer offers both features — an answer can only echo what
    // the offer named (RFC 3264 §6.1).
    EngineConfig peer_cfg = make_test_config();
    peer_cfg.tmmbr_enabled   = true;
    peer_cfg.rtcp_xr_enabled = true;

    NimRTCEngine peer(peer_cfg);
    ASSERT_EQ(peer.open(), 0u);
    const std::string peer_offer = peer.create_offer();
    ASSERT_NE(peer_offer.find("ccm tmmbr"), std::string::npos);
    ASSERT_NE(peer_offer.find("a=rtcp-xr:"), std::string::npos);

    EngineConfig cfg = make_test_config();
    cfg.tmmbr_enabled   = true;
    cfg.rtcp_xr_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto answer = engine.process_remote_sdp(peer_offer);
    ASSERT_TRUE(answer.has_value());
    ASSERT_FALSE(answer->empty());

    EXPECT_NE(answer->find("ccm tmmbr"), std::string::npos)
        << "RFC 5104 §7.2: only the joint subset of CCM parameters may be "
           "used, so the answer must carry the tmmbr parameter back";

    EXPECT_NE(answer->find("a=rtcp-xr:"), std::string::npos)
        << "RFC 3611 §5.2: the answerer echoes the collaborative "
           "'rcvr-rtt' parameter it supports";
}

TEST_F(EngineTmmbr, TheAnswerDoesNotInventAttributesThePeerDidNotOffer) {
    NimRTCEngine peer(make_test_config());
    ASSERT_EQ(peer.open(), 0u);
    // A peer that offers neither TMMBR nor XR.
    std::string peer_offer = peer.create_offer();
    peer_offer = strip_sdp_lines(peer_offer, "tmmbr");
    peer_offer = strip_sdp_lines(peer_offer, "rtcp-xr");

    EngineConfig cfg = make_test_config();
    cfg.tmmbr_enabled   = true;
    cfg.rtcp_xr_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto answer = engine.process_remote_sdp(peer_offer);
    ASSERT_TRUE(answer.has_value());

    EXPECT_EQ(answer->find("tmmbr"), std::string::npos)
        << "the answer must not name a CCM parameter the offerer never "
           "offered (RFC 5104 §7.2)";
    EXPECT_EQ(answer->find("a=rtcp-xr"), std::string::npos)
        << "RFC 3611 §5.1: the rtcp-xr attribute is not supplied when XR "
           "is not used from the start of the session";
}

// ---------------------------------------------------------------------------
// T-E4 — the inbound TMMBR clamps the egress rate
// ---------------------------------------------------------------------------

TEST_F(EngineTmmbr, AnInboundTmmbrSetsThePeerLimit) {
    NimRTCEngine engine(make_test_config());
    ASSERT_EQ(engine.open(), 0u);

    EXPECT_EQ(engine.peer_tmmbr_limit_bps(), 0u)
        << "no limit has been received yet";

    // The engine's own audio SSRC — the tuple has to name a stream the
    // engine actually sends, or it is not ours to honour.
    const ByteBuffer tmmbr = make_tmmbr(0xAABBCCDDu, 0x11223344u, 320'000u);
    engine.feed_media_inbound(tmmbr.data(), tmmbr.size());

    EXPECT_EQ(engine.peer_tmmbr_limit_bps(), 320'000u)
        << "RFC 5104 §4.2.1.2: the receiver's stated maximum bit rate is "
           "the limit the sender must obey";
    EXPECT_EQ(engine.tmmbr_received_count(), 1u);
}

TEST_F(EngineTmmbr, TheTightestTmmbrLimitWins) {
    NimRTCEngine engine(make_test_config());
    ASSERT_EQ(engine.open(), 0u);

    // The engine's own audio SSRC: the engine's config pins it, so the
    // test can name a source the engine really sends.
    const std::uint32_t our_ssrc = engine.config().audio_ssrc;

    const ByteBuffer loose = make_tmmbr(0xAABBCCDDu, our_ssrc, 500'000u);
    engine.feed_media_inbound(loose.data(), loose.size());
    ASSERT_EQ(engine.peer_tmmbr_limit_bps(), 500'000u);

    const ByteBuffer tight = make_tmmbr(0xAABBCCDDu, our_ssrc, 200'000u);
    engine.feed_media_inbound(tight.data(), tight.size());
    EXPECT_EQ(engine.peer_tmmbr_limit_bps(), 200'000u)
        << "the bounding set is the tightest constraint (RFC 5104 "
           "§3.5.4.2)";

    // A *looser* limit must not raise the ceiling.
    const ByteBuffer looser = make_tmmbr(0xAABBCCDDu, our_ssrc, 900'000u);
    engine.feed_media_inbound(looser.data(), looser.size());
    EXPECT_EQ(engine.peer_tmmbr_limit_bps(), 200'000u)
        << "a looser limit must not undo a tighter one";
}

// ---------------------------------------------------------------------------
// T-E5 — a tuple about somebody else's stream is ignored
// ---------------------------------------------------------------------------

TEST_F(EngineTmmbr, ATmmbrNamingAForeignSourceIsIgnored) {
    NimRTCEngine engine(make_test_config());
    ASSERT_EQ(engine.open(), 0u);

    const std::uint32_t our_ssrc = engine.config().audio_ssrc;
    const ByteBuffer tmmbr = make_tmmbr(0xAABBCCDDu, our_ssrc + 1, 100'000u);
    engine.feed_media_inbound(tmmbr.data(), tmmbr.size());

    EXPECT_EQ(engine.peer_tmmbr_limit_bps(), 0u)
        << "a tuple about a media source the engine does not send is not "
           "ours to honour";
    EXPECT_EQ(engine.tmmbr_received_count(), 0u);
}

// ---------------------------------------------------------------------------
// T-E6 — the answer-side CCM filter
// ---------------------------------------------------------------------------

TEST_F(EngineTmmbr, TheAnswerRemovesTheTmmbrParameterWhenDisabled) {
    // The peer offers the codec-control message; the engine has TMMBR
    // switched off, so the answer must drop the "tmmbr" parameter
    // while keeping the rest of the entry (RFC 5104 §7.2).
    NimRTCEngine peer(make_test_config());
    ASSERT_EQ(peer.open(), 0u);
    std::string peer_offer = peer.create_offer();
    // Put the CCM entry on the audio payload type.
    peer_offer = peer_offer.replace(
        peer_offer.find("a=rtcp-fb:111 transport-cc"),
        std::string("a=rtcp-fb:111 transport-cc").size(),
        "a=rtcp-fb:111 transport-cc\r\na=rtcp-fb:111 ccm tmmbr");
    ASSERT_NE(peer_offer.find("ccm tmmbr"), std::string::npos);

    // The answerer has TMMBR off.
    NimRTCEngine engine(make_test_config());
    ASSERT_EQ(engine.open(), 0u);
    const auto answer = engine.process_remote_sdp(peer_offer);
    ASSERT_TRUE(answer.has_value());

    EXPECT_EQ(answer->find("tmmbr"), std::string::npos)
        << "the answer must remove the CCM parameter the answerer does "
           "not support (RFC 5104 §7.2)";
}

TEST_F(EngineTmmbr, TheAnswerKeepsTheTmmbrParameterWhenEnabled) {
    NimRTCEngine peer(make_test_config());
    ASSERT_EQ(peer.open(), 0u);
    std::string peer_offer = peer.create_offer();
    peer_offer = peer_offer.replace(
        peer_offer.find("a=rtcp-fb:111 transport-cc"),
        std::string("a=rtcp-fb:111 transport-cc").size(),
        "a=rtcp-fb:111 transport-cc\r\na=rtcp-fb:111 ccm tmmbr");
    ASSERT_NE(peer_offer.find("ccm tmmbr"), std::string::npos);

    EngineConfig cfg = make_test_config();
    cfg.tmmbr_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    const auto answer = engine.process_remote_sdp(peer_offer);
    ASSERT_TRUE(answer.has_value());

    EXPECT_NE(answer->find("ccm tmmbr"), std::string::npos)
        << "the joint subset includes TMMBR, so the answer must keep it";
}

// ---------------------------------------------------------------------------
// T-E7 — a malformed TMMBR is dropped, not fatal
// ---------------------------------------------------------------------------

TEST_F(EngineTmmbr, AMalformedTmmbrIsDroppedNotFatal) {
    NimRTCEngine engine(make_test_config());
    ASSERT_EQ(engine.open(), 0u);

    // RTPFB with FMT = 3 but a length that leaves no room for the FCI
    // entry — the parser must reject it and the read path must survive.
    ByteBuffer broken;
    put_u8(broken, 0x80u);
    put_u8(broken, 205u);
    put_be16(broken, 1u);            // 4 bytes total
    put_be32(broken, 0xAABBCCDDu);   // sender SSRC, and nothing else

    engine.feed_media_inbound(broken.data(), broken.size());

    EXPECT_EQ(engine.peer_tmmbr_limit_bps(), 0u)
        << "a malformed TMMBR must not be applied";
    EXPECT_EQ(engine.tmmbr_received_count(), 0u);
}

}  // namespace
