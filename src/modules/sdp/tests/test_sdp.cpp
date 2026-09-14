// modules/sdp/tests/test_sdp.cpp
// =============================================================================
// SDP module tests — covers enum conversion, parser/munger round-trips on
// RFC 4566 §6 minimum SDP and RFC 8829 §7 BUNDLE examples, and error
// paths.
// =============================================================================

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include <nimrtc/core/error.hpp>
#include <nimrtc/sdp/session_description.hpp>

namespace {

using namespace nimrtc::sdp;
using nimrtc::core::ErrorCode;

// -----------------------------------------------------------------------------
// MediaType parsing
// -----------------------------------------------------------------------------
TEST(SdpMediaType, ParseValid) {
    ASSERT_TRUE(parse_media_type("audio").has_value());
    EXPECT_EQ(*parse_media_type("audio"), MediaType::Audio);
    ASSERT_TRUE(parse_media_type("video").has_value());
    EXPECT_EQ(*parse_media_type("video"), MediaType::Video);
    ASSERT_TRUE(parse_media_type("application").has_value());
    EXPECT_EQ(*parse_media_type("application"), MediaType::Application);
    ASSERT_TRUE(parse_media_type("data").has_value());
    EXPECT_EQ(*parse_media_type("data"), MediaType::Data);
    EXPECT_FALSE(parse_media_type("unknown").has_value());
    EXPECT_FALSE(parse_media_type("text").has_value());   // text is munger fallback, not parser-accepted
    EXPECT_FALSE(parse_media_type("").has_value());
}

TEST(SdpMediaType, ToString) {
    EXPECT_STREQ(to_string(MediaType::Audio), "audio");
    EXPECT_STREQ(to_string(MediaType::Video), "video");
    EXPECT_STREQ(to_string(MediaType::Application), "application");
    EXPECT_STREQ(to_string(MediaType::Data), "data");
    EXPECT_STREQ(to_string(MediaType::Other), "text");   // munger fallback
}

// -----------------------------------------------------------------------------
// Direction parsing
// -----------------------------------------------------------------------------
TEST(SdpDirection, ParseValid) {
    ASSERT_TRUE(parse_direction("sendrecv").has_value());
    EXPECT_EQ(*parse_direction("sendrecv"), Direction::SendRecv);
    EXPECT_EQ(*parse_direction("sendonly"), Direction::SendOnly);
    EXPECT_EQ(*parse_direction("recvonly"), Direction::RecvOnly);
    EXPECT_EQ(*parse_direction("inactive"), Direction::Inactive);
    EXPECT_FALSE(parse_direction("foobar").has_value());
    EXPECT_FALSE(parse_direction("").has_value());
}

TEST(SdpDirection, ToString) {
    EXPECT_STREQ(to_string(Direction::SendRecv), "sendrecv");
    EXPECT_STREQ(to_string(Direction::SendOnly), "sendonly");
    EXPECT_STREQ(to_string(Direction::RecvOnly), "recvonly");
    EXPECT_STREQ(to_string(Direction::Inactive), "inactive");
}

// -----------------------------------------------------------------------------
// MediaDescription default construction
// -----------------------------------------------------------------------------
TEST(SdpMediaDescription, DefaultConstructs) {
    MediaDescription md;
    EXPECT_EQ(md.type, MediaType::Audio);
    EXPECT_EQ(md.port, 0u);
    EXPECT_TRUE(md.protocol.empty());
    EXPECT_TRUE(md.formats.empty());
    EXPECT_EQ(md.direction, Direction::SendRecv);
    EXPECT_TRUE(md.mid.empty());
    EXPECT_FALSE(md.msid.has_value());
    EXPECT_TRUE(md.rtpmap.empty());
    EXPECT_TRUE(md.fmtp.empty());
    EXPECT_TRUE(md.rtcp_fb.empty());
    EXPECT_TRUE(md.extmap.empty());
    EXPECT_TRUE(md.candidates.empty());
    EXPECT_TRUE(md.extra_attrs.empty());
    EXPECT_EQ(md.ttl, 0);
    EXPECT_EQ(md.address_family, 4);   // default IPv4
}

// -----------------------------------------------------------------------------
// SessionDescription default construction
// -----------------------------------------------------------------------------
TEST(SdpSessionDescription, DefaultConstructs) {
    SessionDescription sdp;
    EXPECT_EQ(sdp.version, 0);
    EXPECT_TRUE(sdp.session_id.empty());
    EXPECT_EQ(sdp.session_name, "-");
    EXPECT_TRUE(sdp.bundle_mids.empty());
    EXPECT_TRUE(sdp.msids.empty());
    EXPECT_TRUE(sdp.media.empty());
    EXPECT_TRUE(sdp.extra_attrs.empty());
    EXPECT_EQ(sdp.session_address_family, 4);  // default IPv4
}

// -----------------------------------------------------------------------------
// Rtpmap and Fmtp storage
// -----------------------------------------------------------------------------
TEST(SdpMediaDescription, RtpmapAndFmtp) {
    MediaDescription md;
    md.rtpmap["96"] = MediaDescription::RtpMap{"opus", 48000, 2};
    md.fmtp["96"] = "minptime=10;useinbandfec=1";

    ASSERT_EQ(md.rtpmap.size(), 1u);
    EXPECT_EQ(md.rtpmap.at("96").encoding, "opus");
    EXPECT_EQ(md.rtpmap.at("96").clock_rate, 48000u);
    EXPECT_EQ(md.rtpmap.at("96").channels, 2u);

    ASSERT_EQ(md.fmtp.size(), 1u);
    EXPECT_EQ(md.fmtp.at("96"), "minptime=10;useinbandfec=1");
}

// -----------------------------------------------------------------------------
// Msid per media line
// -----------------------------------------------------------------------------
TEST(SdpSessionDescription, MsidIndexedByMedia) {
    SessionDescription sdp;
    sdp.media.resize(2);
    sdp.media[0].msid = Msid{"stream-a", "track-audio"};
    sdp.media[1].msid = Msid{"stream-v", "track-video"};

    sdp.msids = {
        {"stream-a", "track-audio"},
        {"stream-v", "track-video"},
    };

    ASSERT_EQ(sdp.msids.size(), 2u);
    EXPECT_EQ(sdp.msids[0].stream_id, "stream-a");
    EXPECT_EQ(sdp.msids[0].track_id, "track-audio");
    EXPECT_EQ(sdp.msids[1].stream_id, "stream-v");
    EXPECT_EQ(sdp.msids[1].track_id, "track-video");
}

// -----------------------------------------------------------------------------
// Parser — minimal SDP (RFC 4566 §6)
// -----------------------------------------------------------------------------
TEST(SdpParser, MinimalSdp) {
    constexpr std::string_view text =
        "v=0\r\n"
        "o=- 1234567890 2 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n";

    Parser p;
    auto r = p.parse(text);
    ASSERT_TRUE(r.ok()) << r.error().message();

    const auto& sdp = r.value();
    EXPECT_EQ(sdp.version, 0);
    EXPECT_EQ(sdp.origin_username, "-");
    EXPECT_EQ(sdp.origin_session_id, "1234567890");
    EXPECT_EQ(sdp.origin_session_version, "2");
    EXPECT_EQ(sdp.origin_address, "127.0.0.1");
    EXPECT_EQ(sdp.session_name, "-");
    EXPECT_TRUE(sdp.media.empty());
}

TEST(SdpParser, MalformedLineRejected) {
    // Line "garbage" has no '=' in position 1.
    constexpr std::string_view text =
        "v=0\r\n"
        "garbage\r\n";

    Parser p;
    auto r = p.parse(text);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), ErrorCode::ProtocolError);
}

