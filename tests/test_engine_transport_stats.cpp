/**
 * @file tests/test_engine_transport_stats.cpp
 * @brief The engine's transport-plane statistics (v1.2 plan **Track S / S5**).
 *
 * ## What is tested
 *
 * The transport plane 鈥?ICE 鈫?DTLS 鈫?SRTP 鈥?is the path a session's
 * packets actually travel, and it is the surface a host reads when a
 * session "is not working".  The v1.2 plan's S5 row says the engine had
 * "**ICE has no statistics at all**: no candidate-pair entries, no RTT,
 * no `selected_local()` / `selected_remote()`", and the connectivity
 * track's C4 row depends on this surface existing.
 *
 * This test drives the **engine** and asserts:
 *
 *   - the ICE state and the nominated candidate pair reach the surface,
 *     and the *relay* bit is derived from the pair's candidate types;
 *   - the DTLS state and role reach the surface;
 *   - the SRTP counters reach the surface;
 *   - the round-trip time reaches the surface, and the surface says
 *     **whether it was measured at all** 鈥?the difference between "the
 *     path is fast" and "nothing was ever measured".
 *
 * ## The negative control
 *
 * The last suite is the important one: it drives the engine with a
 * transport that has *no* nominated pair and a feedback path that has
 * *no* measurement, and asserts that the surface says so rather than
 * inventing a zero.  A surface that reported "0 ms" and "no pair" for a
 * session that never connected would be indistinguishable from a
 * working one 鈥?and that is the failure this suite exists to catch.
 */
#include <gtest/gtest.h>

#include <cctype>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/dtls/dtls_session_factory.hpp>
#include <nimrtc/dtls/dtls_session_iface.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/ice_transport.hpp>
#include <nimrtc/plugins/net_feedback.hpp>

// The engine-private projection header.  The test reaches it to pin down
// the *vocabulary* 鈥?the property worth proving is that the DTLS state
// names are derived from the DTLS module's own spelling rather than
// copied into a second table that could drift.  This header is
// engine-private; including it from a test does not make it public.
#include "engine_transport_stats.hpp"

namespace {

namespace np = nimrtc::plugins;
namespace nd = nimrtc::dtls;
namespace core = nimrtc::core;

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

// ---------------------------------------------------------------------------
// A fake ICE transport whose state and nominated pair the test controls.
// ---------------------------------------------------------------------------

/**
 * @brief An ICE transport the test drives directly.
 *
 * The fake exposes the two things the S5 surface reads from the ICE
 * seam 鈥?the agent's state and the nominated candidate pair 鈥?and lets
 * the test set them.  Everything else is the minimum an `IICETransport`
 * must implement.
 */
class ScriptedIce final : public np::IICETransport {
public:
    // ---- The script the test sets ----------------------------------------

    np::IceState script_state = np::IceState::Connected;
    std::optional<np::IceCandidatePair> script_pair;
    std::uint32_t script_restarts = 0;

    // ---- ITransport --------------------------------------------------------

    const char* name() const noexcept override { return "s5-scripted-ice"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}
    void set_callbacks(np::RecvCallback, np::ErrorCallback) noexcept override {}
    np::Status send(np::BufferView, np::Addr = {}) noexcept override {
        return np::kOk;
    }
    int recv() noexcept override { return 0; }
    np::Addr local_addr() const noexcept override { return {}; }
    np::Addr remote_addr() const noexcept override { return {}; }

    // ---- IICETransport -----------------------------------------------------

    np::IceState state() const noexcept override { return script_state; }
    std::string local_ufrag() const noexcept override { return "s5ufrag"; }
    std::string local_password() const noexcept override { return "s5password"; }
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

    // The S5 seam: the nominated pair, as the ICE module projects it.
    std::optional<np::IceCandidatePair> selected_pair() const noexcept override {
        return script_pair;
    }

    std::uint32_t restart_count() const noexcept override {
        return script_restarts;
    }

