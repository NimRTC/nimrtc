/**
 * @file tests/test_engine_ice_send_gate.cpp
 * @brief The Q5 finding's second defect: the engine kept sending into a
 *        dead transport.
 *
 * ## What the soak found
 *
 * The 24 h impaired soak ran on 2026-11-20.  At 3 h 48 m the ICE
 * consent freshness expired (RFC 7675 §5.1 — the agent MUST cease
 * transmission once a 30 s window passes without a valid Binding
 * response), libjuice moved the agent to `failed`, and from that moment
 * **every** media send was refused:
 *
 *     [error] Send while ICE is not connected        (26 627 times)
 *
 * Twenty-six thousand refusals, at full media rate, for the remaining
 * 21 minutes of the run.  The engine *noticed* the ICE state change —
 * it logged "engine[ice]: connected->failed" — and kept sending
 * anyway: there was no path from "the call is dead" to "stop the media
 * source".
 *
 * Two things are wrong with that, and this test pins both:
 *
 *   1. **The work.**  A refused send still costs an encode, an SRTP
 *      protect, a scheduler slot and a UDP write attempt.
 *   2. **The report.**  The host sees a stream of per-packet failures
 *      instead of one clear "the transport died" event, so it cannot
 *      tell "the peer is not there" from "the codec is broken".
 *
 * ## The fix
 *
 * `NimRTCEngine::update_ice_send_gate()` runs at the top of `tick()`.
 * When the ICE agent has given up, it closes the gate, and the media
 * paths (`send_audio`, the video senders, the DTMF emitter, the
 * scheduler drain) stop producing packets for a transport that cannot
 * carry them.
 *
 * The gate is deliberately **narrower** than "ICE is not connected":
 * the window between `open()` and the first successful connectivity
 * check is a normal startup phase, and a host that feeds PCM during
 * that window must not see its audio rejected.  Only a transport that
 * *was* connected and then stopped counts as dead.
 *
 * ## The fakes
 *
 * The engine is driven through the same two fakes the audio-send suite
 * uses: a **fake ICE transport** whose state the test controls, and a
 * **fake DTLS session** that reports the handshake as complete and
 * exports no SRTP keying material.  The second one matters: the DTLS
 * session stays `Connected` after the ICE consent expires — the crypto
 * layer has no idea the 5-tuple died — so the gate has to be the thing
 * that stops the media, not the DTLS state.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/engine_errors.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/dtls/dtls_session_factory.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/ice_transport.hpp>
#include <nimrtc/rtp/packet.hpp>

namespace {

namespace np = nimrtc::plugins;
namespace nd = nimrtc::dtls;
namespace core = nimrtc::core;

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

/** The payload type RFC 7587 assigns to Opus. */
constexpr std::uint8_t kOpusPt = 111;

/** The engine's default PCM rate (48 kHz) and frame duration (20 ms):
 *  one frame is 960 samples. */
constexpr std::size_t kSampleRate = 48000;
constexpr std::size_t kFrameMs = 20;
constexpr std::size_t kFrameSamples = kSampleRate * kFrameMs / 1000;

// ---------------------------------------------------------------------------
// Fake ICE transport — with a state the test drives
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
        ++send_calls_;
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
    np::IceState state() const noexcept override { return state_; }
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

    /** Drive the transport's state — the seam the whole test turns on. */
    void set_state(np::IceState s) noexcept { state_ = s; }

    /** How many times the engine asked the transport to send.
     *
     *  This is the number the soak's "26 627" measures. */
    std::size_t send_calls() const noexcept { return send_calls_; }

    const std::vector<std::vector<std::uint8_t>>& sent() const noexcept {
        return sent_;
    }

private:
    np::RecvCallback   on_recv_;
    np::ErrorCallback  on_error_;
    std::vector<std::vector<std::uint8_t>> inbound_;
    std::vector<std::vector<std::uint8_t>> sent_;
    std::size_t        send_calls_ = 0;
    np::IceState       state_ = np::IceState::Connected;
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
// Fake DTLS session
// ---------------------------------------------------------------------------

/** A DTLS session that reports the handshake as complete and exports no
 *  SRTP keying material.
 *
 *  The engine asks two things of the DTLS session on the send path:
 *  "is the handshake done?" and "give me the SRTP keys".  The fake
 *  answers the first with yes and the second with "not ready", so the
 *  RTP travels in cleartext and the test can read it.
 *
 *  The important detail is that `is_connected()` stays **true** for the
 *  whole test: a real DTLS session does not notice that the ICE
 *  consent expired, and the fix must not depend on it noticing. */