// -----------------------------------------------------------------------------
// Parser — WebRTC offer with BUNDLE / ICE / DTLS / mid / rtpmap / fmtp /
// rtcp-fb (multiple formats — exercises the bug we just fixed)
// -----------------------------------------------------------------------------
TEST(SdpParser, WebRtcOfferExcerpt) {
    constexpr std::string_view text =
        "v=0\r\n"
        "o=- 4962303335927620101 2 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "a=group:BUNDLE 0 1\r\n"
        "a=msid-semantic: WMS\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111 63 9 0 8 13\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=ice-ufrag:abc\r\n"
        "a=ice-pwd:supersecretpwd\r\n"
        "a=fingerprint:sha-256 AA:BB:CC\r\n"
        "a=setup:actpass\r\n"
        "a=mid:0\r\n"
        "a=rtcp-mux\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=rtcp-fb:111 nack\r\n"
        "a=fmtp:111 minptime=10;useinbandfec=1\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96 97\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=ice-ufrag:def\r\n"
        "a=ice-pwd:anothersecret\r\n"
        "a=fingerprint:sha-256 DD:EE:FF\r\n"
        "a=setup:actpass\r\n"
        "a=mid:1\r\n"
        "a=sendrecv\r\n"
        "a=rtpmap:96 VP8/90000\r\n";

    Parser p;
    auto r = p.parse(text);
    ASSERT_TRUE(r.ok()) << r.error().message();
    const auto& sdp = r.value();

    // Session-level
    EXPECT_EQ(sdp.version, 0);
    EXPECT_EQ(sdp.origin_session_id, "4962303335927620101");
    ASSERT_EQ(sdp.bundle_mids.size(), 2u);
    EXPECT_EQ(sdp.bundle_mids[0], "0");
    EXPECT_EQ(sdp.bundle_mids[1], "1");
    EXPECT_EQ(sdp.msid_semantic_token, "WMS");
    ASSERT_EQ(sdp.media.size(), 2u);

    // First media: audio
    const auto& audio = sdp.media[0];
    EXPECT_EQ(audio.type, MediaType::Audio);
    EXPECT_EQ(audio.port, 9u);
    EXPECT_EQ(audio.protocol, "UDP/TLS/RTP/SAVPF");
    // ← This is the bug we just fixed: all 6 formats must be present.
    ASSERT_EQ(audio.formats.size(), 6u);
    EXPECT_EQ(audio.formats[0], "111");
    EXPECT_EQ(audio.formats[1], "63");
    EXPECT_EQ(audio.formats[2], "9");
    EXPECT_EQ(audio.formats[3], "0");
    EXPECT_EQ(audio.formats[4], "8");
    EXPECT_EQ(audio.formats[5], "13");

    EXPECT_EQ(audio.ice_ufrag, "abc");
    EXPECT_EQ(audio.ice_pwd, "supersecretpwd");
    EXPECT_EQ(audio.dtls_fingerprint_algo, "sha-256");
    EXPECT_EQ(audio.dtls_fingerprint_value, "AA:BB:CC");
    EXPECT_EQ(audio.dtls_setup, "actpass");
    EXPECT_EQ(audio.mid, "0");
    EXPECT_EQ(audio.rtcp_mux_value, "rtcp-mux");
    EXPECT_EQ(audio.connection_address, "0.0.0.0");
    EXPECT_EQ(audio.address_family, 4);

    ASSERT_EQ(audio.rtpmap.size(), 1u);
    auto it = audio.rtpmap.find("111");
    ASSERT_NE(it, audio.rtpmap.end());
    EXPECT_EQ(it->second.encoding, "opus");
    EXPECT_EQ(it->second.clock_rate, 48000u);
    EXPECT_EQ(it->second.channels, 2u);

    ASSERT_EQ(audio.fmtp.size(), 1u);
    EXPECT_EQ(audio.fmtp.at("111"), "minptime=10;useinbandfec=1");

    ASSERT_EQ(audio.rtcp_fb.size(), 1u);
    EXPECT_EQ(audio.rtcp_fb[0], "111 nack");

    // Second media: video
    const auto& video = sdp.media[1];
    EXPECT_EQ(video.type, MediaType::Video);
    EXPECT_EQ(video.mid, "1");
    EXPECT_EQ(video.direction, Direction::SendRecv);
    ASSERT_EQ(video.formats.size(), 2u);
    EXPECT_EQ(video.formats[0], "96");
    EXPECT_EQ(video.formats[1], "97");
    ASSERT_EQ(video.rtpmap.size(), 1u);
    EXPECT_EQ(video.rtpmap.at("96").encoding, "VP8");
    EXPECT_EQ(video.rtpmap.at("96").clock_rate, 90000u);
}