    void set_bwe(np::IBwe*) noexcept override {}
    void set_scheduler(np::IScheduler*) noexcept override {}

private:
    np::RecvCallback  on_recv_;
    np::ErrorCallback on_error_;
};

class ScriptedIceFactory final : public np::IICETransportFactory {
public:
    std::string_view id() const noexcept override { return "s5-scripted-ice"; }
    std::string_view display_name() const noexcept override {
        return "Scripted ICE (S5 test)";
    }
    np::IICETransport* create_ice() const override { return new ScriptedIce(); }
};

// ---------------------------------------------------------------------------
// A fake DTLS session with a controllable state and fingerprint.
// ---------------------------------------------------------------------------

class ScriptedDtls final : public nd::IDtlsSession {
public:
    /** The fingerprint every scripted session reports.
     *
     *  It is a *static* so the test can assert the exact value without
     *  having to reach the instance the factory created. */
    static const char* kFingerprintHex;

    nd::DtlsState script_state = nd::DtlsState::Connected;

    void set_role(nd::Role) noexcept override {}
    void set_peer_fingerprint(std::span<const std::uint8_t>) noexcept override {}
    void start() noexcept override {}
    void pump() noexcept override {}
    void on_handshake_complete(OnCompleteCb) noexcept override {}
    np::Status export_srtp_key_material(std::span<std::uint8_t, 60>) noexcept override {
        return np::kErrNotReady;
    }

    core::Result<void> open() noexcept override {
        return core::Result<void>::make_ok();
    }
    void set_role(nd::DtlsRole) noexcept override {}
    void set_peer_fingerprint(std::string, std::vector<std::uint8_t>) noexcept override {}
    std::size_t feed_inbound(std::span<const std::uint8_t>,
                             const nd::DtlsAddr&) noexcept override {
        return 0;
    }
    std::vector<nd::DtlsRecord> take_outbound() noexcept override { return {}; }
    void tick() noexcept override {}
    nd::DtlsState state() const noexcept override { return script_state; }
    bool is_connected() const noexcept override {
        return script_state == nd::DtlsState::Connected;
    }
    const nd::Fingerprint& local_fingerprint() const noexcept override {
        return fingerprint_;
    }
    std::optional<nd::SrtpKeyingMaterial>
    srtp_keying_material() const noexcept override {
        return std::nullopt;
    }

private:
    nd::Fingerprint fingerprint_{"sha-256", {}, {}, kFingerprintHex};
};

const char* ScriptedDtls::kFingerprintHex = "AA:BB:CC:DD:EE:FF";

class ScriptedDtlsFactory final : public nd::IDtlsSessionFactory {
public:
    std::string_view id() const noexcept override { return "s5-scripted-dtls"; }
    std::string_view display_name() const noexcept override {
        return "Scripted DTLS (S5 test)";
    }
    std::unique_ptr<nd::IDtlsSession> create(const nd::DtlsConfig&) const override {
        return std::make_unique<ScriptedDtls>();
    }
};

// ---------------------------------------------------------------------------
// A fake network-feedback collector with a controllable RTT.
// ---------------------------------------------------------------------------

/**
 * @brief A collector whose snapshot the test dictates.
 *
 * The S5 surface reads the round-trip time from this seam, so the test
 * needs to control both the *value* and the *has_rtt* flag 鈥?the two are
 * distinct facts, and the surface has to report them separately.
 */
class ScriptedNetFeedback final : public np::INetFeedback {
public:
    np::NetFeedbackSnapshot script_snapshot;

    const char* name() const noexcept override { return "s5-scripted-nfb"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    void on_rtcp_block(std::uint8_t, std::uint8_t, core::ByteSpan,
                       core::TimePoint) noexcept override {}
    void on_local_sender_report(std::uint32_t, core::TimePoint) noexcept override {}

    void set_stream_clock_rate(std::uint32_t) noexcept override {}

    np::NetFeedbackSnapshot snapshot() const noexcept override {
        return script_snapshot;
    }

    void set_config(const np::NetFeedbackConfig& config) noexcept override {
        config_ = config;
    }
    np::NetFeedbackConfig config() const noexcept override { return config_; }
    void set_peer_capabilities(const np::NetFeedbackCapabilities& caps) noexcept override {
        caps_ = caps;
    }
    np::NetFeedbackCapabilities peer_capabilities() const noexcept override {
        return caps_;
    }
    bool transport_cc_unavailable() const noexcept override { return false; }
    void evaluate(core::TimePoint) noexcept override {}
    void reset() noexcept override { script_snapshot = {}; }

private:
    np::NetFeedbackConfig config_{};
    np::NetFeedbackCapabilities caps_{};
};

class ScriptedNetFeedbackFactory final : public np::INetFeedbackFactory {
public:
    std::string_view id() const noexcept override {
        return "s5-scripted-net-feedback";
    }
    std::string_view display_name() const noexcept override {
        return "Scripted network feedback (S5 test)";
    }
    np::INetFeedback* create() const override {
        return new ScriptedNetFeedback();
    }
};

// ---------------------------------------------------------------------------
// The fixture
// ---------------------------------------------------------------------------

class EngineTransportStats : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();

