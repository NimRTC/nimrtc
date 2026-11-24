/**
 * @file src/engine/tests/test_engine_path_change.cpp
 * @brief v1-path-change-report (N8) — the path-change ledger and the three
 *        report points.
 *
 * ## What this file proves
 *
 * The slice has one engine-private collaborator — `detail::PathChangeTracker`,
 * the "which pair was the previous one" ledger plus the pure reason
 * classification.  The first half pins it with pure functions (no engine, no
 * clock): the five `classify` cases, the field-by-field `same_path`, the
 * `note_before` / `before` / `reset` round-trip, and `classify`'s purity.
 *
 * The second half drives a real `NimRTCEngine` with a *stateful* fake ICE
 * transport — its `selected_pair()` is a knob the test turns, and its
 * `restart()` advances to the pair the network "settled on" afterwards — to
 * prove the wiring end-to-end:
 *
 *   - an N3 automatic restart reports the old pair (recorded before the
 *     restart) and the new pair (read after it), with the N3 reason;
 *   - a relay-bit flip between the two pairs refines the reason to
 *     "relay-fallback" whatever the trigger;
 *   - `restart_ice()` reports reason "manual" with the pair context;
 *   - a restart with no pair selected yet reports empty pairs and does not
 *     crash; a restart whose new pair is still gathering reports an empty
 *     `new_pair`;
 *   - a peer restart fires `set_on_peer_restart` (reason "peer-restart",
 *     old == new — the local path did not change) while the old
 *     `set_on_ice_restart` still fires;
 *   - the flat `Stats::last_path_change_*` projection follows the reports;
 *   - callbacks fire on the thread that drives the engine.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/dtls/dtls_session_factory.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/ice_transport.hpp>

#include "engine_path_change.hpp"

namespace {

namespace np = nimrtc::plugins;
namespace nd = nimrtc::dtls;

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
using PathChangeTracker = nimrtc::engine::detail::PathChangeTracker;
using IceCandidatePairInfo = nimrtc::engine::IceCandidatePairInfo;

// ===========================================================================
// Helpers — build the two pair shapes the slice juggles: the engine's public
// `IceCandidatePairInfo` (what reports carry) and the seam's
// `plugins::IceCandidatePair` (what the fake transport returns).
// ===========================================================================

IceCandidatePairInfo make_info(std::string laddr, std::uint16_t lport,
                               std::string ltype, std::string raddr,
                               std::uint16_t rport, std::string rtype) {
    IceCandidatePairInfo p;
    p.local.address  = std::move(laddr);
    p.local.port     = lport;
    p.local.protocol = "udp";
    p.local.type     = std::move(ltype);
    p.remote.address = std::move(raddr);
    p.remote.port    = rport;
    p.remote.protocol = "udp";
    p.remote.type    = std::move(rtype);
    return p;
}

np::IceCandidate make_candidate(std::string address, std::uint16_t port,
                                std::string type) {
    np::IceCandidate c;
    c.address  = std::move(address);
    c.port     = port;
    c.protocol = "udp";
    c.type     = std::move(type);
    return c;
}

np::IceCandidatePair make_plugin_pair(const IceCandidatePairInfo& info) {
    np::IceCandidatePair p;
    p.local  = make_candidate(info.local.address, info.local.port,
                              info.local.type);
    p.remote = make_candidate(info.remote.address, info.remote.port,
                              info.remote.type);
    return p;
}

// ===========================================================================
// The machine — `PathChangeTracker` (pure, deterministic)
// ===========================================================================

TEST(PathChangeTracker, ClassifyBothEmptyIsEmpty) {
    // Criterion 17: no old pair, no new pair → nothing to report, no crash.
    EXPECT_TRUE(
        PathChangeTracker::classify(std::nullopt, std::nullopt).empty());
}

TEST(PathChangeTracker, ClassifyFirstSelection) {
    // Criterion 16: nothing selected before, selected now →
    // "local-address-changed".
    const auto newp = make_info("1.1.1.1", 1000, "host", "2.2.2.2", 2000,
                                "host");
    EXPECT_EQ(std::string(PathChangeTracker::classify(std::nullopt, newp)),
              "local-address-changed");
}

TEST(PathChangeTracker, ClassifyPairDropped) {
    // Criterion 15: the old pair was selected, the new one is not yet →
    // "local-address-changed".
    const auto oldp = make_info("1.1.1.1", 1000, "host", "2.2.2.2", 2000,
                                "host");
    EXPECT_EQ(std::string(PathChangeTracker::classify(oldp, std::nullopt)),
              "local-address-changed");
}

TEST(PathChangeTracker, ClassifyRelayFlip) {
    // Criterion 14: direct → relay (or relay → direct) → "relay-fallback".
    const auto direct = make_info("1.1.1.1", 1000, "host", "2.2.2.2", 2000,
                                  "host");
    const auto relayed = make_info("1.1.1.1", 1000, "host", "3.3.3.3", 3478,
                                   "relay");
    EXPECT_EQ(std::string(PathChangeTracker::classify(direct, relayed)),
              "relay-fallback");
    EXPECT_EQ(std::string(PathChangeTracker::classify(relayed, direct)),
              "relay-fallback");

    // Either end of the pair being relay makes the path relayed — the same
    // bit `TransportStats::relayed` uses.
    const auto relayed_local = make_info("4.4.4.4", 3478, "relay", "2.2.2.2",
                                         2000, "host");
    EXPECT_EQ(std::string(PathChangeTracker::classify(direct, relayed_local)),
              "relay-fallback");
}

TEST(PathChangeTracker, ClassifySameRelayBit) {
    // Criterion 15: both present with the same relay bit → a plain local
    // address change.
    const auto a = make_info("1.1.1.1", 1000, "host", "2.2.2.2", 2000, "host");
    const auto b = make_info("9.9.9.9", 9000, "host", "8.8.8.8", 8000, "host");
    EXPECT_EQ(std::string(PathChangeTracker::classify(a, b)),
              "local-address-changed");

    const auto r1 = make_info("1.1.1.1", 1000, "host", "2.2.2.2", 3478,
                              "relay");
    const auto r2 = make_info("9.9.9.9", 9000, "host", "8.8.8.8", 3478,
                              "relay");
    EXPECT_EQ(std::string(PathChangeTracker::classify(r1, r2)),
              "local-address-changed");
}

TEST(PathChangeTracker, ClassifyIsPure) {
    // C3: a decision must be deterministic — the same two pairs give the
    // same reason on every call.
    const auto direct = make_info("1.1.1.1", 1000, "host", "2.2.2.2", 2000,
                                  "host");
    const auto relayed = make_info("1.1.1.1", 1000, "host", "3.3.3.3", 3478,
                                   "relay");
    EXPECT_EQ(std::string(PathChangeTracker::classify(direct, relayed)),
              std::string(PathChangeTracker::classify(direct, relayed)));
    EXPECT_EQ(std::string(PathChangeTracker::classify(std::nullopt,
                                                      std::nullopt)),
              std::string(PathChangeTracker::classify(std::nullopt,
                                                      std::nullopt)));
}

TEST(PathChangeTracker, SamePathComparesEveryField) {
    const auto base = make_info("1.1.1.1", 1000, "host", "2.2.2.2", 2000,
                                "host");
    EXPECT_TRUE(PathChangeTracker::same_path(base, base));

    // Each of the eight fields differing makes it a different path.
    {
        auto b = base; b.local.address = "9.9.9.9";
        EXPECT_FALSE(PathChangeTracker::same_path(base, b));
    }
    {
        auto b = base; b.local.port = 9999;
        EXPECT_FALSE(PathChangeTracker::same_path(base, b));
    }
    {
        auto b = base; b.local.protocol = "tcp";
        EXPECT_FALSE(PathChangeTracker::same_path(base, b));
    }
    {
        auto b = base; b.local.type = "relay";
        EXPECT_FALSE(PathChangeTracker::same_path(base, b));
    }
    {
        auto b = base; b.remote.address = "8.8.8.8";
        EXPECT_FALSE(PathChangeTracker::same_path(base, b));
    }
    {
        auto b = base; b.remote.port = 8888;
        EXPECT_FALSE(PathChangeTracker::same_path(base, b));
    }
    {
        auto b = base; b.remote.protocol = "tcp";
        EXPECT_FALSE(PathChangeTracker::same_path(base, b));
    }
    {
        auto b = base; b.remote.type = "relay";
        EXPECT_FALSE(PathChangeTracker::same_path(base, b));
    }
}

TEST(PathChangeTracker, NoteBeforeBeforeReset) {
    PathChangeTracker tracker;
    EXPECT_FALSE(tracker.before().has_value());

    const auto p = make_info("1.1.1.1", 1000, "host", "2.2.2.2", 2000, "host");
    tracker.note_before(p);
    ASSERT_TRUE(tracker.before().has_value());
    EXPECT_TRUE(PathChangeTracker::same_path(*tracker.before(), p));

    tracker.note_before(std::nullopt);
    EXPECT_FALSE(tracker.before().has_value())
        << "note_before(nullopt) must clear, not keep the old value";

    tracker.note_before(p);
    tracker.reset();
    EXPECT_FALSE(tracker.before().has_value());
}

// ===========================================================================
// A stateful fake ICE transport + a fake DTLS session (the audio_alloc /
// ice-recovery pattern), so the live engine tests below drive a real
// `NimRTCEngine`.
//
// Unlike the N3 fake, `selected_pair()` is a knob the test turns, and
// `restart()` advances to the pair the network "settled on" afterwards —
// that is how a wiring test proves the report carries a *different* new
// pair without ever sleeping on the transport.
// ===========================================================================

class StatefulPathFakeIce final : public np::IICETransport {
public:
    const char* name() const noexcept override { return "test-path-change-ice"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    void set_callbacks(np::RecvCallback on_recv,
                       np::ErrorCallback on_error) noexcept override {
        on_recv_  = std::move(on_recv);
        on_error_ = std::move(on_error);
    }

    np::Status send(np::BufferView, np::Addr = {}) noexcept override {
        return np::kOk;
    }
    int recv() noexcept override { return 0; }
    np::Addr local_addr() const noexcept override { return {}; }
    np::Addr remote_addr() const noexcept override { return {}; }

    np::IceState state() const noexcept override { return state_; }
    std::string local_ufrag() const noexcept override {
        return "ufrag" + std::to_string(restarts_);
    }
    std::string local_password() const noexcept override { return "pwd"; }
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

    std::optional<np::IceCandidatePair> selected_pair() const noexcept override {
        return selected_pair_;
    }

    np::Status restart() noexcept override {
        ++restarts_;
        if (next_pair_) {
            // The network settled on a new path after the restart.
            selected_pair_ = next_pair_;
        } else {
            // Simulate a restart whose new pair is still gathering: the
            // transport has nothing selected yet.
            selected_pair_ = std::nullopt;
        }
        return np::kOk;
    }
    std::uint32_t restart_count() const noexcept override { return restarts_; }

    void set_on_ice_restart(np::OnIceRestart cb) noexcept override {
        on_ice_restart_ = std::move(cb);
    }
    void fire_peer_restart() noexcept {
        if (on_ice_restart_) on_ice_restart_();
    }

    // The test's knobs.
    void set_state(np::IceState s) noexcept { state_ = s; }
    void set_selected_pair(std::optional<np::IceCandidatePair> p) noexcept {
        selected_pair_ = std::move(p);
    }
    void set_next_pair(std::optional<np::IceCandidatePair> p) noexcept {
        next_pair_ = std::move(p);
    }

private:
    np::RecvCallback on_recv_;
    np::ErrorCallback on_error_;
    np::OnIceRestart on_ice_restart_;
    np::IceState state_ = np::IceState::Disconnected;
    std::uint32_t restarts_ = 0;
    std::optional<np::IceCandidatePair> selected_pair_;
    std::optional<np::IceCandidatePair> next_pair_;
};

class StatefulPathFakeIceFactory final : public np::IICETransportFactory {
public:
    std::string_view id() const noexcept override { return "test-path-change-ice"; }
    std::string_view display_name() const noexcept override {
        return "Stateful fake ICE (path-change test)";
    }
    np::IICETransport* create_ice() const override {
        auto* ice = new StatefulPathFakeIce();
        last_ = ice;
        return ice;
    }
    StatefulPathFakeIce* last() const noexcept { return last_; }

private:
    mutable StatefulPathFakeIce* last_ = nullptr;
};

class FakeDtlsSession final : public nd::IDtlsSession {
public:
    void set_role(nd::DtlsRole) noexcept override {}
    void set_peer_fingerprint(std::span<const std::uint8_t>) noexcept override {}
    void start() noexcept override {}
    void pump() noexcept override {}
    void on_handshake_complete(nd::IDtlsSession::OnCompleteCb) noexcept override {}
    np::Status export_srtp_key_material(std::span<std::uint8_t, 60>) noexcept override {
        return np::kErrNotReady;
    }

    nimrtc::core::Result<void> open() noexcept override {
        return nimrtc::core::Result<void>::make_ok();
    }
    void set_role(nd::Role) noexcept override {}
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
    std::string_view id() const noexcept override { return "test-path-change-dtls"; }
    std::string_view display_name() const noexcept override {
        return "Fake DTLS session (path-change test)";
    }
    std::unique_ptr<nd::IDtlsSession> create(const nd::DtlsConfig&) const override {
        return std::make_unique<FakeDtlsSession>();
    }
};

// ===========================================================================
// The live-engine fixture
// ===========================================================================

class EnginePathChangeLive : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
        static StatefulPathFakeIceFactory ice_factory;
        static FakeDtlsFactory dtls_factory;
        auto& reg = PluginRegistry::instance();
        reg.register_ice_transport("test-path-change-ice", &ice_factory);
        reg.register_dtls_session("test-path-change-dtls", &dtls_factory);
        kIceFactory = &ice_factory;
    }

    static EngineConfig make_config() {
        EngineConfig cfg{};
        cfg.transport_name       = "test-path-change-ice";
        cfg.dtls_name            = "test-path-change-dtls";
        cfg.datachannel_backend  = "";
        cfg.bwe_name             = "";
        cfg.scheduler_config.enabled = false;
        cfg.pcm_sample_rate_hz   = 48000;
        cfg.pcm_channels         = 1;
        cfg.codec_name           = "";  // no audio: SFU-style profile
        cfg.audio3a_name         = "";
        cfg.audio_decoder_name   = "";
        cfg.audio_receiver_name  = "";
        cfg.video_receiver_name  = "";
        return cfg;
    }

    static StatefulPathFakeIceFactory* kIceFactory;
};

StatefulPathFakeIceFactory* EnginePathChangeLive::kIceFactory = nullptr;

// ===========================================================================
// Criteria 8 / 9 — an N3 automatic restart reports the old pair (recorded
// before the restart) and the new pair (read after it), with the N3 reason.
// ===========================================================================

TEST_F(EnginePathChangeLive, AutoRestartReportsOldAndNewPairs) {
    EngineConfig cfg = make_config();
    cfg.ice_recovery_policy = "auto";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    auto* ice = static_cast<StatefulPathFakeIce*>(kIceFactory->last());
    ASSERT_NE(ice, nullptr);

    std::vector<NimRTCEngine::IceRecoveryReport> reports;
    engine.set_on_ice_recovery(
        [&reports](const NimRTCEngine::IceRecoveryReport& r) {
            reports.push_back(r);
        });

    const auto old_info = make_info("1.1.1.1", 1000, "host", "2.2.2.2", 2000,
                                    "host");
    const auto new_info = make_info("9.9.9.9", 9000, "host", "8.8.8.8", 8000,
                                    "host");
    ice->set_selected_pair(make_plugin_pair(old_info));
    ice->set_next_pair(make_plugin_pair(new_info));

    // The session establishes once on the old path.
    ice->set_state(np::IceState::Connected);
    engine.tick();
    EXPECT_TRUE(engine.is_ice_connected());
    EXPECT_EQ(ice->restart_count(), 0u);

    // The network drops; the machine enters the grace window, then the first
    // backoff lapses and the engine restarts ICE on its own.
    ice->set_state(np::IceState::Disconnected);
    engine.tick();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (ice->restart_count() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        engine.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_GT(ice->restart_count(), 0u)
        << "the engine must restart ICE itself after the grace window";

    ASSERT_FALSE(reports.empty());
    const auto& r = reports.front();
    EXPECT_EQ(r.reason, "local-address-changed");
    EXPECT_EQ(r.restart_count, 1u);
    ASSERT_TRUE(r.old_pair.has_value());
    ASSERT_TRUE(r.new_pair.has_value());
    EXPECT_TRUE(PathChangeTracker::same_path(*r.old_pair, old_info))
        << "old_pair is the pair recorded before the restart";
    EXPECT_TRUE(PathChangeTracker::same_path(*r.new_pair, new_info))
        << "new_pair is the pair the session is on after the restart";

    // The flat `Stats` projection carries the same facts.
    const auto st = engine.stats();
    EXPECT_EQ(st.last_path_change_reason, "local-address-changed");
    ASSERT_TRUE(st.last_path_change_new_pair.has_value());
    EXPECT_TRUE(
        PathChangeTracker::same_path(*st.last_path_change_new_pair, new_info));
    engine.close();
}

TEST_F(EnginePathChangeLive, RelayFlipRefinesTheReasonToRelayFallback) {
    // Criterion 14 at the wiring level: whatever the trigger, a direct →
    // relay flip reports "relay-fallback".
    EngineConfig cfg = make_config();
    cfg.ice_recovery_policy = "auto";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    auto* ice = static_cast<StatefulPathFakeIce*>(kIceFactory->last());
    ASSERT_NE(ice, nullptr);

    std::vector<NimRTCEngine::IceRecoveryReport> reports;
    engine.set_on_ice_recovery(
        [&reports](const NimRTCEngine::IceRecoveryReport& r) {
            reports.push_back(r);
        });

    const auto direct = make_info("1.1.1.1", 1000, "host", "2.2.2.2", 2000,
                                  "host");
    const auto relayed = make_info("9.9.9.9", 9000, "host", "3.3.3.3", 3478,
                                   "relay");
    ice->set_selected_pair(make_plugin_pair(direct));
    ice->set_next_pair(make_plugin_pair(relayed));

    ice->set_state(np::IceState::Connected);
    engine.tick();
    ice->set_state(np::IceState::Disconnected);
    engine.tick();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (ice->restart_count() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        engine.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_GT(ice->restart_count(), 0u);

    ASSERT_FALSE(reports.empty());
    EXPECT_EQ(reports.front().reason, "relay-fallback");
    engine.close();
}

// ===========================================================================
// Criterion 10 — `restart_ice()` reports reason "manual" with the pair
// context.
// ===========================================================================

TEST_F(EnginePathChangeLive, ManualRestartReportsManualWithPairs) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);
    auto* ice = static_cast<StatefulPathFakeIce*>(kIceFactory->last());
    ASSERT_NE(ice, nullptr);

    std::vector<NimRTCEngine::IceRecoveryReport> reports;
    engine.set_on_ice_recovery(
        [&reports](const NimRTCEngine::IceRecoveryReport& r) {
            reports.push_back(r);
        });

    const auto old_info = make_info("1.1.1.1", 1000, "host", "2.2.2.2", 2000,
                                    "host");
    const auto new_info = make_info("9.9.9.9", 9000, "host", "8.8.8.8", 8000,
                                    "host");
    ice->set_selected_pair(make_plugin_pair(old_info));
    ice->set_next_pair(make_plugin_pair(new_info));

    EXPECT_TRUE(engine.restart_ice());
    EXPECT_EQ(engine.ice_restart_count(), 1u);

    ASSERT_FALSE(reports.empty());
    const auto& r = reports.back();
    EXPECT_EQ(r.reason, "manual");
    ASSERT_TRUE(r.old_pair.has_value());
    ASSERT_TRUE(r.new_pair.has_value());
    EXPECT_TRUE(PathChangeTracker::same_path(*r.old_pair, old_info));
    EXPECT_TRUE(PathChangeTracker::same_path(*r.new_pair, new_info));

    const auto st = engine.stats();
    EXPECT_EQ(st.last_path_change_reason, "manual");
    EXPECT_TRUE(
        PathChangeTracker::same_path(*st.last_path_change_new_pair, new_info));
    engine.close();
}

// ===========================================================================
// Criteria 17 / 18 — empty pairs are honest, never faked, and never crash.
// ===========================================================================

TEST_F(EnginePathChangeLive, ManualRestartWithNoPairSelectedDoesNotCrash) {
    // No pair was ever selected → old_pair / new_pair are empty; the N3
    // reason is still reported unchanged (criterion 2 / 17).
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);
    auto* ice = static_cast<StatefulPathFakeIce*>(kIceFactory->last());
    ASSERT_NE(ice, nullptr);

    std::vector<NimRTCEngine::IceRecoveryReport> reports;
    engine.set_on_ice_recovery(
        [&reports](const NimRTCEngine::IceRecoveryReport& r) {
            reports.push_back(r);
        });

    ASSERT_TRUE(engine.restart_ice());
    ASSERT_FALSE(reports.empty());
    const auto& r = reports.back();
    EXPECT_EQ(r.reason, "manual");
    EXPECT_FALSE(r.old_pair.has_value());
    EXPECT_FALSE(r.new_pair.has_value());
    engine.close();
}

TEST_F(EnginePathChangeLive, NewPairEmptyWhileStillGathering) {
    // Criterion 18: the restart executed but the new pair has not been
    // selected yet → `new_pair` is empty; the host can tell "not selected
    // yet" from a nullopt, not from a fake pair.
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);
    auto* ice = static_cast<StatefulPathFakeIce*>(kIceFactory->last());
    ASSERT_NE(ice, nullptr);

    std::vector<NimRTCEngine::IceRecoveryReport> reports;
    engine.set_on_ice_recovery(
        [&reports](const NimRTCEngine::IceRecoveryReport& r) {
            reports.push_back(r);
        });

    const auto old_info = make_info("1.1.1.1", 1000, "host", "2.2.2.2", 2000,
                                    "host");
    ice->set_selected_pair(make_plugin_pair(old_info));
    // No next pair: after the restart the transport has nothing selected.

    ASSERT_TRUE(engine.restart_ice());
    ASSERT_FALSE(reports.empty());
    const auto& r = reports.back();
    ASSERT_TRUE(r.old_pair.has_value());
    EXPECT_TRUE(PathChangeTracker::same_path(*r.old_pair, old_info));
    EXPECT_FALSE(r.new_pair.has_value());
    engine.close();
}

// ===========================================================================
// Criterion 11 — a peer restart fires `set_on_peer_restart` with reason
// "peer-restart" and the pair context, while the old `set_on_ice_restart`
// keeps its original meaning and still fires.
// ===========================================================================

TEST_F(EnginePathChangeLive, PeerRestartFiresNewCallbackAndOldStillFires) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);
    auto* ice = static_cast<StatefulPathFakeIce*>(kIceFactory->last());
    ASSERT_NE(ice, nullptr);

    const auto info = make_info("1.1.1.1", 1000, "host", "2.2.2.2", 2000,
                                "host");
    ice->set_selected_pair(make_plugin_pair(info));

    std::vector<NimRTCEngine::PeerRestartReport> reports;
    int old_callback_fires = 0;
    engine.set_on_ice_restart([&old_callback_fires]() { ++old_callback_fires; });
    engine.set_on_peer_restart(
        [&reports](const NimRTCEngine::PeerRestartReport& r) {
            reports.push_back(r);
        });

    ice->fire_peer_restart();

    EXPECT_EQ(old_callback_fires, 1)
        << "set_on_ice_restart must keep firing — the new callback does not "
           "replace it";
    ASSERT_EQ(reports.size(), 1u);
    const auto& r = reports.front();
    EXPECT_EQ(r.reason, "peer-restart");
    // A peer restart changes the *credentials*, not the local path — so the
    // old and new pairs are the same (spec §五 三 / the risk table).
    ASSERT_TRUE(r.old_pair.has_value());
    ASSERT_TRUE(r.new_pair.has_value());
    EXPECT_TRUE(PathChangeTracker::same_path(*r.old_pair, info));
    EXPECT_TRUE(PathChangeTracker::same_path(*r.new_pair, info));

    EXPECT_EQ(engine.stats().last_path_change_reason, "peer-restart");
    engine.close();
}

TEST_F(EnginePathChangeLive, PeerRestartCallbackFiresOnDrivingThread) {
    // Criterion 21: the callback runs on the thread that drives the engine
    // (here, the test thread that fires the peer restart) — never on some
    // plugin-internal worker.
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);
    auto* ice = static_cast<StatefulPathFakeIce*>(kIceFactory->last());
    ASSERT_NE(ice, nullptr);

    const auto driving_thread = std::this_thread::get_id();
    std::thread::id callback_thread;
    engine.set_on_peer_restart(
        [&callback_thread](const NimRTCEngine::PeerRestartReport&) {
            callback_thread = std::this_thread::get_id();
        });

    ice->fire_peer_restart();
    EXPECT_EQ(callback_thread, driving_thread);
    engine.close();
}

// ===========================================================================
// The flat `Stats` projection — a fresh open() starts empty.
// ===========================================================================

TEST_F(EnginePathChangeLive, FreshSessionStartsWithNoPathChange) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    const auto st = engine.stats();
    EXPECT_TRUE(st.last_path_change_reason.empty());
    EXPECT_FALSE(st.last_path_change_new_pair.has_value());
    engine.close();
}

}  // namespace
