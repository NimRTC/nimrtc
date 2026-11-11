/**
 * @file tests/test_engine_xr_tmmbr.cpp
 * @brief v1.2.0 (session capability plan X2 / X4 / X5) — the engine's
 *        *outbound* RTCP XR / TMMBR plane.
 *
 * The wire formats have their own module tests
 * (`src/modules/rtp/tests/test_rtcp_xr.cpp` covers the XR blocks and the
 * TMMBR / TMMBN round-trip) and the SDP negotiation has its own engine
 * tests (`tests/test_engine_tmmbr.cpp`).  What neither covers is the
 * part that actually matters to a peer: **does the engine put the
 * packets on the wire, and do they carry the right numbers?**
 *
 * Covered:
 *   X-T1 - the engine emits an XR report (PT 207) carrying a Receiver
 *          Reference Time block (BT = 4) once the peer asked for
 *          "rcvr-rtt"
 *   X-T2 - the XR report is silent until the peer named the format
 *          (RFC 3611 §5.1: "participants SHOULD NOT send XR blocks other
 *          than the ones indicated by the parameters")
 *   X-T3 - the DLRR block (BT = 5) answers the peer's own Receiver
 *          Reference Time report, with the right SSRC / LRR / DLRR
 *   X-T4 - the Statistics Summary block (BT = 6) reports the inbound
 *          audio stream's loss and jitter, and only the field
 *          indicators the peer asked for
 *   T-T1 - the engine emits a TMMBR (RTPFB, FMT = 3) whose bit rate is
 *          the live bandwidth estimate (X4: "the rate we report is the
 *          rate we are using")
 *   T-T2 - the TMMBR is silent unless "ccm tmmbr" was negotiated
 *          (RFC 5104 §7.2 — the joint subset rule)
 *   T-T3 - `disable_tmmbr` vetoes the outbound TMMBR even when the peer
 *          negotiated it (X5)
 *   T-T4 - an inbound TMMBR is answered with a TMMBN (RTPFB, FMT = 4)
 *          whose FCI entry names the *owner* of the bounding tuple
 *          (RFC 5104 §4.2.2.1 / §4.2.2.2)
 *
 * The engine is driven through a **fake ICE transport** registered under
 * the id "fake-ice" — the same pattern `test_engine_audio_stats.cpp`
 * uses.  The fake reports `Connected`, so the engine's periodic work
 * runs, and it captures every outbound datagram, which is what makes the
 * wire format assertable.  No DTLS handshake completes, so SRTP is never
 * keyed and the RTCP travels in cleartext — exactly what makes the wire
 * format assertable.  The SRTCP protection path is covered by the SRTP
 * module's own tests.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/ice_transport.hpp>
#include <nimrtc/rtp/packet.hpp>
#include <nimrtc/rtp/rtcp_tmmbr.hpp>
#include <nimrtc/rtp/rtcp_xr.hpp>

namespace {

namespace np = nimrtc::plugins;

using nimrtc::core::ByteBuffer;
using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
using namespace std::chrono_literals;

/** The payload type RFC 7587 assigns to Opus, and the one the engine
 *  advertises by default. */
constexpr std::uint8_t kOpusPt = 111;

/** The SSRC the fake peer uses for its audio stream. */
constexpr std::uint32_t kPeerAudioSsrc = 0x0A0A0A0Au;