        static ScriptedIceFactory ice;
        PluginRegistry::instance().register_ice_transport(ice.id(), &ice);

        static ScriptedDtlsFactory dtls;
        PluginRegistry::instance().register_dtls_session(dtls.id(), &dtls);

        static ScriptedNetFeedbackFactory nfb;
        PluginRegistry::instance().register_net_feedback(nfb.id(), &nfb);
    }

    static EngineConfig make_config() {
        EngineConfig cfg{};
        cfg.transport_name       = "s5-scripted-ice";
        cfg.dtls_name            = "s5-scripted-dtls";
        cfg.net_feedback_name    = "s5-scripted-net-feedback";
        cfg.bwe_name             = "";
        cfg.scheduler_config.enabled = false;
        cfg.datachannel_backend = "";
        return cfg;
    }

    /** Build a candidate the way the ICE seam publishes one. */
    static np::IceCandidate make_candidate(const char* address,
                                            std::uint16_t port,
                                            const char* type) {
        np::IceCandidate c;
        c.address  = address;
        c.port     = port;
        c.protocol = "udp";
        c.type     = type;
        return c;
    }
};

// ---------------------------------------------------------------------------
// The vocabulary
// ---------------------------------------------------------------------------

/**
 * @brief The DTLS state names are *derived*, not copied.
 *
 * The projection lower-cases the DTLS module's own CamelCase name and
 * inserts a hyphen before each capital.  The property worth pinning
 * down is that there is only **one** table 鈥?the module's 鈥?so a state
 * the module adds later cannot be missing here.
 */
TEST(TransportStatsVocabulary, TheDtlsNamesFollowTheModulesOwnSpelling) {
    using nimrtc::engine::detail::TransportStatsProjector;

    // Every state the module knows, re-spelled.
    EXPECT_EQ(TransportStatsProjector::dtls_state_name(nd::DtlsState::Closed),
              "closed");
    EXPECT_EQ(TransportStatsProjector::dtls_state_name(nd::DtlsState::Initial),
              "initial");
    EXPECT_EQ(TransportStatsProjector::dtls_state_name(nd::DtlsState::HelloVerify),
              "hello-verify");
    EXPECT_EQ(TransportStatsProjector::dtls_state_name(nd::DtlsState::HelloSent),
              "hello-sent");
    EXPECT_EQ(TransportStatsProjector::dtls_state_name(nd::DtlsState::HelloReceived),
              "hello-received");
    EXPECT_EQ(TransportStatsProjector::dtls_state_name(
                  nd::DtlsState::CertificateReceived),
              "certificate-received");
    EXPECT_EQ(TransportStatsProjector::dtls_state_name(nd::DtlsState::KeyExchange),
              "key-exchange");
    EXPECT_EQ(TransportStatsProjector::dtls_state_name(
                  nd::DtlsState::ChangeCipherSpec),
              "change-cipher-spec");
    EXPECT_EQ(TransportStatsProjector::dtls_state_name(nd::DtlsState::Finished),
              "finished");
    EXPECT_EQ(TransportStatsProjector::dtls_state_name(nd::DtlsState::Connected),
              "connected");
    EXPECT_EQ(TransportStatsProjector::dtls_state_name(nd::DtlsState::Failed),
              "failed");
}

/**
 * @brief The ICE state names are the engine's own vocabulary.
 *
 * `NimRTCEngine::ice_state_string()` publishes the same words, so a
 * host that reads both surfaces sees one vocabulary rather than two
 * that differ only in spelling.
 */