// -----------------------------------------------------------------------------
// Parser — extmap with optional direction suffix (the bug we just fixed)
// -----------------------------------------------------------------------------
TEST(SdpParser, ExtmapDirectionSuffix) {
    constexpr std::string_view text =
        "v=0\r\n"
        "o=- 0 0 IN IP4 0.0.0.0\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level\r\n"
        "a=extmap:2/sendonly urn:ietf:params:rtp-hdrext:toffset\r\n"
        "a=extmap:3/recvonly urn:ietf:params:rtp-hdrext:abs-send-time\r\n";

    Parser p;
    auto r = p.parse(text);
    ASSERT_TRUE(r.ok()) << r.error().message();

    ASSERT_EQ(r.value().media.size(), 1u);
    const auto& m = r.value().media[0];
    ASSERT_EQ(m.extmap.size(), 3u);

    EXPECT_EQ(m.extmap[0].id, 1);
    EXPECT_EQ(m.extmap[0].direction, Direction::SendRecv);
    EXPECT_EQ(m.extmap[0].uri, "urn:ietf:params:rtp-hdrext:ssrc-audio-level");

    EXPECT_EQ(m.extmap[1].id, 2);
    EXPECT_EQ(m.extmap[1].direction, Direction::SendOnly);
    EXPECT_EQ(m.extmap[1].uri, "urn:ietf:params:rtp-hdrext:toffset");

    EXPECT_EQ(m.extmap[2].id, 3);
    EXPECT_EQ(m.extmap[2].direction, Direction::RecvOnly);
    EXPECT_EQ(m.extmap[2].uri, "urn:ietf:params:rtp-hdrext:abs-send-time");
}