// ---------------------------------------------------------------------------
// Fake ICE transport
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
        on_recv_  = std::move(on_recv);
        on_error_ = std::move(on_error);
    }

    np::Status send(np::BufferView view, np::Addr = {}) noexcept override {
        sent_.emplace_back(view.data(), view.data() + view.size());
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

    // ---- IICETransport ---------------------------------------------------
    np::IceState state() const noexcept override {
        return np::IceState::Connected;
    }
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
    void add_turn_server(std::string_view, std::uint16_t, std::string_view,
                         std::string_view) noexcept override {}
    void set_bwe(np::IBwe*) noexcept override {}
    void set_scheduler(np::IScheduler*) noexcept override {}

    // ---- Test helpers ----------------------------------------------------
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
// The fixture
// ---------------------------------------------------------------------------

class EngineXrTmmbr : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();

        static FakeIceFactory ice_factory;
        PluginRegistry::instance().register_ice_transport("fake-ice", &ice_factory);
    }

    EngineConfig make_config() {
        EngineConfig cfg{};
        // The fake ICE stands in for the real transport: it reports
        // Connected (so the engine's periodic work runs) and captures
        // every outbound datagram.
        cfg.transport_name           = "fake-ice";
        cfg.datachannel_backend      = "";
        cfg.scheduler_config.enabled = false;
        // The AIMD estimator's default initial target — the number the
        // TMMBR has to report (X4).
        cfg.bwe_name                 = "aimd";
        cfg.bwe_config.initial_bitrate_bps = 500'000;
        // A reproducible SSRC for the engine's own audio source.  The
        // inbound TMMBR names *our* stream (RFC 5104 §4.2.1.2 — the
        // tuple is about the media source it applies to), so the test
        // needs to know the value the engine sends under.  The default
        // is drawn at random; pinning it here keeps the test
        // deterministic.
        cfg.audio_ssrc = 0x1234ABCDu;
        return cfg;
    }

    // ---- Wire helpers ----------------------------------------------------

    /// Find the first captured datagram that is an RTCP XR packet
    /// (RFC 3611 §2: payload type 207).
    static const std::vector<std::uint8_t>* find_xr(
        const std::vector<std::vector<std::uint8_t>>& sent) {
        for (const auto& p : sent) {
            if (p.size() >= 4 && p[1] == nimrtc::rtp::kXrPayloadType) return &p;
        }
        return nullptr;
    }

    /// Find the first captured datagram that is an RTPFB packet with the
    /// given FMT (RFC 4585 §6.1: payload type 205).
    static const std::vector<std::uint8_t>* find_rtpfb(
        const std::vector<std::vector<std::uint8_t>>& sent, std::uint8_t fmt) {
        for (const auto& p : sent) {
            if (p.size() >= 2 && p[1] == 205 && (p[0] & 0x1Fu) == fmt) return &p;
        }
        return nullptr;
    }

    static bool is_xr(const std::vector<std::uint8_t>& p) noexcept {
        return p.size() >= 4 && p[1] == nimrtc::rtp::kXrPayloadType;
    }

    static bool is_rtpfb(const std::vector<std::uint8_t>& p,
                         std::uint8_t fmt) noexcept {
        return p.size() >= 2 && p[1] == 205 && (p[0] & 0x1Fu) == fmt;
    }

    /// Build an RTP packet carrying an Opus frame — the engine's audio
    /// receive path needs a decodable payload to record the stream.
    static ByteBuffer make_opus_rtp(std::uint32_t ssrc, std::uint8_t pt,
                                    std::uint32_t seq, std::uint32_t rtp_ts) {
        const auto* factory =
            nimrtc::core::PluginRegistry::instance().get_codec("opus");
        if (!factory) return {};

        nimrtc::plugins::CodecConfig cc{};
        cc.sample_rate_hz = 48000;
        cc.channels       = 1;
        cc.bitrate_bps    = 64000;
        cc.complexity     = 10;
        cc.payload_type   = pt;
        cc.name           = "opus";

        auto enc = factory->create(cc);
        if (!enc) return {};
        if (enc->open() != np::kOk) return {};

        std::vector<float> pcm(960, 0.25f);
        std::vector<std::uint8_t> bytes(4096);
        const std::size_t n = enc->encode(pcm.data(), 960, bytes.data(),
                                          bytes.size());
        if (n == 0) return {};

        nimrtc::rtp::PacketBuilder pb;
        pb.set_ssrc(ssrc)
          .set_seq(static_cast<std::uint16_t>(seq))
          .set_timestamp(rtp_ts)
          .set_payload_type(pt);
        pb.set_payload(nimrtc::core::ByteSpan(bytes.data(), n));
        return pb.build();
    }

    /// Feed one inbound audio packet so the engine knows the peer's
    /// media source (needed by the Statistics Summary block and by the
    /// TMMBR's FCI entry).
    static void feed_audio(NimRTCEngine& engine) {
        const auto pkt = make_opus_rtp(kPeerAudioSsrc, kOpusPt, 1, 0);
        if (!pkt.empty()) engine.feed_media_inbound(pkt.data(), pkt.size());
    }

    /// Let the engine's periodic work run for a while.
    ///
    /// The XR report and the TMMBR both ride a one-second cadence
    /// (RFC 3611 §5.2 / RFC 5104 §4.2.1.3), so a test has to wait for
    /// the first slot to open.
    static void run_for(NimRTCEngine& engine, std::chrono::milliseconds span) {
        const auto deadline = std::chrono::steady_clock::now() + span;
        while (std::chrono::steady_clock::now() < deadline) {
            engine.tick();
            std::this_thread::sleep_for(20ms);
        }
    }
};

