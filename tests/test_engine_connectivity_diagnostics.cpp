/**
 * @file tests/test_engine_connectivity_diagnostics.cpp
 * @brief N4 — a session that never connects says **why**, from the stats alone.
 *
 * ## What is tested
 *
 * The v1.4 plan's N4 row says: *"A session that never connects says why
 * from the stats snapshot alone."*  This test drives the engine through a
 * **fake ICE transport** whose gathering state, candidate list, nominated
 * pair and TURN failure the test controls, and asserts that the engine's
 * `Stats::transport` says:
 *
 *   - how far the **gathering** got ("new" / "gathering" / "complete"),
 *     as a fact *separate* from the agent's own state machine;
 *   - how many candidates of each type were gathered
 *     (host / srflx / relay);
 *   - **why** the session failed, in a closed six-value vocabulary;
 *   - which TURN transport the session asked for.
 *
 * ## The vocabulary is closed, and the order is fixed
 *
 * The failure reason is the point of the whole surface, so two properties
 * are pinned down hard:
 *
 *   - every value the engine can produce is one of the six;
 *   - the values are chosen by a **fixed priority order**, so a session
 *     that is broken in two ways reports the more specific cause (the
 *     one a host can act on) rather than whichever check happened to
 *     run first.
 *
 * ## The negative control
 *
 * The last suite is the important one: a session where **nothing** is
 * wrong must report `"none"` — not a failure, and not an empty string.
 * "Nothing failed" is a fact a host can act on (the session is fine, the
 * network is fine, look elsewhere), and a surface that reported "" for it
 * would be indistinguishable from a surface that had not been told.
 */
#include <gtest/gtest.h>

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

namespace {

namespace np = nimrtc::plugins;
namespace nd = nimrtc::dtls;
namespace core = nimrtc::core;

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

// ---------------------------------------------------------------------------
// A fake ICE transport whose whole story the test dictates.
// ---------------------------------------------------------------------------

/**
 * @brief An ICE transport the test drives directly.
 *
 * The fake exposes everything N4 reads from the ICE seam — the agent's
 * state, the gathering action's state, the gathered candidate list, the
 * nominated pair and the TURN failure reason — and lets the test set
 * each one independently.
 *
 * The seam's two new methods (`gathering_state()` and
 * `turn_failure_reason()`) are **overridden** here rather than left at
 * their defaults: the point of the test is to drive the engine's
 * projection, so the fake has to be able to produce every value the
 * real module can.
 */
class DiagnosticIce final : public np::IICETransport {
public:
    // ---- The script the test sets ----------------------------------------

    np::IceState script_state = np::IceState::Gathering;
    std::string script_gathering_state = "new";
    std::vector<std::string> script_candidates;
    std::optional<np::IceCandidatePair> script_pair;
    std::string script_turn_failure;

    /** The STUN server the engine configured, as the test sees it. */
    std::string script_stun_server_host;
    std::uint16_t script_stun_server_port = 0;

    /** Whether the seam can speak the TURN transport the host asked for. */
    bool script_turn_transport_supported = true;

    // ---- ITransport --------------------------------------------------------

    const char* name() const noexcept override { return "n4-diagnostic-ice"; }
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
    std::string local_ufrag() const noexcept override { return "n4ufrag"; }
    std::string local_password() const noexcept override { return "n4password"; }
    bool wait_for_gathering(int) noexcept override { return true; }
    std::vector<std::string> gathered_local_candidates() const noexcept override {
        return script_candidates;
    }
    np::Status set_remote_description(std::string_view) noexcept override {
        return np::kOk;
    }
    np::Status add_remote_candidate(std::string_view) noexcept override {
        return np::kOk;
    }
    void set_bind_address(std::string_view) noexcept override {}
    void set_stun_server(std::string_view host,
                         std::uint16_t port) noexcept override {
        script_stun_server_host = std::string{host};
        script_stun_server_port = port;
    }
    void set_local_port_range(std::uint16_t, std::uint16_t) noexcept override {}
    void add_turn_server(std::string_view, std::uint16_t, std::string_view,
                         std::string_view) noexcept override {}