// -----------------------------------------------------------------------------
// Parser — msid per-media (the bug we just fixed: msid must NOT clobber mid)
// -----------------------------------------------------------------------------
TEST(SdpParser, MsidPerMedia) {
    constexpr std::string_view text =
        "v=0\r\n"
        "o=- 0 0 IN IP4 0.0.0.0\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=mid:audio\r\n"
        "a=msid:stream-a track-audio\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
        "a=mid:video\r\n"
        "a=msid:stream-v track-video\r\n";

    Parser p;
    auto r = p.parse(text);
    ASSERT_TRUE(r.ok()) << r.error().message();
    const auto& sdp = r.value();

    ASSERT_EQ(sdp.media.size(), 2u);
    EXPECT_EQ(sdp.media[0].mid, "audio");      // ← mid NOT clobbered by msid anymore
    EXPECT_EQ(sdp.media[1].mid, "video");

    ASSERT_TRUE(sdp.media[0].msid.has_value());
    EXPECT_EQ(sdp.media[0].msid->stream_id, "stream-a");
    EXPECT_EQ(sdp.media[0].msid->track_id, "track-audio");
    ASSERT_TRUE(sdp.media[1].msid.has_value());
    EXPECT_EQ(sdp.media[1].msid->stream_id, "stream-v");
    EXPECT_EQ(sdp.media[1].msid->track_id, "track-video");

    // Positional mirror is also populated.
    ASSERT_EQ(sdp.msids.size(), 2u);
    EXPECT_EQ(sdp.msids[0].stream_id, "stream-a");
    EXPECT_EQ(sdp.msids[1].stream_id, "stream-v");
}

// -----------------------------------------------------------------------------
// Munger — minimum required lines
// -----------------------------------------------------------------------------
TEST(SdpMunger, MinimumRequiredLines) {
    SessionDescription sdp;
    sdp.version = 0;
    sdp.origin_username = "-";
    sdp.origin_session_id = "1234";
    sdp.origin_session_version = "1";
    sdp.origin_address = "127.0.0.1";
    sdp.session_name = "-";

    Munger m;
    auto r = m.to_sdp(sdp);
    ASSERT_TRUE(r.ok()) << r.error().message();
    const std::string& out = r.value();

    // RFC 4566 requires: v, o, s, t — exactly those four minimum lines.
    EXPECT_NE(out.find("v=0\r\n"), std::string::npos);
    EXPECT_NE(out.find("o=- 1234 1 IN IP4 127.0.0.1\r\n"), std::string::npos);
    EXPECT_NE(out.find("s=-\r\n"), std::string::npos);
    EXPECT_NE(out.find("t=0 0\r\n"), std::string::npos);
}