// ===========================================================================
// X-T1 / X-T2 — the Receiver Reference Time block (RFC 3611 §4.4)
// ===========================================================================

TEST_F(EngineXrTmmbr, TheXrReportCarriesAReceiverReferenceTimeBlock) {
    EngineConfig cfg = make_config();
    cfg.rtcp_xr_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ice->clear_sent();

    // The peer offers "rcvr-rtt", which authorises us to send the
    // initiator (RFC 3611 §5.2).
    const std::string peer_offer =
        "v=0\r\n"
        "o=- 1 1 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=rtcp-xr:rcvr-rtt=all\r\n";
    const auto answer = engine.process_remote_sdp(peer_offer);
    ASSERT_TRUE(answer.has_value());

    ice->clear_sent();
    run_for(engine, 1500ms);

    const auto* xr = find_xr(ice->sent());
    ASSERT_NE(xr, nullptr)
        << "the engine did not put an XR report on the wire";

    const auto parsed = nimrtc::rtp::parse_xr_packet(
        nimrtc::core::ByteSpan(xr->data(), xr->size()));
    ASSERT_TRUE(parsed.ok());
    ASSERT_FALSE(parsed.value().blocks.empty());

    bool found_rrtr = false;
    for (const auto& b : parsed.value().blocks) {
        if (b.type != nimrtc::rtp::XrBlockType::kReceiverReferenceTime) continue;
        found_rrtr = true;
        EXPECT_GT(b.receiver_reference_time.ntp_timestamp, 0u)
            << "RFC 3611 §4.4: the block carries the wall-clock instant at "
               "which it was sent";
    }
    EXPECT_TRUE(found_rrtr)
        << "RFC 3611 §4.4: the Receiver Reference Time block (BT = 4) is "
           "the initiator of the round-trip measurement";

    // The report rides the RTCP cadence (RFC 3611 §5.2), so a 1.5 s
    // window may see more than one report — what matters is that the
    // first one was sent at all.
    EXPECT_GE(engine.xr_reports_sent_count(), 1u);
}

TEST_F(EngineXrTmmbr, NoXrReportWhenThePeerNeverNamedTheFormat) {
    // RFC 3611 §5.1: "participants SHOULD NOT send XR blocks other than
    // the ones indicated by the parameters".  A peer that never wrote
    // "rtcp-xr" gets no XR from us.
    EngineConfig cfg = make_config();
    cfg.rtcp_xr_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ice->clear_sent();

    const std::string peer_offer =
        "v=0\r\n"
        "o=- 1 1 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "a=rtpmap:111 opus/48000/2\r\n";
    const auto answer = engine.process_remote_sdp(peer_offer);
    ASSERT_TRUE(answer.has_value());

    ice->clear_sent();
    run_for(engine, 1500ms);

    for (const auto& p : ice->sent()) {
        EXPECT_FALSE(is_xr(p))
            << "an XR block may only be sent when the peer named the "
               "format (RFC 3611 §5.1)";
    }
    EXPECT_EQ(engine.xr_reports_sent_count(), 0u);
}

TEST_F(EngineXrTmmbr, NoXrReportWhenTheFeatureIsOff) {
    EngineConfig cfg = make_config();
    cfg.rtcp_xr_enabled = false;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ice->clear_sent();

    const std::string peer_offer =
        "v=0\r\n"
        "o=- 1 1 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=rtcp-xr:rcvr-rtt=all\r\n";
    const auto answer = engine.process_remote_sdp(peer_offer);
    ASSERT_TRUE(answer.has_value());

    ice->clear_sent();
    run_for(engine, 1500ms);

    for (const auto& p : ice->sent()) {
        EXPECT_FALSE(is_xr(p))
            << "XR is opt-in: a host that did not ask for it must not pay "
               "for the packets";
    }
    EXPECT_EQ(engine.xr_reports_sent_count(), 0u);
}