    std::optional<np::IceCandidatePair> selected_pair() const noexcept override {
        return script_pair;
    }

    std::uint32_t restart_count() const noexcept override { return 0; }

    void set_bwe(np::IBwe*) noexcept override {}
    void set_scheduler(np::IScheduler*) noexcept override {}

    // ---- The N4 seam -------------------------------------------------------

    std::string_view gathering_state() const noexcept override {
        return script_gathering_state;
    }

    std::string_view turn_failure_reason() const noexcept override {
        return script_turn_failure;
    }

    bool turn_transport_supported(np::TurnTransport) const noexcept override {
        return script_turn_transport_supported;
    }

private:
    np::RecvCallback  on_recv_;
    np::ErrorCallback on_error_;
};

class DiagnosticIceFactory final : public np::IICETransportFactory {
public:
    std::string_view id() const noexcept override { return "n4-diagnostic-ice"; }
    std::string_view display_name() const noexcept override {
        return "Diagnostic ICE (N4 test)";
    }
    np::IICETransport* create_ice() const override { return new DiagnosticIce(); }
};

// ---------------------------------------------------------------------------
// A fake DTLS session — the transport surface needs one to exist.
// ---------------------------------------------------------------------------

class QuietDtls final : public nd::IDtlsSession {
public:
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
    nd::DtlsState state() const noexcept override { return nd::DtlsState::Closed; }
    bool is_connected() const noexcept override { return false; }
    const nd::Fingerprint& local_fingerprint() const noexcept override {
        return fingerprint_;
    }
    std::optional<nd::SrtpKeyingMaterial>
    srtp_keying_material() const noexcept override {
        return std::nullopt;
    }

private:
    nd::Fingerprint fingerprint_{"sha-256", {}, {}, "AA:BB"};
};

class QuietDtlsFactory final : public nd::IDtlsSessionFactory {
public:
    std::string_view id() const noexcept override { return "n4-quiet-dtls"; }
    std::string_view display_name() const noexcept override {
        return "Quiet DTLS (N4 test)";
    }
    std::unique_ptr<nd::IDtlsSession> create(const nd::DtlsConfig&) const override {
        return std::make_unique<QuietDtls>();
    }
};

// ---------------------------------------------------------------------------
// The fixture
// ---------------------------------------------------------------------------

class EngineConnectivityDiagnostics : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();

        static DiagnosticIceFactory ice;
        PluginRegistry::instance().register_ice_transport(ice.id(), &ice);

        static QuietDtlsFactory dtls;
        PluginRegistry::instance().register_dtls_session(dtls.id(), &dtls);
    }

    static EngineConfig make_config() {
        EngineConfig cfg{};
        cfg.transport_name       = "n4-diagnostic-ice";
        cfg.dtls_name            = "n4-quiet-dtls";
        cfg.bwe_name             = "";
        cfg.scheduler_config.enabled = false;
        cfg.datachannel_backend = "";
        // The device layer is off: a capture device on a machine that has
        // one would pump real frames into the send path, and none of this
        // test is about media.  ("none" is the explicit opt-out —
        // ADR-040, plan D1-1.)
        cfg.audio_source_name = "none";
        cfg.audio_sink_name   = "none";
        return cfg;
    }

    /** The fake the engine created, so the test can script it. */
    static DiagnosticIce* script(NimRTCEngine& engine) {
        auto* ice = engine.get_ice_transport();
        auto* fake = dynamic_cast<DiagnosticIce*>(ice);
        EXPECT_NE(fake, nullptr);
        return fake;
    }

    /** A candidate line in the shape the ICE seam publishes them
     *  (RFC 8839 §5.1). */
    static std::string candidate_line(const char* type) {
        return std::string{"candidate:1 1 UDP 2113929471 192.0.2.1 5000 typ "} +
               type;
    }
};

// ---------------------------------------------------------------------------
// The gathering action's state
// ---------------------------------------------------------------------------