// -----------------------------------------------------------------------------
// Full parse → munger → parse round-trip
// -----------------------------------------------------------------------------
TEST(SdpRoundTrip, WebRtcOfferPreservesBundleAndMids) {
    constexpr std::string_view text =
        "v=0\r\n"
        "o=- 4962303335927620101 2 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "a=group:BUNDLE 0 1\r\n"
        "a=msid-semantic: WMS\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=ice-ufrag:abc\r\n"
        "a=ice-pwd:supersecretpwd\r\n"
        "a=fingerprint:sha-256 AA:BB:CC\r\n"
        "a=setup:actpass\r\n"
        "a=mid:0\r\n"
        "a=rtcp-mux\r\n"
        "a=sendonly\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=fmtp:111 minptime=10\r\n"
        "a=msid:stream-a track-audio\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=ice-ufrag:def\r\n"
        "a=ice-pwd:anothersecret\r\n"
        "a=fingerprint:sha-256 DD:EE:FF\r\n"
        "a=setup:actpass\r\n"
        "a=mid:1\r\n"
        "a=recvonly\r\n"
        "a=rtpmap:96 VP8/90000\r\n";

    Parser p;
    auto r1 = p.parse(text);
    ASSERT_TRUE(r1.ok()) << r1.error().message();

    Munger m;
    auto r2 = m.to_sdp(r1.value());
    ASSERT_TRUE(r2.ok()) << r2.error().message();
    const std::string emitted = r2.value();

    // Now parse it again.
    auto r3 = p.parse(emitted);
    ASSERT_TRUE(r3.ok()) << r3.error().message();
    const auto& sdp2 = r3.value();

    ASSERT_EQ(sdp2.bundle_mids.size(), 2u);
    EXPECT_EQ(sdp2.bundle_mids[0], "0");
    EXPECT_EQ(sdp2.bundle_mids[1], "1");
    EXPECT_EQ(sdp2.msid_semantic_token, "WMS");

    ASSERT_EQ(sdp2.media.size(), 2u);
    EXPECT_EQ(sdp2.media[0].type, MediaType::Audio);
    EXPECT_EQ(sdp2.media[0].mid, "0");
    EXPECT_EQ(sdp2.media[0].direction, Direction::SendOnly);
    EXPECT_EQ(sdp2.media[0].ice_ufrag, "abc");
    EXPECT_EQ(sdp2.media[0].ice_pwd, "supersecretpwd");
    EXPECT_EQ(sdp2.media[0].dtls_fingerprint_algo, "sha-256");
    EXPECT_EQ(sdp2.media[0].dtls_fingerprint_value, "AA:BB:CC");
    EXPECT_EQ(sdp2.media[0].dtls_setup, "actpass");
    EXPECT_EQ(sdp2.media[0].rtcp_mux_value, "rtcp-mux");
    EXPECT_EQ(sdp2.media[0].connection_address, "0.0.0.0");
    EXPECT_EQ(sdp2.media[0].address_family, 4);
    ASSERT_EQ(sdp2.media[0].formats.size(), 1u);
    EXPECT_EQ(sdp2.media[0].formats[0], "111");
    ASSERT_EQ(sdp2.media[0].rtpmap.size(), 1u);
    EXPECT_EQ(sdp2.media[0].rtpmap.at("111").encoding, "opus");
    EXPECT_EQ(sdp2.media[0].rtpmap.at("111").clock_rate, 48000u);
    EXPECT_EQ(sdp2.media[0].rtpmap.at("111").channels, 2u);
    ASSERT_EQ(sdp2.media[0].fmtp.size(), 1u);
    EXPECT_EQ(sdp2.media[0].fmtp.at("111"), "minptime=10");
    ASSERT_TRUE(sdp2.media[0].msid.has_value());
    EXPECT_EQ(sdp2.media[0].msid->stream_id, "stream-a");
    EXPECT_EQ(sdp2.media[0].msid->track_id, "track-audio");

    EXPECT_EQ(sdp2.media[1].type, MediaType::Video);
    EXPECT_EQ(sdp2.media[1].mid, "1");
    EXPECT_EQ(sdp2.media[1].direction, Direction::RecvOnly);
    EXPECT_EQ(sdp2.media[1].ice_ufrag, "def");
    EXPECT_EQ(sdp2.media[1].ice_pwd, "anothersecret");
    ASSERT_EQ(sdp2.media[1].rtpmap.size(), 1u);
    EXPECT_EQ(sdp2.media[1].rtpmap.at("96").encoding, "VP8");
    EXPECT_EQ(sdp2.media[1].rtpmap.at("96").clock_rate, 90000u);

    // Positional msids mirror still works: msids[i] lines up with media[i]
    // positionally even when media[i] has no a=msid: line (it just gets an
    // empty Msid{} entry).
    ASSERT_EQ(sdp2.msids.size(), 2u);
    EXPECT_EQ(sdp2.msids[0].stream_id, "stream-a");
    EXPECT_EQ(sdp2.msids[0].track_id,  "track-audio");
    // video had no a=msid: in the input → msids[1] is the empty entry.
    EXPECT_EQ(sdp2.msids[1].stream_id, "");
    EXPECT_EQ(sdp2.msids[1].track_id,  "");
}

