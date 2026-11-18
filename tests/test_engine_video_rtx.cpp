/**
 * @file tests/test_engine_video_rtx.cpp
 * @brief Engine-level test of the RFC 4588 retransmission stream
 *        (v1.3 plan Q3 / video plan T1).
 *
 * ## What this file covers
 *
 * The `rtx` module's tests cover the *format* (the two-octet OSN
 * header, the `apt=` parameter), and the video pipeline's tests cover
 * the *mechanism* (the sender frames a retransmission; the receiver
 * unwraps it and restores the original sequence number).
 *
 * What is left is the part only the engine can do: the **negotiation**
 * and the **wire**.
 *
 *   1. the offer advertises the retransmission stream — the
 *      `a=rtpmap:<pt> rtx/90000` line, the `a=fmtp:<pt> apt=<vpt>`
 *      line, and the `a=ssrc-group:FID` line that binds the
 *      retransmission SSRC to the media one;
 *   2. the answer echoes it (RFC 3264 §6.1);
 *   3. a peer that offers **no** retransmission stream keeps working
 *      exactly as before — the scope boundary the plan names;
 *   4. with the stream negotiated, a NACK is answered on the
 *      retransmission payload type, with the OSN header, the
 *      retransmission stream's *own* sequence number and its own SSRC;
 *   5. an inbound retransmission is unwrapped rather than decoded.
 *
 * The engine is driven through the same *fake ICE transport* the
 * feedback test uses: it reports `Connected`, captures every outbound
 * datagram, and can inject inbound ones the engine drains in `tick()`.
 *
 * No DTLS handshake completes (there is no peer), so SRTP is never
 * keyed and the packets travel in cleartext — which is exactly what
 * makes the wire format assertable here.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/ice_transport.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/plugins/video_pipeline.hpp>

namespace {

namespace np = nimrtc::plugins;
using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

std::uint16_t be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}

std::uint32_t be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

/** Is this datagram an RTP packet with the given payload type? */
bool is_rtp_with_pt(const std::vector<std::uint8_t>& p, std::uint8_t pt) noexcept {
    return p.size() >= 12 && (p[0] >> 6) == 2 && (p[1] & 0x7F) == pt;
}