/**
 * @brief The gathering state reaches the surface, in all three values.
 */
TEST_F(EngineConnectivityDiagnostics, TheGatheringStateReachesTheSurface) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0);

    auto* fake = script(engine);
    ASSERT_NE(fake, nullptr);

    fake->script_gathering_state = "new";
    EXPECT_EQ(engine.stats().transport.ice_gathering_state, "new");

    fake->script_gathering_state = "gathering";
    EXPECT_EQ(engine.stats().transport.ice_gathering_state, "gathering");

    fake->script_gathering_state = "complete";
    EXPECT_EQ(engine.stats().transport.ice_gathering_state, "complete");

    engine.close();
}

/**
 * @brief The gathering state is **independent** of the agent's state.
 *
 * This is the property that makes the field worth having.  The agent's
 * state machine is about the *session* (RFC 8445 §6); the gathering
 * state is about the *collection* (RFC 8445 §5.1.1).  A reflexive
 * candidate may arrive after the path is already up, so "connected" and
 * "gathering" are a perfectly normal pair — and a surface that derived
 * one from the other would report the wrong thing exactly when the host
 * is asking "why is there no srflx candidate yet".
 */
TEST_F(EngineConnectivityDiagnostics, TheGatheringStateIsIndependentOfTheIceState) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0);

    auto* fake = script(engine);
    ASSERT_NE(fake, nullptr);

    // Connected, but the collection has not finished: a normal state.
    fake->script_state            = np::IceState::Connected;
    fake->script_gathering_state  = "gathering";
    fake->script_candidates       = {candidate_line("host")};

    const auto stats = engine.stats();
    EXPECT_EQ(stats.transport.ice_state, "connected");
    EXPECT_EQ(stats.transport.ice_gathering_state, "gathering");

    engine.close();
}

// ---------------------------------------------------------------------------
// The candidate counts
// ---------------------------------------------------------------------------

/**
 * @brief The candidate counts are per type, and they add up.
 */
TEST_F(EngineConnectivityDiagnostics, TheCandidateCountsArePerType) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0);

    auto* fake = script(engine);
    ASSERT_NE(fake, nullptr);

    fake->script_gathering_state = "complete";
    fake->script_candidates = {
        candidate_line("host"),
        candidate_line("host"),
        candidate_line("srflx"),
        candidate_line("relay"),
    };

    const auto stats = engine.stats();
    EXPECT_EQ(stats.transport.ice_host_candidates, 2u);
    EXPECT_EQ(stats.transport.ice_srflx_candidates, 1u);
    EXPECT_EQ(stats.transport.ice_relay_candidates, 1u);

    // The three counts cover every gathered candidate: a candidate the
    // counter does not know about would be a candidate a host cannot
    // account for.
    const auto total = stats.transport.ice_host_candidates +
                       stats.transport.ice_srflx_candidates +
                       stats.transport.ice_relay_candidates;
    EXPECT_EQ(total, fake->script_candidates.size());

    engine.close();
}

/**
 * @brief A candidate the counter does not know about is not counted.
 *
 * The three published counts are host / srflx / relay.  A line whose
 * `typ` field is something else must not be silently folded into one of
 * them — that would make the counts lie about what was gathered.
 */
TEST_F(EngineConnectivityDiagnostics, AnUnknownCandidateTypeIsNotCounted) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0);

    auto* fake = script(engine);
    ASSERT_NE(fake, nullptr);

    fake->script_gathering_state = "complete";
    fake->script_candidates = {
        candidate_line("host"),
        "candidate:2 1 UDP 2113929471 192.0.2.2 5001 typ prflx",
    };

    const auto stats = engine.stats();
    EXPECT_EQ(stats.transport.ice_host_candidates, 1u);
    EXPECT_EQ(stats.transport.ice_srflx_candidates, 0u);
    EXPECT_EQ(stats.transport.ice_relay_candidates, 0u);

    engine.close();
}

// ---------------------------------------------------------------------------
// The failure vocabulary — one case per value
// ---------------------------------------------------------------------------