// ===========================================================================
// X-T6 — the session-level "rtcp-xr" attribute (RFC 3611 §5.1)
// ===========================================================================

TEST_F(EngineXrTmmbr, TheSessionLevelXrAttributeAuthorisesTheReport) {
    // RFC 3611 §5.1: the "rtcp-xr" attribute "is both a session and a
    // media level attribute.  When specified at session level, it
    // applies to all media level blocks in the session."
    //
    // So a peer that named the format at *session* level only is still
    // authorising the XR blocks — the media level is not the only
    // place the attribute may live.
    EngineConfig cfg = make_config();
    cfg.rtcp_xr_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ice->clear_sent();

    // The "a=rtcp-xr:" line sits *before* the m= line, so it is a
    // session-level attribute.
    const std::string peer_offer =
        "v=0\r\n"
        "o=- 1 1 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "a=rtcp-xr:rcvr-rtt=all\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "a=rtpmap:111 opus/48000/2\r\n";
    const auto answer = engine.process_remote_sdp(peer_offer);
    ASSERT_TRUE(answer.has_value());

    ice->clear_sent();
    run_for(engine, 1500ms);

    const auto* xr = find_xr(ice->sent());
    ASSERT_NE(xr, nullptr)
        << "RFC 3611 §5.1: a session-level \"rtcp-xr\" applies to every "
           "media block in the session";

    const auto parsed = nimrtc::rtp::parse_xr_packet(
        nimrtc::core::ByteSpan(xr->data(), xr->size()));
    ASSERT_TRUE(parsed.ok());

    bool found_rrtr = false;
    for (const auto& b : parsed.value().blocks) {
        if (b.type == nimrtc::rtp::XrBlockType::kReceiverReferenceTime) {
            found_rrtr = true;
        }
    }
    EXPECT_TRUE(found_rrtr)
        << "the session-level \"rcvr-rtt\" authorises the Receiver "
           "Reference Time block";
}

// ===========================================================================
// X-T3 — the DLRR block answers the peer's own report (RFC 3611 §4.5)
// ===========================================================================

TEST_F(EngineXrTmmbr, TheDlrrBlockAnswersThePeersReferenceTimeReport) {
    EngineConfig cfg = make_config();
    cfg.rtcp_xr_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The peer offers "rcvr-rtt" (so we may send the initiator) and we
    // echo it in our answer (so we are prepared to send the response).
    const std::string peer_offer =
        "v=0\r\n"
        "o=- 1 1 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=rtcp-xr:rcvr-rtt=all\r\n";
    const auto answer = engine.process_remote_sdp(peer_offer);
    ASSERT_TRUE(answer.has_value());
    ASSERT_NE(answer->find("rcvr-rtt"), std::string::npos)
        << "the answer must echo the collaborative parameter we support";

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ice->clear_sent();

    // The peer sends *its* Receiver Reference Time Report.  The engine
    // remembers it (RFC 3611 §4.5) so its next DLRR can answer it.
    constexpr std::uint32_t kPeerRtxSsrc = 0xBBBBBBBBu;
    constexpr std::uint64_t kPeerNtp = 0x1122334455667788ull;

    ByteBuffer peer_xr;
    // XR header: V=2, PT=207, length, then the originator SSRC.
    peer_xr.push_back(0x80u);
    peer_xr.push_back(207u);
    peer_xr.push_back(0u);
    peer_xr.push_back(0u);   // length patched below
    peer_xr.push_back(static_cast<std::uint8_t>(kPeerRtxSsrc >> 24));
    peer_xr.push_back(static_cast<std::uint8_t>(kPeerRtxSsrc >> 16));
    peer_xr.push_back(static_cast<std::uint8_t>(kPeerRtxSsrc >> 8));
    peer_xr.push_back(static_cast<std::uint8_t>(kPeerRtxSsrc));
    // The RRTR block: BT=4, reserved=0, length=2, then the NTP timestamp.
    peer_xr.push_back(4u);
    peer_xr.push_back(0u);
    peer_xr.push_back(0u);
    peer_xr.push_back(2u);
    for (int i = 7; i >= 0; --i) {
        peer_xr.push_back(static_cast<std::uint8_t>(kPeerNtp >> (i * 8)));
    }
    // The length field counts 32-bit words minus one.
    const std::uint16_t words =
        static_cast<std::uint16_t>(peer_xr.size() / 4u - 1u);
    peer_xr[2] = static_cast<std::uint8_t>(words >> 8);
    peer_xr[3] = static_cast<std::uint8_t>(words);

    engine.feed_media_inbound(peer_xr.data(), peer_xr.size());

    // Let a full RTCP interval pass so the DLRR is due.
    std::this_thread::sleep_for(30ms);
    ice->clear_sent();
    run_for(engine, 1500ms);

    const auto* xr = find_xr(ice->sent());
    ASSERT_NE(xr, nullptr)
        << "the engine did not put an XR report on the wire";

    const auto parsed = nimrtc::rtp::parse_xr_packet(
        nimrtc::core::ByteSpan(xr->data(), xr->size()));
    ASSERT_TRUE(parsed.ok());

    const nimrtc::rtp::XrDlrr* dlrr = nullptr;
    for (const auto& b : parsed.value().blocks) {
        if (b.type == nimrtc::rtp::XrBlockType::kDlrr) dlrr = &b.dlrr;
    }
    ASSERT_NE(dlrr, nullptr)
        << "RFC 3611 §4.5: the DLRR block answers the peer's Receiver "
           "Reference Time report";
    ASSERT_EQ(dlrr->sub_reports.size(), 1u);

    const auto& sub = dlrr->sub_reports.front();
    EXPECT_EQ(sub.ssrc, kPeerRtxSsrc)
        << "RFC 3611 §4.5: the sub-block names the SSRC of the receiver "
           "whose report is being answered";
    EXPECT_EQ(sub.last_rr,
              static_cast<std::uint32_t>((kPeerNtp >> 16) & 0xFFFFFFFFull))
        << "RFC 3611 §4.5: the LRR field is the middle 32 bits of the "
           "peer's NTP timestamp";
    EXPECT_GT(sub.dlrr, 0u)
        << "RFC 3611 §4.5: the DLRR field is the delay since that report, "
           "in 1/65536 s units";
}