class FakeDtlsSession final : public nd::IDtlsSession {
public:
    // ---- The seam surface ------------------------------------------------
    void set_role(nd::Role) noexcept override {}
    void set_peer_fingerprint(std::span<const std::uint8_t>) noexcept override {}
    void start() noexcept override {}
    void pump() noexcept override {}
    void on_handshake_complete(OnCompleteCb) noexcept override {}
    np::Status export_srtp_key_material(std::span<std::uint8_t, 60>) noexcept override {
        return np::kErrNotReady;
    }

    // ---- The legacy engine-facing surface --------------------------------
    nimrtc::core::Result<void> open() noexcept override {
        return nimrtc::core::Result<void>::make_ok();
    }
    void set_role(nd::DtlsRole) noexcept override {}
    void set_peer_fingerprint(std::string, std::vector<std::uint8_t>) noexcept override {}
    std::size_t feed_inbound(std::span<const std::uint8_t>,
                             const nd::DtlsAddr&) noexcept override {
        return 0;
    }
    std::vector<nd::DtlsRecord> take_outbound() noexcept override { return {}; }
    void tick() noexcept override {}
    nd::DtlsState state() const noexcept override { return nd::DtlsState::Connected; }
    bool is_connected() const noexcept override { return true; }
    const nd::Fingerprint& local_fingerprint() const noexcept override {
        return fingerprint_;
    }
    std::optional<nd::SrtpKeyingMaterial>
    srtp_keying_material() const noexcept override {
        return std::nullopt;
    }

private:
    nd::Fingerprint fingerprint_{"sha-256", {}, {}, ""};
};

class FakeDtlsFactory final : public nd::IDtlsSessionFactory {
public:
    std::string_view id() const noexcept override { return "fake-dtls"; }
    std::string_view display_name() const noexcept override {
        return "Fake DTLS session (test)";
    }
    std::unique_ptr<nd::IDtlsSession> create(const nd::DtlsConfig&) const override {
        return std::make_unique<FakeDtlsSession>();
    }
};

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class EngineIceSendGate : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();

        static FakeIceFactory ice_factory;
        static FakeDtlsFactory dtls_factory;
        PluginRegistry::instance().register_ice_transport("fake-ice", &ice_factory);
        PluginRegistry::instance().register_dtls_session("fake-dtls", &dtls_factory);
    }

    EngineConfig make_config() {
        EngineConfig cfg{};
        cfg.transport_name          = "fake-ice";
        cfg.dtls_name               = "fake-dtls";
        cfg.datachannel_backend     = "";
        cfg.bwe_name                = "";
        cfg.scheduler_config.enabled = false;
        return cfg;
    }

    /** The RTP packets the engine put on the wire. */
    static std::vector<nimrtc::rtp::PacketView> rtp_packets(
        const std::vector<std::vector<std::uint8_t>>& datagrams) {
        std::vector<nimrtc::rtp::PacketView> out;
        nimrtc::rtp::Parser parser;
        for (const auto& d : datagrams) {
            auto parsed = parser.parse(nimrtc::core::ByteSpan(d.data(), d.size()));
            if (!parsed.ok()) continue;
            const nimrtc::rtp::PacketView& view = parsed.value();
            if (view.payload_type != kOpusPt) continue;
            out.push_back(view);
        }
        return out;
    }
};

// ============================================================================
// 1. The gate closes when the transport dies
// ============================================================================

TEST_F(EngineIceSendGate, TheMediaStopsWhenIceFails) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);

    // ---- The call is healthy: the audio goes out. ----------------------
    std::vector<float> pcm(kFrameSamples, 0.25f);
    ASSERT_EQ(engine.send_audio(pcm.data(), pcm.size()), 0u);
    EXPECT_EQ(rtp_packets(ice->sent()).size(), 1u)
        << "a healthy transport carries the audio";
    const std::size_t sends_before = ice->send_calls();

    // ---- The transport dies. --------------------------------------------
    ice->set_state(np::IceState::Failed);
    engine.tick();   // the gate is evaluated at the top of tick()

    // ---- The media stops. -----------------------------------------------
    const std::size_t sends_at_failure = ice->send_calls();
    EXPECT_EQ(engine.send_audio(pcm.data(), pcm.size()), core::kEngineNotReady)
        << "the engine must refuse to build media for a dead transport";

    // Twenty more frames: the point of the test.  Without the gate each
    // of these would reach the transport and be refused with "Send while
    // ICE is not connected" — which is exactly the 26 627 the soak saw.
    for (int i = 0; i < 20; ++i) {
        engine.send_audio(pcm.data(), pcm.size());
        engine.tick();
    }

    EXPECT_EQ(ice->send_calls(), sends_at_failure)
        << "not one packet may be offered to a transport that has failed — "
           "the 24 h soak measured 26 627 of them";
    EXPECT_EQ(rtp_packets(ice->sent()).size(), 1u)
        << "the only RTP on the wire is the one from before the failure";
    EXPECT_GT(sends_before, 0u);
}