/**
 * @brief `"none"` — nothing is wrong.
 *
 * The negative control for the whole surface: a working session must
 * say so, not report an empty string and not invent a failure.
 */
TEST_F(EngineConnectivityDiagnostics, AHealthySessionReportsNoFailure) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0);

    auto* fake = script(engine);
    ASSERT_NE(fake, nullptr);

    fake->script_state           = np::IceState::Connected;
    fake->script_gathering_state = "complete";
    fake->script_candidates      = {candidate_line("host"),
                                     candidate_line("srflx")};

    np::IceCandidatePair pair;
    pair.local.address  = "192.0.2.1";
    pair.local.port     = 5000;
    pair.local.protocol = "udp";
    pair.local.type     = "host";
    pair.remote.address = "198.51.100.7";
    pair.remote.port    = 40000;
    pair.remote.protocol = "udp";
    pair.remote.type    = "srflx";
    fake->script_pair = pair;

    EXPECT_EQ(engine.stats().transport.ice_failure_reason, "none");

    engine.close();
}

/**
 * @brief `"no-candidates"` — the collection finished and produced nothing.
 */
TEST_F(EngineConnectivityDiagnostics, NoCandidatesIsReportedAsSuch) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0);

    auto* fake = script(engine);
    ASSERT_NE(fake, nullptr);

    fake->script_state           = np::IceState::Connecting;
    fake->script_gathering_state = "complete";
    fake->script_candidates.clear();
    fake->script_pair.reset();

    EXPECT_EQ(engine.stats().transport.ice_failure_reason, "no-candidates");

    engine.close();
}

/**
 * @brief `"no-pair"` — there are candidates, but nothing was nominated.
 */
TEST_F(EngineConnectivityDiagnostics, NoNominatedPairIsReportedAsSuch) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0);

    auto* fake = script(engine);
    ASSERT_NE(fake, nullptr);

    fake->script_state           = np::IceState::Connecting;
    fake->script_gathering_state = "complete";
    fake->script_candidates      = {candidate_line("host"),
                                     candidate_line("srflx")};
    fake->script_pair.reset();

    EXPECT_EQ(engine.stats().transport.ice_failure_reason, "no-pair");

    engine.close();
}

/**
 * @brief `"checks-failed"` — the agent gave up.
 */
TEST_F(EngineConnectivityDiagnostics, AFailedAgentIsReportedAsSuch) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0);

    auto* fake = script(engine);
    ASSERT_NE(fake, nullptr);

    fake->script_state           = np::IceState::Failed;
    fake->script_gathering_state = "complete";
    fake->script_candidates      = {candidate_line("host")};
    fake->script_pair.reset();

    EXPECT_EQ(engine.stats().transport.ice_failure_reason, "checks-failed");

    engine.close();
}

/**
 * @brief `"credentials-rejected"` — the TURN server refused the credentials.
 *
 * This is the value the whole seam exists for: libjuice used to report
 * a refused credential and an unreachable server in exactly the same
 * way, so a host could not tell "refresh the credential" from "try
 * another path".
 */
TEST_F(EngineConnectivityDiagnostics, ARefusedCredentialIsReportedAsSuch) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0);

    auto* fake = script(engine);
    ASSERT_NE(fake, nullptr);

    fake->script_state           = np::IceState::Connecting;
    fake->script_gathering_state = "complete";
    fake->script_candidates      = {candidate_line("host")};
    fake->script_pair.reset();
    fake->script_turn_failure = "credentials-rejected";

    EXPECT_EQ(engine.stats().transport.ice_failure_reason, "credentials-rejected");

    engine.close();
}

/**
 * @brief `"transport-unsupported"` — this build cannot speak the TURN
 *  transport the host asked for.
 */