// ===========================================================================
// X-T4 — the Statistics Summary block (RFC 3611 §4.6)
// ===========================================================================

TEST_F(EngineXrTmmbr, TheStatisticsSummaryReportsTheInboundAudioStream) {
    EngineConfig cfg = make_config();
    cfg.rtcp_xr_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::string peer_offer =
        "v=0\r\n"
        "o=- 1 1 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=rtcp-xr:rcvr-rtt=all stat-summary=loss,jitt\r\n";
    const auto answer = engine.process_remote_sdp(peer_offer);
    ASSERT_TRUE(answer.has_value());

    // Two packets with a hole: 1 and 3.  The gap is the loss the
    // summary has to report.
    for (std::uint32_t seq : {1u, 3u}) {
        const auto pkt = make_opus_rtp(kPeerAudioSsrc, kOpusPt, seq,
                                       (seq - 1) * 960);
        if (pkt.empty()) {
            GTEST_SKIP() << "the Opus codec plugin is not registered in this build";
        }
        engine.feed_media_inbound(pkt.data(), pkt.size());
    }

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ice->clear_sent();
    run_for(engine, 1500ms);

    const auto* xr = find_xr(ice->sent());
    ASSERT_NE(xr, nullptr);

    const auto parsed = nimrtc::rtp::parse_xr_packet(
        nimrtc::core::ByteSpan(xr->data(), xr->size()));
    ASSERT_TRUE(parsed.ok());

    const nimrtc::rtp::XrStatisticsSummary* stat = nullptr;
    for (const auto& b : parsed.value().blocks) {
        if (b.type == nimrtc::rtp::XrBlockType::kStatisticsSummary) {
            stat = &b.statistics_summary;
        }
    }
    ASSERT_NE(stat, nullptr)
        << "RFC 3611 §4.6: the Statistics Summary block reports the "
           "inbound stream's loss and jitter";
    EXPECT_EQ(stat->ssrc, kPeerAudioSsrc)        << "the summary must describe the inbound audio source";
    EXPECT_TRUE(stat->has_loss);
    EXPECT_TRUE(stat->has_jitter)
        << "the peer asked for \"loss,jitt\", so both flag bits are set";

    // RFC 3611 §4.1 defines the sequence fields as:
    //
    //     begin_seq: "The first sequence number that this block
    //                  reports on."
    //     end_seq:   "The last sequence number that this block
    //                  reports on *plus one*."
    //
    // So `end_seq` is the *exclusive* end of the interval.  The two
    // packets fed in were seq 1 and seq 3, so the interval starts at 1
    // and ends *after* 3 — i.e. end_seq must be 4, not 3.
    EXPECT_EQ(stat->begin_seq, 1u)
        << "the interval starts at the first sequence number seen";
    EXPECT_EQ(stat->end_seq, 4u)
        << "RFC 3611 §4.1: end_seq is the last sequence number plus one";
}