/** Build a minimal RTP packet (12-byte header + payload). */
std::vector<std::uint8_t> make_rtp(std::uint32_t ssrc,
                                    std::uint16_t seq,
                                    std::uint32_t timestamp,
                                    std::uint8_t pt,
                                    std::vector<std::uint8_t> payload) {
    std::vector<std::uint8_t> out;
    out.reserve(12 + payload.size());
    out.push_back(0x80);
    out.push_back(pt & 0x7F);
    out.push_back(static_cast<std::uint8_t>((seq >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(seq & 0xFF));
    out.push_back(static_cast<std::uint8_t>((timestamp >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((timestamp >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((timestamp >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(timestamp & 0xFF));
    out.push_back(static_cast<std::uint8_t>((ssrc >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((ssrc >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((ssrc >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(ssrc & 0xFF));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

/** Generic NACK (RFC 4585 §6.2.1) with a single FCI entry (PID + BLP). */
std::vector<std::uint8_t> make_nack(std::uint32_t sender_ssrc,
                                    std::uint32_t media_ssrc,
                                    std::uint16_t pid,
                                    std::uint16_t blp) {
    std::vector<std::uint8_t> out;
    out.reserve(16);
    out.push_back(0x81);          // V=2, P=0, FMT=1 (Generic NACK)
    out.push_back(205);           // RTPFB
    out.push_back(0x00);          // length = 16/4 - 1 = 3
    out.push_back(0x03);
    const std::uint32_t ssrcs[2] = {sender_ssrc, media_ssrc};
    for (std::uint32_t s : ssrcs) {
        out.push_back(static_cast<std::uint8_t>((s >> 24) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((s >> 16) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((s >> 8) & 0xFF));
        out.push_back(static_cast<std::uint8_t>(s & 0xFF));
    }
    out.push_back(static_cast<std::uint8_t>((pid >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(pid & 0xFF));
    out.push_back(static_cast<std::uint8_t>((blp >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(blp & 0xFF));
    return out;
}

/** Does the SDP contain this line? */
bool has_line(const std::string& sdp, const std::string& needle) {
    return sdp.find(needle) != std::string::npos;
}

/** The payload type the `a=rtpmap:<pt> rtx/90000` line names. */
std::string rtx_pt_of(const std::string& sdp) {
    const std::string marker = "a=rtpmap:";
    const std::string body   = " rtx/90000";
    std::size_t pos = 0;
    while ((pos = sdp.find(marker, pos)) != std::string::npos) {
        const std::size_t start = pos + marker.size();
        const std::size_t end = sdp.find(' ', start);
        if (end == std::string::npos) break;
        const std::string pt = sdp.substr(start, end - start);
        if (sdp.compare(end, body.size(), body) == 0) return pt;
        pos = end;
    }
    return {};
}

// ---------------------------------------------------------------------------
// The fake ICE transport
// ---------------------------------------------------------------------------

class FakeIceTransport final : public np::IICETransport {
public:
    // ---- IPlugin ---------------------------------------------------------
    const char* name() const noexcept override { return "fake-ice"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    // ---- ITransport ------------------------------------------------------
    void set_callbacks(np::RecvCallback on_recv,
                       np::ErrorCallback on_error) noexcept override {
        on_recv_ = std::move(on_recv);
        on_error_ = std::move(on_error);
    }

    np::Status send(np::BufferView view, np::Addr = {}) noexcept override {
        std::vector<std::uint8_t> copy{};
        const std::uint8_t* data = view.data();
        const std::size_t size   = view.size();
        if (data && size) {
            copy.assign(data, data + size);
        }
        sent_.push_back(std::move(copy));
        return np::kOk;
    }

    int recv() noexcept override {
        int n = 0;
        while (!inbound_.empty()) {
            std::vector<std::uint8_t> pkt = std::move(inbound_.front());
            inbound_.erase(inbound_.begin());
            if (on_recv_) on_recv_(np::BufferView(pkt.data(), pkt.size()));
            ++n;
        }
        return n;
    }

    np::Addr local_addr() const noexcept override { return {}; }
    np::Addr remote_addr() const noexcept override { return {}; }

    np::IceState state() const noexcept override { return np::IceState::Connected; }
    std::string local_ufrag() const noexcept override { return "fakeufrag"; }
    std::string local_password() const noexcept override { return "fakepassword"; }
    bool wait_for_gathering(int) noexcept override { return true; }
    std::vector<std::string> gathered_local_candidates() const noexcept override {
        return {};
    }
    np::Status set_remote_description(std::string_view) noexcept override {
        return np::kOk;
    }
    np::Status add_remote_candidate(std::string_view) noexcept override {
        return np::kOk;
    }
    void set_bind_address(std::string_view) noexcept override {}
    void set_stun_server(std::string_view, std::uint16_t) noexcept override {}
    void set_local_port_range(std::uint16_t, std::uint16_t) noexcept override {}
    void add_turn_server(std::string_view,
                         std::uint16_t,
                         std::string_view,
                         std::string_view) noexcept override {}
    void set_bwe(np::IBwe*) noexcept override {}
    void set_scheduler(np::IScheduler*) noexcept override {}

    // ---- Test helpers ----------------------------------------------------
    void queue_inbound(std::vector<std::uint8_t> pkt) {
        inbound_.push_back(std::move(pkt));
    }
    const std::vector<std::vector<std::uint8_t>>& sent() const noexcept {
        return sent_;
    }
    void clear_sent() { sent_.clear(); }

private:
    np::RecvCallback   on_recv_;
    np::ErrorCallback  on_error_;
    std::vector<std::vector<std::uint8_t>> inbound_;
    std::vector<std::vector<std::uint8_t>> sent_;
};

class FakeIceFactory final : public np::IICETransportFactory {
public:
    std::string_view id() const noexcept override { return "fake-ice"; }
    std::string_view display_name() const noexcept override {
        return "Fake ICE transport (test)";
    }
    np::IICETransport* create_ice() const override {
        auto* t = new FakeIceTransport();
        last_created() = t;
        return t;
    }
    static FakeIceTransport*& last_created() {
        static FakeIceTransport* instance = nullptr;
        return instance;
    }
};

// ---------------------------------------------------------------------------
// A minimal video codec
// ---------------------------------------------------------------------------

class FakeVideoCodec final : public np::IVideoCodec {
public:
    const char* name() const noexcept override { return "rtx-test-codec"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    bool can_encode() const noexcept override { return true; }
    bool can_decode() const noexcept override { return true; }
    np::VideoCodecKind kind() const noexcept override {
        return np::VideoCodecKind::kH264;
    }
    std::string_view codec_name() const noexcept override { return "rtx-test"; }

    np::Status encode(const np::VideoFrame&,
                       std::uint8_t* output,
                       std::size_t output_capacity,
                       np::EncodedVideoFrame& encoded_out) noexcept override {
        static constexpr std::uint8_t kAnnexBIdr[] = {0x00, 0x00, 0x00, 0x01, 0x65};
        if (!output || output_capacity < sizeof(kAnnexBIdr)) return np::kErrBufferTooSmall;
        std::memcpy(output, kAnnexBIdr, sizeof(kAnnexBIdr));
        encoded_out.payload = nimrtc::core::ByteSpan{output, sizeof(kAnnexBIdr)};
        encoded_out.codec = np::VideoCodecKind::kH264;
        encoded_out.is_keyframe = true;
        encoded_out.nalu_format = np::NaluFormat::kAnnexBStartCode;
        encoded_out.payload_type = payload_type();
        return np::kOk;
    }

    np::Status decode(const np::EncodedVideoFrame&,
                      np::VideoFrame&,
                      std::uint8_t* const*) noexcept override {
        ++decode_calls_;
        return np::kOk;
    }

    np::Status force_keyframe() noexcept override { return np::kOk; }

    np::VideoCodecConfig config() const noexcept override { return cfg_; }
    np::Status update_config(np::VideoCodecConfig cfg) noexcept override {
        cfg_ = cfg;
        return np::kOk;
    }
    np::VideoCodecStats stats() const noexcept override { return {}; }
    std::uint8_t payload_type() const noexcept override { return 102; }

    int decode_calls() const noexcept {
        return decode_calls_.load(std::memory_order_relaxed);
    }

private:
    np::VideoCodecConfig cfg_{};
    std::atomic<int>     decode_calls_{0};
};

class FakeVideoCodecFactory final : public np::IVideoCodecFactory {
public:
    std::string_view id() const noexcept override { return "rtx-test-codec"; }
    std::string_view display_name() const noexcept override {
        return "RTX test video codec";
    }
    np::IVideoCodec* create(np::VideoCodecConfig) const override {
        auto* c = new FakeVideoCodec();
        last_created() = c;
        return c;
    }
    static FakeVideoCodec*& last_created() {
        static FakeVideoCodec* instance = nullptr;
        return instance;
    }
};

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

constexpr std::uint32_t kPeerVideoSsrc = 0x11223344u;
constexpr std::uint32_t kPeerRtxSsrc  = 0x11223345u;

class EngineVideoRtx : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();

        static FakeIceFactory ice_factory;
        PluginRegistry::instance().register_ice_transport("fake-ice", &ice_factory);

        static FakeVideoCodecFactory codec_factory;
        PluginRegistry::instance().register_video_codec("rtx-test-codec", &codec_factory);
    }

    EngineConfig make_config() {
        EngineConfig cfg{};
        cfg.transport_name = "fake-ice";
        cfg.video_receiver_name = "reference";
        cfg.video_sender_name   = "reference";
        cfg.video_codec_name    = "rtx-test-codec";
        cfg.datachannel_backend = "";
        cfg.bwe_name            = "";
        cfg.scheduler_config.enabled = false;
        return cfg;
    }

    /** A peer's offer that advertises a retransmission stream.
     *
     *  The shape is Chrome's: the media payload type, the
     *  retransmission payload type with its `apt=`, and the
     *  `a=ssrc-group:FID` line that binds the two SSRCs.
     */
    static std::string peer_offer_with_rtx(std::uint8_t media_pt,
                                           std::uint8_t rtx_pt) {
        std::string sdp;
        sdp += "v=0\r\n";
        sdp += "o=- 4611731400430051336 2 IN IP4 127.0.0.1\r\n";
        sdp += "s=-\r\n";
        sdp += "t=0 0\r\n";
        sdp += "a=group:BUNDLE 0\r\n";
        sdp += "m=video 9 UDP/TLS/RTP/SAVPF ";
        sdp += std::to_string(media_pt);
        sdp += " ";
        sdp += std::to_string(rtx_pt);
        sdp += "\r\n";
        sdp += "c=IN IP4 0.0.0.0\r\n";
        sdp += "a=mid:0\r\n";
        sdp += "a=rtcp-mux\r\n";
        sdp += "a=rtpmap:" + std::to_string(media_pt) + " H264/90000\r\n";
        sdp += "a=fmtp:" + std::to_string(media_pt) +
               " level-asymmetry-allowed=1;packetization-mode=1;"
               "profile-level-id=42e01f\r\n";
        sdp += "a=rtpmap:" + std::to_string(rtx_pt) + " rtx/90000\r\n";
        sdp += "a=fmtp:" + std::to_string(rtx_pt) + " apt=" +
               std::to_string(media_pt) + ";rtx-time=200\r\n";
        sdp += "a=rtcp-fb:" + std::to_string(media_pt) + " nack\r\n";
        sdp += "a=rtcp-fb:" + std::to_string(media_pt) + " nack pli\r\n";
        sdp += "a=ssrc:" + std::to_string(kPeerVideoSsrc) + " cname:peer\r\n";
        sdp += "a=ssrc-group:FID " + std::to_string(kPeerVideoSsrc) + " " +
               std::to_string(kPeerRtxSsrc) + "\r\n";
        return sdp;
    }

    /** A peer's offer that advertises **no** retransmission stream. */
    static std::string peer_offer_without_rtx(std::uint8_t media_pt) {
        std::string sdp;
        sdp += "v=0\r\n";
        sdp += "o=- 4611731400430051336 2 IN IP4 127.0.0.1\r\n";
        sdp += "s=-\r\n";
        sdp += "t=0 0\r\n";
        sdp += "a=group:BUNDLE 0\r\n";
        sdp += "m=video 9 UDP/TLS/RTP/SAVPF ";
        sdp += std::to_string(media_pt);
        sdp += "\r\n";
        sdp += "c=IN IP4 0.0.0.0\r\n";
        sdp += "a=mid:0\r\n";
        sdp += "a=rtcp-mux\r\n";
        sdp += "a=rtpmap:" + std::to_string(media_pt) + " H264/90000\r\n";
        sdp += "a=fmtp:" + std::to_string(media_pt) +
               " level-asymmetry-allowed=1;packetization-mode=1;"
               "profile-level-id=42e01f\r\n";
        sdp += "a=rtcp-fb:" + std::to_string(media_pt) + " nack\r\n";
        sdp += "a=ssrc:" + std::to_string(kPeerVideoSsrc) + " cname:peer\r\n";
        return sdp;
    }
};

// ---------------------------------------------------------------------------
// The offer advertises the retransmission stream
// ---------------------------------------------------------------------------

TEST_F(EngineVideoRtx, the_offer_advertises_the_retransmission_stream) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::string offer = engine.create_offer();
    ASSERT_FALSE(offer.empty());

    // The payload type itself.
    const std::string rtx_pt = rtx_pt_of(offer);
    ASSERT_FALSE(rtx_pt.empty())
        << "the offer carries no `a=rtpmap:<pt> rtx/90000` line:\n" << offer;

    // The association: the `apt=` names the media payload type.
    EXPECT_TRUE(has_line(offer, "a=fmtp:" + rtx_pt + " apt="))
        << "the offer carries no `apt=` for the retransmission payload type:\n"
        << offer;

    // The media payload type the `apt=` names must be one we offered.
    const std::size_t apt_pos = offer.find("apt=");
    ASSERT_NE(apt_pos, std::string::npos);
    std::string apt;
    for (std::size_t i = apt_pos + 4; i < offer.size(); ++i) {
        const char c = offer[i];
        if (c < '0' || c > '9') break;
        apt.push_back(c);
    }
    ASSERT_FALSE(apt.empty());
    EXPECT_TRUE(has_line(offer, "a=rtpmap:" + apt + " H264/90000"))
        << "the apt= names a payload type the offer does not declare";

    // The FID group: the media SSRC first, the retransmission SSRC
    // second (the WebRTC convention — Chrome, pion, libwebrtc).
    const std::uint32_t media_ssrc = cfg.video_sender_tuning.ssrc;
    EXPECT_TRUE(has_line(offer, "a=ssrc-group:FID " + std::to_string(media_ssrc)))
        << "the offer carries no FID group for the media SSRC:\n" << offer;

    engine.close();
}

TEST_F(EngineVideoRtx, the_offer_gives_the_retransmission_stream_its_own_ssrc) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::string offer = engine.create_offer();

    const std::uint32_t media_ssrc = cfg.video_sender_tuning.ssrc;
    // RFC 4588 §5.2: the retransmission SSRC MUST differ from the media
    // one.  The engine derives it, so it is the media SSRC plus one.
    const std::uint32_t rtx_ssrc = media_ssrc + 1u;
    EXPECT_NE(rtx_ssrc, media_ssrc);

    // The group names both, and the retransmission SSRC has its own
    // `a=ssrc:` line (RFC 5576 §4.2).
    EXPECT_TRUE(has_line(offer, "a=ssrc-group:FID " + std::to_string(media_ssrc) +
                                " " + std::to_string(rtx_ssrc)))
        << "the FID group does not name both SSRCs:\n" << offer;
    EXPECT_TRUE(has_line(offer, "a=ssrc:" + std::to_string(rtx_ssrc) + " cname:"))
        << "the retransmission SSRC has no `a=ssrc:` line of its own:\n" << offer;

    engine.close();
}

TEST_F(EngineVideoRtx, the_retransmission_payload_type_does_not_collide) {
    EngineConfig cfg = make_config();
    // A media payload type in the dynamic range, so the RTX one has to
    // walk past it.
    cfg.video_sender_tuning.payload_type = 96;
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::string offer = engine.create_offer();
    const std::string rtx_pt = rtx_pt_of(offer);
    ASSERT_FALSE(rtx_pt.empty());

    EXPECT_NE(rtx_pt, "96")
        << "the retransmission payload type collided with the media one";

    // And the `apt=` must name the media payload type, not the RTX one.
    EXPECT_TRUE(has_line(offer, "a=fmtp:" + rtx_pt + " apt=96"))
        << "the apt= does not name the media payload type:\n" << offer;

    engine.close();
}

// ---------------------------------------------------------------------------
// The answer echoes the offer's retransmission stream
// ---------------------------------------------------------------------------

TEST_F(EngineVideoRtx, the_answer_echoes_the_retransmission_stream) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The peer offers a retransmission stream on payload type 103.
    const auto answer = engine.process_remote_sdp(peer_offer_with_rtx(102, 103));
    ASSERT_TRUE(answer.has_value());

    const std::string& sdp = *answer;
    EXPECT_TRUE(has_line(sdp, "a=rtpmap:103 rtx/90000"))
        << "the answer dropped the peer's `a=rtpmap:` line:\n" << sdp;
    EXPECT_TRUE(has_line(sdp, "a=fmtp:103 apt=102"))
        << "the answer dropped the `apt=` association:\n" << sdp;

    // The answer's FID group names *our* two SSRCs.
    const std::uint32_t media_ssrc = cfg.video_sender_tuning.ssrc;
    EXPECT_TRUE(has_line(sdp, "a=ssrc-group:FID " + std::to_string(media_ssrc)))
        << "the answer carries no FID group:\n" << sdp;

    engine.close();
}

TEST_F(EngineVideoRtx, a_peer_without_a_retransmission_stream_gets_none) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The peer offers no RTX: the scope boundary the plan names.
    const auto answer = engine.process_remote_sdp(peer_offer_without_rtx(102));
    ASSERT_TRUE(answer.has_value());

    const std::string& sdp = *answer;
    EXPECT_FALSE(has_line(sdp, "rtx/90000"))
        << "the answer advertised a retransmission stream the peer never "
           "offered:\n" << sdp;
    EXPECT_FALSE(has_line(sdp, "a=ssrc-group:FID"))
        << "the answer advertised an FID group with no retransmission "
           "stream:\n" << sdp;

    engine.close();
}

// ---------------------------------------------------------------------------
// The wire: a NACK is answered on the retransmission payload type
// ---------------------------------------------------------------------------

TEST_F(EngineVideoRtx, a_nack_is_answered_on_the_retransmission_payload_type) {
    EngineConfig cfg = make_config();
    // Pin the media stream's initial sequence number so the two sequence
    // spaces are *distinguishable* on the wire.  Both counters start at
    // zero by default, and a single "the RTX sequence differs from the
    // original's" assertion would then pass or fail by coincidence
    // rather than by design.
    cfg.video_sender_tuning.initial_seq = 1000;
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // Negotiate the retransmission stream with the peer.
    const auto answer = engine.process_remote_sdp(peer_offer_with_rtx(102, 103));
    ASSERT_TRUE(answer.has_value());

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ice->clear_sent();

    // Push one encoded frame through the sender so it lands in the
    // retransmission cache and goes out on the wire.
    np::EncodedVideoFrame frame{};
    frame.codec        = np::VideoCodecKind::kH264;
    frame.payload_type = cfg.video_sender_tuning.payload_type;
    frame.is_keyframe  = true;
    frame.nalu_format  = np::NaluFormat::kAnnexBStartCode;
    static const std::uint8_t kIdrSlice[] = {
        0x00, 0x00, 0x00, 0x01,
        0x65,
        0x88, 0x84, 0x00, 0x33,
    };
    frame.payload = nimrtc::core::ByteSpan{kIdrSlice, sizeof(kIdrSlice)};
    frame.info.capture_ts_us = 5000;
    frame.info.frame_seq     = 1;
    frame.info.rtp_timestamp = 90000;

    ASSERT_EQ(engine.video_sender()->push_frame(frame, 90000), np::kOk);

    // Find the original on the wire.
    const std::uint8_t media_pt = 102;
    const std::uint8_t rtx_pt   = 103;
    const std::vector<std::uint8_t>* original = nullptr;
    for (const auto& p : ice->sent()) {
        if (is_rtp_with_pt(p, media_pt)) { original = &p; break; }
    }
    ASSERT_NE(original, nullptr) << "the sender did not emit an RTP packet";

    const std::uint16_t orig_seq = be16(original->data() + 2);
    const std::uint32_t orig_ts  = be32(original->data() + 4);
    const bool orig_marker       = (original->data()[1] & 0x80) != 0;

    // The peer asks for that packet back.
    const auto nack = make_nack(/*sender_ssrc=*/0x0BADF00D,
                                 /*media_ssrc=*/cfg.video_sender_tuning.ssrc,
                                 /*pid=*/orig_seq,
                                 /*blp=*/0);
    engine.feed_media_inbound(nack.data(), nack.size());

    EXPECT_EQ(engine.video_sender()->stats().rtx_packets_emitted, 1u)
        << "the sender did not answer the NACK with an RFC 4588 "
           "retransmission";

    // The retransmission went out on the retransmission payload type.
    const std::vector<std::uint8_t>* rtx = nullptr;
    for (const auto& p : ice->sent()) {
        if (is_rtp_with_pt(p, rtx_pt)) { rtx = &p; break; }
    }
    ASSERT_NE(rtx, nullptr)
        << "no packet went out on the retransmission payload type (" +
           std::to_string(rtx_pt) + ")";

    // The OSN header carries the original's sequence number, in network
    // byte order (RFC 4588 §4.1).
    ASSERT_GE(rtx->size(), 12u + 2u);
    EXPECT_EQ(be16(rtx->data() + 12), orig_seq)
        << "the OSN header does not carry the original's sequence number";

    // The RTP header carries the *original's* timestamp and marker bit
    // (RFC 4588 §4.1).
    EXPECT_EQ(be32(rtx->data() + 4), orig_ts)
        << "the retransmission's timestamp is not the original's";
    EXPECT_EQ((rtx->data()[1] & 0x80) != 0, orig_marker)
        << "the retransmission's marker bit is not the original's";

    // The sequence number is the retransmission stream's *own* — not the
    // original's (RFC 4588 §4: the retransmission stream has its own
    // sequence number space).
    EXPECT_NE(be16(rtx->data() + 2), orig_seq)
        << "the retransmission reused the original's sequence number — "
           "RFC 4588 §4 gives the retransmission stream its own space";
    EXPECT_EQ(be16(rtx->data() + 2), 0u)
        << "the retransmission stream did not start its own sequence space "
           "at its own counter";

    // And the SSRC is the retransmission stream's own.
    EXPECT_EQ(be32(rtx->data() + 8), cfg.video_sender_tuning.ssrc + 1u)
        << "the retransmission did not use the retransmission SSRC";

    engine.close();
}

TEST_F(EngineVideoRtx, without_a_negotiated_stream_the_original_is_re_sent) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The peer offers NO retransmission stream.
    const auto answer = engine.process_remote_sdp(peer_offer_without_rtx(102));
    ASSERT_TRUE(answer.has_value());

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ice->clear_sent();

    np::EncodedVideoFrame frame{};
    frame.codec        = np::VideoCodecKind::kH264;
    frame.payload_type = cfg.video_sender_tuning.payload_type;
    frame.is_keyframe  = true;
    frame.nalu_format  = np::NaluFormat::kAnnexBStartCode;
    static const std::uint8_t kIdrSlice[] = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88};
    frame.payload = nimrtc::core::ByteSpan{kIdrSlice, sizeof(kIdrSlice)};
    frame.info.capture_ts_us = 5000;
    frame.info.frame_seq     = 1;
    frame.info.rtp_timestamp = 90000;

    ASSERT_EQ(engine.video_sender()->push_frame(frame, 90000), np::kOk);

    const std::uint8_t media_pt = 102;
    const std::vector<std::uint8_t>* original = nullptr;
    for (const auto& p : ice->sent()) {
        if (is_rtp_with_pt(p, media_pt)) { original = &p; break; }
    }
    ASSERT_NE(original, nullptr);
    const std::uint16_t orig_seq = be16(original->data() + 2);

    const auto nack = make_nack(/*sender_ssrc=*/0x0BADF00D,
                                 /*media_ssrc=*/cfg.video_sender_tuning.ssrc,
                                 /*pid=*/orig_seq,
                                 /*blp=*/0);
    engine.feed_media_inbound(nack.data(), nack.size());

    // The pre-RTX behaviour: the original packet is re-sent, with its
    // own sequence number, on the media payload type.
    EXPECT_EQ(engine.video_sender()->stats().rtx_packets_emitted, 0u)
        << "the sender took the RFC 4588 path without a negotiated "
           "retransmission stream";
    EXPECT_EQ(engine.video_sender()->stats().packets_retransmitted, 1u);

    std::size_t same_seq = 0;
    for (const auto& p : ice->sent()) {
        if (is_rtp_with_pt(p, media_pt) && be16(p.data() + 2) == orig_seq) ++same_seq;
    }
    EXPECT_EQ(same_seq, 2u)
        << "expected the original + the re-sent original on the wire";

    engine.close();
}

// ---------------------------------------------------------------------------
// The wire: an inbound retransmission is unwrapped, not decoded
// ---------------------------------------------------------------------------

TEST_F(EngineVideoRtx, an_inbound_retransmission_is_unwrapped_not_decoded) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // Negotiate the retransmission stream with the peer.
    const auto answer = engine.process_remote_sdp(peer_offer_with_rtx(102, 103));
    ASSERT_TRUE(answer.has_value());

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ice->clear_sent();

    FakeVideoCodec* codec = FakeVideoCodecFactory::last_created();
    ASSERT_NE(codec, nullptr);
    const int decode_before = codec->decode_calls();

    // The peer sends a media packet, then a retransmission of it: the
    // OSN header (0x00 0x64 = sequence 100) followed by the original's
    // payload.
    const std::uint8_t media_pt = 102;
    const std::uint8_t rtx_pt   = 103;
    ice->queue_inbound(make_rtp(kPeerVideoSsrc, 100, 90000, media_pt,
                                {0x65, 0x88}));
    ice->queue_inbound(make_rtp(kPeerRtxSsrc, 500, 90000, rtx_pt,
                                {0x00, 0x64, 0x65, 0x88}));
    engine.tick();

    EXPECT_EQ(engine.video_sender()->stats().rtx_packets_emitted, 0u);

    // The receiver counted the retransmission.
    const auto rx_stats = engine.video_receiver()->stats();
    EXPECT_EQ(rx_stats.rtx_packets_received, 1u)
        << "the receiver did not unwrap the retransmission";
    EXPECT_EQ(rx_stats.rtx_packets_malformed, 0u);

    // And the *retransmission* stream's sequence number never entered the
    // media stream's sequence space: the packet was unwrapped, so the
    // media stream saw only sequence 100.
    EXPECT_EQ(rx_stats.packets_pushed, 2u)
        << "the receiver did not push both the original and the "
           "retransmission into the jitter buffer";

    engine.close();
}

} // anonymous namespace