TEST_F(EngineConnectivityDiagnostics, AnUnsupportedTurnTransportIsReportedAsSuch) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0);

    auto* fake = script(engine);
    ASSERT_NE(fake, nullptr);

    // The host asks for a TURN server over a transport this build
    // cannot speak.
    fake->script_turn_transport_supported = false;
    ASSERT_EQ(engine.add_turn_server("turn.example.com", 3478, "u", "p",
                                     np::TurnTransport::Tls),
              0);

    fake->script_state           = np::IceState::Connecting;
    fake->script_gathering_state = "complete";
    fake->script_candidates.clear();
    fake->script_pair.reset();

    EXPECT_EQ(engine.stats().transport.ice_failure_reason,
              "transport-unsupported");

    engine.close();
}

// ---------------------------------------------------------------------------
// The priority order
// ---------------------------------------------------------------------------

/**
 * @brief The more specific cause wins.
 *
 * A session whose TURN transport is unsupported would *also* report
 * "no candidates", and one whose credentials were refused would also
 * report "no candidates".  The vocabulary is chosen by a fixed order
 * that runs from the most specific cause to the least, because the
 * specific one is the one a host can act on.
 */
TEST_F(EngineConnectivityDiagnostics, TheMoreSpecificCauseWins) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0);

    auto* fake = script(engine);
    ASSERT_NE(fake, nullptr);

    // Broken in *every* way at once: no candidates, no pair, the agent
    // failed, the credentials were refused, and the transport is not
    // supported.  The host has configured a TURN server, so the
    // transport question is live.
    fake->script_turn_transport_supported = false;
    ASSERT_EQ(engine.add_turn_server("turn.example.com", 3478, "u", "p",
                                     np::TurnTransport::Tls),
              0);

    fake->script_state           = np::IceState::Failed;
    fake->script_gathering_state = "complete";
    fake->script_candidates.clear();
    fake->script_pair.reset();
    fake->script_turn_failure = "credentials-rejected";

    // "transport-unsupported" is the most specific of the five.
    EXPECT_EQ(engine.stats().transport.ice_failure_reason,
              "transport-unsupported");

    // With the transport supported, the refused credential is next.
    fake->script_turn_transport_supported = true;
    EXPECT_EQ(engine.stats().transport.ice_failure_reason,
              "credentials-rejected");

    // With the credentials accepted, the absent candidates are next.
    fake->script_turn_failure.clear();
    EXPECT_EQ(engine.stats().transport.ice_failure_reason, "no-candidates");

    engine.close();
}

/**
 * @brief The vocabulary is **closed**: every value is one of the six.
 *
 * A surface whose failure reason could be an arbitrary string would
 * give a host a sentence to log and nothing to branch on.  The check
 * runs over every scenario this test can produce, and each one has to
 * land inside the six.
 */
TEST_F(EngineConnectivityDiagnostics, EveryReportedReasonIsInTheClosedVocabulary) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0);

    auto* fake = script(engine);
    ASSERT_NE(fake, nullptr);

    static const char* kAllowed[] = {
        "none", "no-candidates", "no-pair", "checks-failed",
        "credentials-rejected", "transport-unsupported",
    };

    // Walk a few combinations and check each answer is inside the set.
    const np::IceState states[] = {
        np::IceState::Disconnected, np::IceState::Gathering,
        np::IceState::Connecting,   np::IceState::Connected,
        np::IceState::Completed,    np::IceState::Failed,
    };
    const char* gathering[] = {"new", "gathering", "complete"};
    const char* failures[] = {"", "credentials-rejected", "stale-nonce",
                              "transport-unavailable", "allocate-refused"};

    for (const auto state : states) {
        for (const auto* g : gathering) {
            for (const auto* f : failures) {
                fake->script_state           = state;
                fake->script_gathering_state = g;
                fake->script_candidates.clear();
                fake->script_pair.reset();
                fake->script_turn_failure = f;
                fake->script_turn_transport_supported = true;

                const std::string reason =
                    engine.stats().transport.ice_failure_reason;
                bool allowed = false;
                for (const auto* candidate : kAllowed) {
                    if (reason == candidate) { allowed = true; break; }
                }
                EXPECT_TRUE(allowed)
                    << "the failure reason \"" << reason << "\" is outside "
                    << "the closed vocabulary";
            }
        }
    }

    engine.close();
}