TEST_F(EngineXrTmmbr, TheStatisticsSummaryOnlyCarriesTheRequestedFields) {
    EngineConfig cfg = make_config();
    cfg.rtcp_xr_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The peer asks for the loss field only.  RFC 3611 §5.1: the
    // "stat-summary" parameter "contains a list indicating which fields
    // SHOULD be included in the Statistics Summary report blocks that
    // are sent" — so the jitter flag must stay clear.
    const std::string peer_offer =
        "v=0\r\n"
        "o=- 1 1 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=rtcp-xr:rcvr-rtt=all stat-summary=loss\r\n";
    const auto answer = engine.process_remote_sdp(peer_offer);
    ASSERT_TRUE(answer.has_value());

    feed_audio(engine);

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ice->clear_sent();
    run_for(engine, 1500ms);

    const auto* xr = find_xr(ice->sent());
    ASSERT_NE(xr, nullptr);

    const auto parsed = nimrtc::rtp::parse_xr_packet(
        nimrtc::core::ByteSpan(xr->data(), xr->size()));
    ASSERT_TRUE(parsed.ok());

    const nimrtc::rtp::XrStatisticsSummary* stat = nullptr;
    for (const auto& b : parsed.value().blocks) {
        if (b.type == nimrtc::rtp::XrBlockType::kStatisticsSummary) {
            stat = &b.statistics_summary;
        }
    }
    ASSERT_NE(stat, nullptr);
    EXPECT_TRUE(stat->has_loss);
    EXPECT_FALSE(stat->has_jitter)
        << "RFC 3611 §5.1: only the fields the peer asked for may be "
           "reported";
}

// ===========================================================================
// T-T1 / T-T2 / T-T3 — the outbound TMMBR (RFC 5104 §4.2.1)
// ===========================================================================