// -----------------------------------------------------------------------------
// Debug: parse a real Chrome offer SDP (test_chrome_opus.html with recvonly
// audio transceiver).  This test prints diagnostic info so we can see whether
// the parser extracts media-level ICE creds, candidates, DTLS fingerprint, etc.
// -----------------------------------------------------------------------------
TEST(SdpDebug, ChromeOffer) {
    const char* chrome_offer =
        "v=0\r\n"
        "o=- 8388600061881731281 2 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "a=group:BUNDLE 0\r\n"
        "a=extmap-allow-mixed\r\n"
        "a=msid-semantic: WMS\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111 63 9 0 8 13 110 126\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=rtcp:9 IN IP4 0.0.0.0\r\n"
        "a=candidate:2442145044 1 udp 2113937151 d66fb4af-a6e4-430a-a915-aa7651fe79f2.local 64322 typ host generation 0 network-cost 999\r\n"
        "a=ice-ufrag:IeAH\r\n"
        "a=ice-pwd:qLnsb7NUyf1NZLO/fKV/ULqY\r\n"
        "a=ice-options:trickle\r\n"
        "a=fingerprint:sha-256 80:5A:E3:85:19:90:0F:E2:0C:A8:A5:B2:CA:9E:F7:B0:07:16:77:9D:CB:77:69:E1:E4:2F:D2:40:23:E9:84:A0\r\n"
        "a=setup:actpass\r\n"
        "a=mid:0\r\n"
        "a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level\r\n"
        "a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time\r\n"
        "a=recvonly\r\n"
        "a=rtcp-mux\r\n"
        "a=rtcp-rsize\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=rtcp-fb:111 transport-cc\r\n"
        "a=fmtp:111 minptime=10;useinbandfec=1\r\n"
        "a=rtpmap:63 red/48000/2\r\n";
    Parser p;
    auto result = p.parse(chrome_offer);
    ASSERT_TRUE(result) << "parse failed: " << result.error().message();
    auto& sd = result.value();
    std::printf("\n=== SdpDebug::ChromeOffer ===\n");
    std::printf("  version=%d\n", sd.version);
    std::printf("  media.size=%zu\n", sd.media.size());
    std::printf("  bundle_mids.size=%zu\n", sd.bundle_mids.size());
    std::printf("  dtls_setup='%s'\n", sd.dtls_setup.c_str());
    std::printf("  dtls_fp_algo='%s' value.len=%zu\n",
                sd.dtls_fingerprint_algo.c_str(),
                sd.dtls_fingerprint_value.size());
    for (size_t i = 0; i < sd.media.size(); ++i) {
        const auto& m = sd.media[i];
        std::printf("  media[%zu]: type=%s port=%d proto='%s'\n",
                    i, to_string(m.type), m.port, m.protocol.c_str());
        std::printf("    mid='%s' direction=%s\n",
                    m.mid.c_str(),
                    to_string(m.direction));
        std::printf("    ice_ufrag='%s' ice_pwd.len=%zu candidates=%zu\n",
                    m.ice_ufrag.c_str(), m.ice_pwd.size(), m.candidates.size());
        std::printf("    rtcp_mux_value='%s'\n", m.rtcp_mux_value.c_str());
        std::printf("    dtls_fp_algo='%s' value.len=%zu\n",
                    m.dtls_fingerprint_algo.c_str(),
                    m.dtls_fingerprint_value.size());
        std::printf("    rtpmap.size=%zu\n", m.rtpmap.size());
        std::printf("    connection='%s'\n", m.connection_address.c_str());
    }
    std::printf("==============================\n");
}

}  // namespace