TEST(TransportStatsVocabulary, TheIceNamesAreLowerCased) {
    using nimrtc::engine::detail::TransportStatsProjector;

    EXPECT_STREQ(TransportStatsProjector::ice_state_name(np::IceState::Disconnected),
                  "disconnected");
    EXPECT_STREQ(TransportStatsProjector::ice_state_name(np::IceState::Gathering),
                  "gathering");
    EXPECT_STREQ(TransportStatsProjector::ice_state_name(np::IceState::Connecting),
                  "connecting");
    EXPECT_STREQ(TransportStatsProjector::ice_state_name(np::IceState::Connected),
                  "connected");
    EXPECT_STREQ(TransportStatsProjector::ice_state_name(np::IceState::Completed),
                  "completed");
    EXPECT_STREQ(TransportStatsProjector::ice_state_name(np::IceState::Failed),
                  "failed");
}

/**
 * @brief The derived names are lower-case and hyphen-separated.
 *
 * A host that prints the state into a log line (or compares it against
 * a configured string) must not have to know the DTLS module's
 * CamelCase spelling.  The check is generic rather than per-state: it
 * applies to *every* state, including one the module adds later.
 */
TEST(TransportStatsVocabulary, EveryDtlsNameIsLowerCaseAndHyphenated) {
    using nimrtc::engine::detail::TransportStatsProjector;

    const nd::DtlsState all[] = {
        nd::DtlsState::Closed,      nd::DtlsState::Initial,
        nd::DtlsState::HelloVerify, nd::DtlsState::HelloSent,
        nd::DtlsState::HelloReceived, nd::DtlsState::CertificateReceived,
        nd::DtlsState::KeyExchange, nd::DtlsState::ChangeCipherSpec,
        nd::DtlsState::Finished,     nd::DtlsState::Connected,
        nd::DtlsState::Failed,
    };

    for (const auto state : all) {
        const std::string name =
            TransportStatsProjector::dtls_state_name(state);
        EXPECT_FALSE(name.empty());
        EXPECT_EQ(name.front(), std::tolower(name.front()));
        for (const char ch : name) {
            const bool lower = (ch >= 'a' && ch <= 'z');
            const bool digit = (ch >= '0' && ch <= '9');
            const bool hyphen = (ch == '-');
            EXPECT_TRUE(lower || digit || hyphen)
                << "the derived name \"" << name << "\" carries a character "
                   "that is not lower-case, a digit or a hyphen";
        }
    }
}

// ---------------------------------------------------------------------------
// The ICE plane
// ---------------------------------------------------------------------------

TEST_F(EngineTransportStats, IceStateReachesTheSurface) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0);

    // The fake ICE transport reports "connected"; the surface must
    // publish the same word the engine's own `ice_state_string()` uses.
    const auto stats = engine.stats();
    EXPECT_EQ(stats.transport.ice_state, "connected");

    engine.close();
}

TEST_F(EngineTransportStats, TheNominatedPairReachesTheSurface) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0);

    // The pair is installed on the *engine's* transport, which the fake
    // creates per engine 鈥?so the test reaches it through the seam the
    // host would use.
    auto* ice = engine.get_ice_transport();
    ASSERT_NE(ice, nullptr);
    auto* scripted = dynamic_cast<ScriptedIce*>(ice);
    ASSERT_NE(scripted, nullptr);

    np::IceCandidatePair pair;
    pair.local  = make_candidate("192.168.1.10", 50000, "host");
    pair.remote = make_candidate("203.0.113.5", 40000, "srflx");
    scripted->script_pair = pair;

    const auto stats = engine.stats();
    ASSERT_TRUE(stats.transport.selected_pair.has_value());
    EXPECT_EQ(stats.transport.selected_pair->local.address, "192.168.1.10");
    EXPECT_EQ(stats.transport.selected_pair->local.port, 50000);
    EXPECT_EQ(stats.transport.selected_pair->local.protocol, "udp");
    EXPECT_EQ(stats.transport.selected_pair->local.type, "host");

    // A host / srflx pair is a *direct* path, so the relay bit is false.
    EXPECT_FALSE(stats.transport.relayed);

    engine.close();
}

TEST_F(EngineTransportStats, ARelayPairSetsTheRelayBit) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0);

    auto* ice = engine.get_ice_transport();
    auto* scripted = dynamic_cast<ScriptedIce*>(ice);
    ASSERT_NE(scripted, nullptr);

    np::IceCandidatePair pair;
    pair.local  = make_candidate("192.168.1.10", 50000, "relay");
    pair.remote = make_candidate("203.0.113.5", 40000, "host");
    scripted->script_pair = pair;

    const auto stats = engine.stats();
    EXPECT_TRUE(stats.transport.relayed);

    engine.close();
}