TEST_F(EngineXrTmmbr, TheTmmbrCarriesTheLiveBandwidthEstimate) {
    EngineConfig cfg = make_config();
    cfg.tmmbr_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The peer negotiates "ccm tmmbr" (RFC 5104 §7.2).
    const std::string peer_offer =
        "v=0\r\n"
        "o=- 1 1 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=rtcp-fb:111 ccm tmmbr\r\n";
    const auto answer = engine.process_remote_sdp(peer_offer);
    ASSERT_TRUE(answer.has_value());

    // The TMMBR names the peer's media source, so the engine has to
    // have seen a packet from it first.
    feed_audio(engine);

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ice->clear_sent();
    run_for(engine, 1500ms);

    const auto* tmmbr = find_rtpfb(ice->sent(), 3);
    ASSERT_NE(tmmbr, nullptr)
        << "the engine did not put a TMMBR on the wire";

    const auto msg = nimrtc::rtp::parse_tmmbr_message(
        nimrtc::core::ByteSpan(tmmbr->data(), tmmbr->size()));
    ASSERT_TRUE(msg.ok());
    ASSERT_EQ(msg.value().entries.size(), 1u);

    const auto& entry = msg.value().entries.front();
    EXPECT_EQ(entry.ssrc, kPeerAudioSsrc)
        << "RFC 5104 §4.2.1.2: the FCI entry names the media sender the "
           "tuple applies to";
    EXPECT_EQ(entry.bitrate_bps, 500'000u)
        << "X4: the reported rate must be the live bandwidth estimate — a "
           "TMMBR that announces a limit we do not honour is worse than no "
           "TMMBR at all";

    EXPECT_EQ(engine.last_tmmbr_bps(), 500'000u);
    EXPECT_GE(engine.tmmbr_sent_count(), 1u);
}

TEST_F(EngineXrTmmbr, NoTmmbrWhenThePeerNeverNegotiatedIt) {
    // RFC 5104 §7.2: "only the joint subset of CCM parameters from the
    // offer and answer may be used".  A peer that never named "tmmbr"
    // gets no TMMBR.
    EngineConfig cfg = make_config();
    cfg.tmmbr_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::string peer_offer =
        "v=0\r\n"
        "o=- 1 1 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "a=rtpmap:111 opus/48000/2\r\n";
    const auto answer = engine.process_remote_sdp(peer_offer);
    ASSERT_TRUE(answer.has_value());

    feed_audio(engine);

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ice->clear_sent();
    run_for(engine, 1500ms);

    for (const auto& p : ice->sent()) {
        EXPECT_FALSE(is_rtpfb(p, 3))
            << "an un-negotiated TMMBR must not be sent (RFC 5104 §7.2)";
    }
    EXPECT_EQ(engine.last_tmmbr_bps(), 0u);
}

TEST_F(EngineXrTmmbr, TheDisableTmmbrSwitchVetoesTheOutboundTmmbr) {
    // X5: `disable_tmmbr` vetoes the whole feature, even when the peer
    // negotiated it and the host enabled it.
    EngineConfig cfg = make_config();
    cfg.tmmbr_enabled  = true;
    cfg.disable_tmmbr  = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::string peer_offer =
        "v=0\r\n"
        "o=- 1 1 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=rtcp-fb:111 ccm tmmbr\r\n";
    const auto answer = engine.process_remote_sdp(peer_offer);
    ASSERT_TRUE(answer.has_value());

    feed_audio(engine);

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ice->clear_sent();
    run_for(engine, 1500ms);

    for (const auto& p : ice->sent()) {
        EXPECT_FALSE(is_rtpfb(p, 3))
            << "disable_tmmbr must veto the outbound TMMBR (X5)";
    }
    EXPECT_EQ(engine.last_tmmbr_bps(), 0u);
}

// ===========================================================================
// T-T4 — the TMMBN answers an inbound TMMBR (RFC 5104 §4.2.2)
// ===========================================================================

TEST_F(EngineXrTmmbr, AnInboundTmmbrIsAnsweredWithATmmbn) {
    EngineConfig cfg = make_config();
    cfg.tmmbr_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::string peer_offer =
        "v=0\r\n"
        "o=- 1 1 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=rtcp-fb:111 ccm tmmbr\r\n";
    const auto answer = engine.process_remote_sdp(peer_offer);
    ASSERT_TRUE(answer.has_value());

    // The peer asks us to throttle.  The tuple names the engine's own
    // audio source — the source the engine really sends.
    const std::uint32_t our_ssrc = engine.config().audio_ssrc;
    ASSERT_NE(our_ssrc, 0u);

    nimrtc::rtp::TmmbrEntry entry;
    entry.ssrc       = our_ssrc;
    entry.bitrate_bps = 320'000;

    constexpr std::uint32_t kPeerFeedbackSsrc = 0xAABBCCDDu;
    const ByteBuffer tmmbr = nimrtc::rtp::build_tmmbr(kPeerFeedbackSsrc,
                                                      {entry});
    ASSERT_FALSE(tmmbr.empty());
    engine.feed_media_inbound(tmmbr.data(), tmmbr.size());

    EXPECT_EQ(engine.peer_tmmbr_limit_bps(), 320'000u);

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ice->clear_sent();
    run_for(engine, 600ms);

    const auto* tmmbn = find_rtpfb(ice->sent(), 4);
    ASSERT_NE(tmmbn, nullptr)
        << "RFC 5104 §4.2.2.2: a TMMBN is owed for every TMMBR we receive";

    const auto msg = nimrtc::rtp::parse_tmmbn_message(
        nimrtc::core::ByteSpan(tmmbn->data(), tmmbn->size()));
    ASSERT_TRUE(msg.ok());
    ASSERT_EQ(msg.value().entries.size(), 1u);

    const auto& out = msg.value().entries.front();
    EXPECT_EQ(out.ssrc, kPeerFeedbackSsrc)
        << "RFC 5104 §4.2.2.1: the TMMBN FCI entry names the *owner* of "
           "the bounding tuple — in the unicast case the receiver that "
           "issued the TMMBR becomes the owner (§4.2.2.2)";
    EXPECT_EQ(out.bitrate_bps, 320'000u)
        << "the TMMBN reports the limit we are actually honouring";
}

TEST_F(EngineXrTmmbr, TheTmmbnIsSentOnlyOnceForABatchOfTmmbrs) {
    // RFC 5104 §4.2.2.2: "Only a single TMMBN SHALL be sent, even if
    // more than one TMMBR message is received between the scheduling of
    // the transmission of the TMMBN message and the actual transmission
    // of the TMMBN message."
    EngineConfig cfg = make_config();
    cfg.tmmbr_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::string peer_offer =
        "v=0\r\n"
        "o=- 1 1 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=rtcp-fb:111 ccm tmmbr\r\n";
    const auto answer = engine.process_remote_sdp(peer_offer);
    ASSERT_TRUE(answer.has_value());

    const std::uint32_t our_ssrc = engine.config().audio_ssrc;
    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ice->clear_sent();

    // Three TMMBRs land before the engine gets a chance to answer.
    for (std::uint32_t bps : {400'000u, 300'000u, 200'000u}) {
        nimrtc::rtp::TmmbrEntry e;
        e.ssrc        = our_ssrc;
        e.bitrate_bps = bps;
        const ByteBuffer pkt =
            nimrtc::rtp::build_tmmbr(0xAABBCCDDu, {e});
        engine.feed_media_inbound(pkt.data(), pkt.size());
    }

    // The tightest limit wins (RFC 5104 §3.5.4.2).
    EXPECT_EQ(engine.peer_tmmbr_limit_bps(), 200'000u);

    run_for(engine, 600ms);

    int tmmbn_count = 0;
    for (const auto& p : ice->sent()) {
        if (is_rtpfb(p, 4)) ++tmmbn_count;
    }
    EXPECT_EQ(tmmbn_count, 1)
        << "RFC 5104 §4.2.2.2: exactly one TMMBN per batch of TMMBRs";
}

// ===========================================================================
// T-T5 — the TMMBN is immediate feedback (RFC 5104 §4.2.2.3)
// ===========================================================================

TEST_F(EngineXrTmmbr, TheTmmbnGoesOutAsImmediateFeedback) {
    // RFC 5104 §4.2.2.3: "The TMMBN acknowledgement SHOULD be sent as
    // soon as allowed by the applied timing rules for the session.
    // Immediate or early feedback mode SHOULD be used for these
    // messages."
    //
    // So the acknowledgement must not wait for the next RTCP slot: it
    // goes out on the very next tick.  A test that only checks "a TMMBN
    // eventually arrived" would pass even with a one-second delay, so
    // this one measures the delay.
    EngineConfig cfg = make_config();
    cfg.tmmbr_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::string peer_offer =
        "v=0\r\n"
        "o=- 1 1 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "a=rtcp-fb:111 ccm tmmbr\r\n";
    const auto answer = engine.process_remote_sdp(peer_offer);
    ASSERT_TRUE(answer.has_value());

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ice->clear_sent();

    const std::uint32_t our_ssrc = engine.config().audio_ssrc;
    nimrtc::rtp::TmmbrEntry entry;
    entry.ssrc        = our_ssrc;
    entry.bitrate_bps = 320'000;
    const ByteBuffer tmmbr =
        nimrtc::rtp::build_tmmbr(0xAABBCCDDu, {entry});
    ASSERT_FALSE(tmmbr.empty());

    // The TMMBR arrives; the engine must answer it without waiting for
    // the RTCP interval.
    engine.feed_media_inbound(tmmbr.data(), tmmbr.size());

    // A handful of ticks — far less than the one-second RTCP interval.
    bool answered = false;
    for (int i = 0; i < 5 && !answered; ++i) {
        engine.tick();
        for (const auto& p : ice->sent()) {
            if (is_rtpfb(p, 4)) { answered = true; break; }
        }
        if (!answered) std::this_thread::sleep_for(20ms);
    }

    ASSERT_TRUE(answered)
        << "RFC 5104 §4.2.2.3: the TMMBN is immediate feedback — it must "
           "not wait for the next RTCP slot";
}

}  // namespace