TEST_F(EngineIceSendGate, TheGateAlsoClosesWhenIceDropsWithoutFailing) {
    // ICE can drop to `Disconnected` without reaching `Failed` — a NAT
    // rebinding, or a network blip the agent is still retrying.  The
    // consent is gone either way (RFC 7675 §5.1), so the media stops
    // either way.
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);

    std::vector<float> pcm(kFrameSamples, 0.25f);
    ASSERT_EQ(engine.send_audio(pcm.data(), pcm.size()), 0u);

    ice->set_state(np::IceState::Disconnected);
    engine.tick();

    const std::size_t sends_at_drop = ice->send_calls();
    EXPECT_EQ(engine.send_audio(pcm.data(), pcm.size()), core::kEngineNotReady);
    for (int i = 0; i < 20; ++i) {
        engine.send_audio(pcm.data(), pcm.size());
        engine.tick();
    }
    EXPECT_EQ(ice->send_calls(), sends_at_drop)
        << "a dropped transport must not be written to either";
}

// ============================================================================
// 2. The gate opens again when the transport recovers
// ============================================================================

TEST_F(EngineIceSendGate, TheMediaResumesWhenTheTransportComesBack) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);

    std::vector<float> pcm(kFrameSamples, 0.25f);
    ASSERT_EQ(engine.send_audio(pcm.data(), pcm.size()), 0u);

    // The transport dies, and the gate closes.
    ice->set_state(np::IceState::Failed);
    engine.tick();
    EXPECT_EQ(engine.send_audio(pcm.data(), pcm.size()), core::kEngineNotReady);

    // The transport comes back (a NAT rebinding the flap tracker handles).
    ice->set_state(np::IceState::Connected);
    engine.tick();

    EXPECT_EQ(engine.send_audio(pcm.data(), pcm.size()), 0u)
        << "a recovered transport must carry the media again — the gate is "
           "not a one-way latch";
    EXPECT_EQ(rtp_packets(ice->sent()).size(), 2u)
        << "the packet that was refused while the transport was down is "
           "not replayed, but the new one goes out";
}

// ============================================================================
// 3. The startup phase is not an outage
// ============================================================================

TEST_F(EngineIceSendGate, TheStartupPhaseIsNotTreatedAsAnOutage) {
    // Before the first successful connectivity check the transport is
    // "not connected", but that is a *normal* phase of every session —
    // not a failure.  A host that feeds PCM during that window must not
    // have its audio rejected, and the engine must not have declared the
    // transport dead.
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ice->set_state(np::IceState::Connecting);

    std::vector<float> pcm(kFrameSamples, 0.25f);
    EXPECT_EQ(engine.send_audio(pcm.data(), pcm.size()), 0u)
        << "the startup phase is not an outage: the media path must not "
           "be gated on a transport that has not connected *yet*";
}

// ============================================================================
// 4. The report
// ============================================================================

TEST_F(EngineIceSendGate, TheOutageIsReportedOnceAndTheCounterGrows) {
    // The soak's second complaint was the *report*: 26 627 refusals is
    // not a report, it is noise.  The gate says so once per outage, and
    // counts the outages so a host can tell one long outage from two
    // short ones.
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);

    std::vector<float> pcm(kFrameSamples, 0.25f);
    ASSERT_EQ(engine.send_audio(pcm.data(), pcm.size()), 0u);

    ice->set_state(np::IceState::Failed);
    engine.tick();
    engine.tick();
    engine.tick();

    // A second, separate outage.
    ice->set_state(np::IceState::Connected);
    engine.tick();
    ice->set_state(np::IceState::Failed);
    engine.tick();

    EXPECT_EQ(engine.stats().ice_send_blocked_events, 2u)
        << "two outages are two events, however many ticks each one spans";
}

}  // namespace