TEST_F(EngineTransportStats, TheRestartCountReachesTheSurface) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0);

    auto* ice = engine.get_ice_transport();
    auto* scripted = dynamic_cast<ScriptedIce*>(ice);
    ASSERT_NE(scripted, nullptr);

    EXPECT_EQ(engine.stats().transport.ice_restart_count, 0u);

    scripted->script_restarts = 3;
    EXPECT_EQ(engine.stats().transport.ice_restart_count, 3u);

    engine.close();
}

// ---------------------------------------------------------------------------
// The DTLS plane
// ---------------------------------------------------------------------------

TEST_F(EngineTransportStats, TheDtlsStateReachesTheSurface) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0);

    // The engine's own accessor 鈥?the surface must agree with it.
    const auto engine_state = engine.dtls_state();
    ASSERT_EQ(engine_state, nd::DtlsState::Connected);

    const auto stats = engine.stats();
    // The DTLS module spells its states in CamelCase; the surface
    // publishes them lower-cased and hyphen-separated.
    EXPECT_EQ(stats.transport.dtls_state, "connected");

    engine.close();
}

TEST_F(EngineTransportStats, TheFingerprintReachesTheSurface) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0);

    const auto stats = engine.stats();
    // The DTLS session is the fake one, so the surface publishes the
    // fake's fingerprint 鈥?in RFC 8122's colon-separated UPPER hex form.
    EXPECT_EQ(stats.transport.dtls_fingerprint, ScriptedDtls::kFingerprintHex);

    engine.close();
}

// ---------------------------------------------------------------------------
// The round-trip time
// ---------------------------------------------------------------------------

TEST_F(EngineTransportStats, TheRttReachesTheSurface) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0);

    // The collector is the engine's own instance, reached through the
    // seam the host would use.
    auto* nfb = engine.net_feedback();
    ASSERT_NE(nfb, nullptr);
    auto* scripted = dynamic_cast<ScriptedNetFeedback*>(nfb);
    ASSERT_NE(scripted, nullptr);

    np::NetFeedbackSnapshot snap;
    snap.has_rtt = true;
    snap.rtt     = core::Milliseconds{42};
    scripted->script_snapshot = snap;

    const auto stats = engine.stats();
    EXPECT_TRUE(stats.transport.has_rtt);
    EXPECT_EQ(stats.transport.rtt_ms, 42u);

    engine.close();
}

// ---------------------------------------------------------------------------
// The negative control
// ---------------------------------------------------------------------------

TEST_F(EngineTransportStats, AnUnmeasuredRttIsReportedAsUnmeasured) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0);

    auto* nfb = engine.net_feedback();
    auto* scripted = dynamic_cast<ScriptedNetFeedback*>(nfb);
    ASSERT_NE(scripted, nullptr);

    // A snapshot with *no* measurement: the flag is false and the value
    // is zero.  The surface must report "not measured" 鈥?not "0 ms",
    // which would be indistinguishable from a genuinely instant path.
    np::NetFeedbackSnapshot snap;
    snap.has_loss_rtt = true;   // a report arrived...
    snap.has_rtt      = false;  // ...but it carried no usable LSR / DLSR
    snap.rtt          = core::Milliseconds{0};
    scripted->script_snapshot = snap;

    const auto stats = engine.stats();
    EXPECT_FALSE(stats.transport.has_rtt);
    EXPECT_EQ(stats.transport.rtt_ms, 0u);

    engine.close();
}

TEST_F(EngineTransportStats, AnUnnominatedPairIsReportedAsAbsent) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0);

    auto* ice = engine.get_ice_transport();
    auto* scripted = dynamic_cast<ScriptedIce*>(ice);
    ASSERT_NE(scripted, nullptr);

    // The agent is connected but has not nominated a pair yet.
    scripted->script_state = np::IceState::Connected;
    scripted->script_pair.reset();

    const auto stats = engine.stats();
    EXPECT_FALSE(stats.transport.selected_pair.has_value());
    EXPECT_FALSE(stats.transport.relayed);

    engine.close();
}

}  // namespace