// ---------------------------------------------------------------------------
// The TURN transport
// ---------------------------------------------------------------------------

/**
 * @brief The TURN transport in use reaches the surface.
 *
 * This is the field that would catch a **silent downgrade**: a session
 * configured for TURN over TLS that ends up on plain UDP is a weaker
 * session, and the host has no other way to learn it happened.
 */
TEST_F(EngineConnectivityDiagnostics, TheTurnTransportReachesTheSurface) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0);

    // The plain overload is TURN over UDP (RFC 5766).
    EXPECT_EQ(engine.add_turn_server("turn.example.com", 3478, "u", "p"), 0);
    EXPECT_EQ(engine.stats().transport.turn_transport_used, "udp");

    engine.close();
}

/**
 * @brief A session with no TURN server reports no TURN transport.
 *
 * Empty is a fact here: "this session does not use TURN" is different
 * from "this session uses TURN over UDP", and a host branching on the
 * field needs to be able to tell them apart.
 */
TEST_F(EngineConnectivityDiagnostics, NoTurnServerMeansNoTurnTransport) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0);

    EXPECT_TRUE(engine.stats().transport.turn_transport_used.empty());

    engine.close();
}

// ---------------------------------------------------------------------------
// D6 -- the connectivity profile
// ---------------------------------------------------------------------------

/**
 * @brief The default profile is `"auto"`, and `"auto"` means **on**.
 *
 * The whole point of a connectivity profile is that a host says "a
 * conference call" instead of spelling out the traversal policy.  The
 * default therefore has to mean "on" -- a profile that defaulted to
 * "off" would make the SDK's promise ("just works on a real network")
 * false by default.
 */
TEST_F(EngineConnectivityDiagnostics, TheDefaultProfileIsAuto) {
    EngineConfig cfg = make_config();
    EXPECT_EQ(cfg.connectivity_profile, "auto");
}

/**
 * @brief The profile fills in the STUN server the host left alone.
 *
 * The fake ICE transport records the STUN server it was given, so the
 * test can see what the engine configured.
 */
TEST_F(EngineConnectivityDiagnostics, TheProfileConfiguresTheDefaultStunServer) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0);

    auto* fake = script(engine);
    ASSERT_NE(fake, nullptr);

    // The host named no STUN server, so the profile filled one in.
    EXPECT_EQ(fake->script_stun_server_host, "stun.l.google.com");
    EXPECT_EQ(fake->script_stun_server_port, 3478u);

    engine.close();
}

/**
 * @brief The host's explicit STUN server **wins**.
 *
 * A profile is a set of *defaults*; it never overrides a key the host
 * set.  This is the property that makes the profile safe to add to an
 * SDK: the profile cannot change the behaviour of a host that already
 * configures its own traversal.
 */
TEST_F(EngineConnectivityDiagnostics, TheHostsStunServerWins) {
    EngineConfig cfg = make_config();
    cfg.stun_server_host = "stun.example.com";
    cfg.stun_server_port = 3479;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0);

    auto* fake = script(engine);
    ASSERT_NE(fake, nullptr);

    EXPECT_EQ(fake->script_stun_server_host, "stun.example.com");
    EXPECT_EQ(fake->script_stun_server_port, 3479u);

    engine.close();
}

/**
 * @brief An unknown profile is not an error — the engine falls back.
 *
 * A profile is a convenience, and refusing to start a session over a
 * typo would be a worse trade than running with the default policy.
 */
TEST_F(EngineConnectivityDiagnostics, AnUnknownProfileFallsBackToAuto) {
    EngineConfig cfg = make_config();
    cfg.connectivity_profile = "no-such-profile";

    NimRTCEngine engine(cfg);
    // The session still starts.
    ASSERT_EQ(engine.open(), 0);

    auto* fake = script(engine);
    ASSERT_NE(fake, nullptr);

    // ...and it runs with the "auto" policy: the default STUN server is
    // configured.
    EXPECT_EQ(fake->script_stun_server_host, "stun.l.google.com");

    engine.close();
}

}  // namespace
