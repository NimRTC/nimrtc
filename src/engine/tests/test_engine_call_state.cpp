/**
 * @file src/engine/tests/test_engine_call_state.cpp
 * @brief v1-call-state-surface (v1.4 D7, ADR-035) — one call's lifecycle.
 *
 * ## What this file proves
 *
 * The slice has one engine-private collaborator — `detail::CallStateMachine`,
 * the "translate engine events into one state migration, and report at most
 * once per real migration" machine — and one pure mapping,
 * `detail::failure_from_reason()`.  The first half of this file pins both
 * with pure functions: the translation of the six ICE states onto the five
 * call states, the no-repeat judge, the enter-reconnecting / fail / close
 * migrations (and the end-state absorbers), `reset()`, and the six-entry N4
 * vocabulary mapping.
 *
 * The second half drives a real `NimRTCEngine` with the same *stateful* fake
 * ICE transport (its `state()` is a knob the test turns, and `restart()` /
 * `restart_count()` are real counters) plus a *stateful* fake DTLS session
 * (a `state()` knob too, so a definitive DTLS failure can be staged), to
 * prove the wiring end-to-end:
 *
 *   - `open()` → one `kConnecting` report (acceptance 7);
 *   - ICE up → one `kConnected` report, and the same state is never repeated
 *     (acceptance 8 / 12);
 *   - an N3 restart → one `kReconnecting` report carrying the N3 / N8
 *     details (acceptance 9 / 13 / 14);
 *   - the N3 terminal → one `kFailed` report with `failure == kIceFailed`,
 *     the N4 reason and the spent budget (acceptance 10 / 15);
 *   - a DTLS failure → `kFailed` with `failure == kDtlsFailed`, and `kFailed`
 *     is absorbing — ICE staying up does not resurrect the report (the
 *     oscillation guard, ADR-035 §2);
 *   - `start()` / `stop()` / `hangup()` → `kClosed` (acceptance 11 / 22-25);
 *   - the callback fires on the engine's (tick) thread (acceptance 26).
 */
#include <gtest/gtest.h>

#include <algorithm>
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

#include <nimrtc/core/engine_errors.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/dtls/dtls_session_factory.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/ice_transport.hpp>

#include "engine_call_state.hpp"

namespace {

namespace np = nimrtc::plugins;
namespace nd = nimrtc::dtls;

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
using CallStateMachine = nimrtc::engine::detail::CallStateMachine;
using CallState = NimRTCEngine::CallState;
using CallFailure = NimRTCEngine::CallFailure;

// ===========================================================================
// The pure mapping — the N4 failure vocabulary (spec §三 15-19 / ADR-035 §2)
// ===========================================================================

TEST(EngineCallState, FailureFromReasonMapsTheN4Vocabulary) {
    EXPECT_EQ(nimrtc::engine::detail::failure_from_reason("credentials-rejected"),
              CallFailure::kCredentialsRejected);
    EXPECT_EQ(nimrtc::engine::detail::failure_from_reason("transport-unsupported"),
              CallFailure::kTurnRefused);
    for (const char* s : {"checks-failed", "no-pair", "no-candidates"}) {
        EXPECT_EQ(nimrtc::engine::detail::failure_from_reason(s),
                  CallFailure::kIceFailed)
            << "ICE failure reason '" << s << "' maps to kIceFailed";
    }
    // "none" (and anything unrecognised) is NOT a failure — acceptance 19:
    // a healthy session is never misreported as failed.
    EXPECT_EQ(nimrtc::engine::detail::failure_from_reason("none"),
              CallFailure::kNone);
    EXPECT_EQ(nimrtc::engine::detail::failure_from_reason(""),
              CallFailure::kNone);
    EXPECT_EQ(nimrtc::engine::detail::failure_from_reason("no-such-reason"),
              CallFailure::kNone);
}

// ===========================================================================
// The machine — the first observation (spec §二 / acceptance 7)
// ===========================================================================

TEST(EngineCallState, FirstObservationAlwaysReportsConnecting) {
    // Whatever the ICE layer already says, the very first observation of a
    // fresh machine is the "the call is being established" report — that is
    // what `open()` fires.
    CallStateMachine cs;
    EXPECT_TRUE(cs.observe(np::IceState::Disconnected));
    EXPECT_EQ(cs.state(), CallState::kConnecting);

    CallStateMachine already_up;
    EXPECT_TRUE(already_up.observe(np::IceState::Connected));
    EXPECT_EQ(already_up.state(), CallState::kConnecting);
}

// ===========================================================================
// The machine — observe()'s translation of the six ICE states
// ===========================================================================

TEST(EngineCallState, ObserveTranslatesTheSixIceStates) {
    // The kConnecting group: prime the machine at kConnected, then each
    // pre-connect ICE state migrates back to kConnecting.
    for (const np::IceState ice : {np::IceState::Disconnected,
                                   np::IceState::Gathering,
                                   np::IceState::Connecting}) {
        CallStateMachine cs;
        ASSERT_TRUE(cs.observe(np::IceState::Connected));  // first obs
        ASSERT_TRUE(cs.observe(np::IceState::Connected));  // -> kConnected
        EXPECT_TRUE(cs.observe(ice));
        EXPECT_EQ(cs.state(), CallState::kConnecting);
    }

    // Connected / Completed -> kConnected.
    CallStateMachine up;
    ASSERT_TRUE(up.observe(np::IceState::Disconnected));  // first obs
    EXPECT_TRUE(up.observe(np::IceState::Connected));
    EXPECT_EQ(up.state(), CallState::kConnected);

    CallStateMachine completed;
    ASSERT_TRUE(completed.observe(np::IceState::Disconnected));
    EXPECT_TRUE(completed.observe(np::IceState::Completed));
    EXPECT_EQ(completed.state(), CallState::kConnected);

    // Failed -> kFailed.
    CallStateMachine down;
    ASSERT_TRUE(down.observe(np::IceState::Disconnected));
    EXPECT_TRUE(down.observe(np::IceState::Failed));
    EXPECT_EQ(down.state(), CallState::kFailed);
}

// ===========================================================================
// The machine — the no-repeat judge (spec §二 / acceptance 12)
// ===========================================================================

TEST(EngineCallState, SameStateNotRepeated) {
    CallStateMachine cs;
    ASSERT_TRUE(cs.observe(np::IceState::Disconnected));  // first obs
    EXPECT_FALSE(cs.observe(np::IceState::Disconnected))
        << "the same ICE state again must not report again";

    // A second Connected observation after kConnected is also a repeat.
    CallStateMachine cs2;
    ASSERT_TRUE(cs2.observe(np::IceState::Disconnected));
    ASSERT_TRUE(cs2.observe(np::IceState::Connected));
    EXPECT_FALSE(cs2.observe(np::IceState::Connected));
}

// ===========================================================================
// The machine — the reconnecting round-trip (spec §二 / acceptance 9)
// ===========================================================================

TEST(EngineCallState, ReconnectingRoundTrip) {
    CallStateMachine cs;
    ASSERT_TRUE(cs.observe(np::IceState::Disconnected));  // kConnecting
    ASSERT_TRUE(cs.observe(np::IceState::Connected));     // kConnected

    // N3 enters a recovery episode.
    EXPECT_TRUE(cs.enter_reconnecting());
    EXPECT_EQ(cs.state(), CallState::kReconnecting);
    EXPECT_FALSE(cs.enter_reconnecting())
        << "a second enter while already reconnecting reports nothing";

    // ICE up again ends the episode: reconnecting -> connected.
    EXPECT_TRUE(cs.observe(np::IceState::Connected));
    EXPECT_EQ(cs.state(), CallState::kConnected);

    // And another drop with another N3 episode migrates again.
    EXPECT_TRUE(cs.enter_reconnecting());
    EXPECT_EQ(cs.state(), CallState::kReconnecting);
}

// ===========================================================================
// The machine — fail() carries the failure and the reason
// ===========================================================================

TEST(EngineCallState, FailCarriesFailureAndReason) {
    CallStateMachine cs;
    ASSERT_TRUE(cs.observe(np::IceState::Disconnected));

    EXPECT_TRUE(cs.fail(CallFailure::kDtlsFailed, "dtls-handshake-failed"));
    EXPECT_EQ(cs.state(), CallState::kFailed);
    EXPECT_EQ(cs.failure(), CallFailure::kDtlsFailed);
    EXPECT_EQ(cs.reason(), "dtls-handshake-failed");

    // A second fail() reports nothing and leaves the first intact.
    EXPECT_FALSE(cs.fail(CallFailure::kMediaFailed, "audio-decode-failed"));
    EXPECT_EQ(cs.failure(), CallFailure::kDtlsFailed);
    EXPECT_EQ(cs.reason(), "dtls-handshake-failed");
}

// ===========================================================================
// The machine — the end states are absorbing (ADR-035 §2)
// ===========================================================================

TEST(EngineCallState, FailedIsAbsorbing) {
    CallStateMachine cs;
    ASSERT_TRUE(cs.observe(np::IceState::Disconnected));
    ASSERT_TRUE(cs.observe(np::IceState::Connected));
    ASSERT_EQ(cs.state(), CallState::kConnected);

    ASSERT_TRUE(cs.fail(CallFailure::kIceFailed, "checks-failed"));
    // ICE staying up must not resurrect the call: without the absorber the
    // report would oscillate kConnected <-> kFailed on every tick.
    EXPECT_FALSE(cs.observe(np::IceState::Connected));
    EXPECT_EQ(cs.state(), CallState::kFailed);
    EXPECT_FALSE(cs.enter_reconnecting());
    EXPECT_FALSE(cs.fail(CallFailure::kDtlsFailed, "x"));

    // close() is the only migration out of kFailed.
    EXPECT_TRUE(cs.close());
    EXPECT_EQ(cs.state(), CallState::kClosed);
}

TEST(EngineCallState, CloseIsTerminalAndIdempotent) {
    CallStateMachine cs;
    EXPECT_TRUE(cs.close());
    EXPECT_EQ(cs.state(), CallState::kClosed);
    EXPECT_FALSE(cs.close());
    EXPECT_FALSE(cs.observe(np::IceState::Connected));
    EXPECT_FALSE(cs.enter_reconnecting());
    EXPECT_FALSE(cs.fail(CallFailure::kIceFailed, "x"));
}

// ===========================================================================
// The machine — reset (spec §二 / acceptance: every session starts from zero)
// ===========================================================================

TEST(EngineCallState, ResetClearsEverything) {
    CallStateMachine cs;
    ASSERT_TRUE(cs.observe(np::IceState::Disconnected));
    ASSERT_TRUE(cs.observe(np::IceState::Connected));
    ASSERT_TRUE(cs.enter_reconnecting());
    ASSERT_TRUE(cs.fail(CallFailure::kMediaFailed, "video-encode-failed"));
    ASSERT_EQ(cs.state(), CallState::kFailed);

    cs.reset();
    EXPECT_EQ(cs.state(), CallState::kConnecting);
    EXPECT_EQ(cs.failure(), CallFailure::kNone);
    EXPECT_TRUE(cs.reason().empty());

    // A fresh lifecycle reports kConnecting again (open() calls reset()).
    EXPECT_TRUE(cs.observe(np::IceState::Connected));
    EXPECT_EQ(cs.state(), CallState::kConnecting);
}

// ===========================================================================
// A stateful fake ICE transport + a stateful fake DTLS session (the
// audio_alloc pattern), so the live engine tests drive a real NimRTCEngine.
// ===========================================================================

class StatefulFakeIce final : public np::IICETransport {
public:
    const char* name() const noexcept override { return "test-call-state-ice"; }
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

    // Real counters, so the engine's auto-restarts are observable through
    // the engine's own `ice_restart_count()` too.
    np::Status restart() noexcept override {
        ++restarts_;
        return np::kOk;
    }
    std::uint32_t restart_count() const noexcept override { return restarts_; }

    void set_on_ice_restart(np::OnIceRestart cb) noexcept override {
        on_ice_restart_ = std::move(cb);
    }

    // The test's knob: flip the ICE state to simulate the network.
    void set_state(np::IceState s) noexcept { state_ = s; }

private:
    np::RecvCallback on_recv_;
    np::ErrorCallback on_error_;
    np::OnIceRestart on_ice_restart_;
    np::IceState state_ = np::IceState::Disconnected;
    std::uint32_t restarts_ = 0;
};

class StatefulFakeIceFactory final : public np::IICETransportFactory {
public:
    std::string_view id() const noexcept override { return "test-call-state-ice"; }
    std::string_view display_name() const noexcept override {
        return "Stateful fake ICE (call-state test)";
    }
    np::IICETransport* create_ice() const override {
        auto* ice = new StatefulFakeIce();
        last_ = ice;
        return ice;
    }
    StatefulFakeIce* last() const noexcept { return last_; }

private:
    mutable StatefulFakeIce* last_ = nullptr;
};

class StatefulFakeDtlsSession final : public nd::IDtlsSession {
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
    nd::DtlsState state() const noexcept override { return state_; }
    bool is_connected() const noexcept override {
        return state_ == nd::DtlsState::Connected;
    }
    const nd::Fingerprint& local_fingerprint() const noexcept override {
        return fingerprint_;
    }
    std::optional<nd::SrtpKeyingMaterial>
    srtp_keying_material() const noexcept override {
        return std::nullopt;
    }

    // The test's knob: a definitive handshake failure can be staged.
    void set_state(nd::DtlsState s) noexcept { state_ = s; }

private:
    nd::DtlsState state_ = nd::DtlsState::Connected;
    nd::Fingerprint fingerprint_{"sha-256", {}, {}, ""};
};

class StatefulFakeDtlsFactory final : public nd::IDtlsSessionFactory {
public:
    std::string_view id() const noexcept override { return "test-call-state-dtls"; }
    std::string_view display_name() const noexcept override {
        return "Stateful fake DTLS (call-state test)";
    }
    std::unique_ptr<nd::IDtlsSession> create(const nd::DtlsConfig&) const override {
        auto* raw = new StatefulFakeDtlsSession();
        last_ = raw;
        return std::unique_ptr<nd::IDtlsSession>(raw);
    }
    StatefulFakeDtlsSession* last() const noexcept { return last_; }

private:
    mutable StatefulFakeDtlsSession* last_ = nullptr;
};

// ===========================================================================
// The live-engine fixture
// ===========================================================================

class EngineCallStateLive : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
        static StatefulFakeIceFactory ice_factory;
        static StatefulFakeDtlsFactory dtls_factory;
        auto& reg = PluginRegistry::instance();
        reg.register_ice_transport("test-call-state-ice", &ice_factory);
        reg.register_dtls_session("test-call-state-dtls", &dtls_factory);
        kIceFactory = &ice_factory;
        kDtlsFactory = &dtls_factory;
    }

    static EngineConfig make_config() {
        EngineConfig cfg{};
        cfg.transport_name       = "test-call-state-ice";
        cfg.dtls_name            = "test-call-state-dtls";
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

    static StatefulFakeIceFactory* kIceFactory;
    static StatefulFakeDtlsFactory* kDtlsFactory;
};

StatefulFakeIceFactory* EngineCallStateLive::kIceFactory = nullptr;
StatefulFakeDtlsFactory* EngineCallStateLive::kDtlsFactory = nullptr;

// ===========================================================================
// Acceptance 7 — open() fires exactly one kConnecting report.
// ===========================================================================

TEST_F(EngineCallStateLive, OpenReportsConnecting) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);

    std::vector<NimRTCEngine::CallStateReport> reports;
    engine.set_on_call_state(
        [&reports](const NimRTCEngine::CallStateReport& r) {
            reports.push_back(r);
        });

    ASSERT_EQ(engine.open(), 0u);
    ASSERT_FALSE(reports.empty());
    EXPECT_EQ(reports.front().state, CallState::kConnecting);
    engine.close();
}

// ===========================================================================
// Acceptance 8 / 12 — ICE up fires one kConnected report; the same state is
// never repeated.
// ===========================================================================

TEST_F(EngineCallStateLive, IceUpReportsConnectedThenStaysQuiet) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    auto* ice = static_cast<StatefulFakeIce*>(kIceFactory->last());
    ASSERT_NE(ice, nullptr);

    std::vector<NimRTCEngine::CallStateReport> reports;
    engine.set_on_call_state(
        [&reports](const NimRTCEngine::CallStateReport& r) {
            reports.push_back(r);
        });

    ice->set_state(np::IceState::Connected);
    engine.tick();
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].state, CallState::kConnected);

    // The same ICE state again — no report (acceptance 12).
    engine.tick();
    EXPECT_EQ(reports.size(), 1u);
    engine.close();
}

// ===========================================================================
// Acceptance 9 / 13 / 14 — an N3 restart fires one kReconnecting report
// carrying the N3 details and the N8 path-change context.
// ===========================================================================

TEST_F(EngineCallStateLive, N3RestartReportsReconnectingWithDetails) {
    EngineConfig cfg = make_config();
    cfg.ice_recovery_policy = "auto";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    auto* ice = static_cast<StatefulFakeIce*>(kIceFactory->last());
    ASSERT_NE(ice, nullptr);

    std::vector<NimRTCEngine::CallStateReport> reports;
    engine.set_on_call_state(
        [&reports](const NimRTCEngine::CallStateReport& r) {
            reports.push_back(r);
        });

    // The session establishes once, then the network drops.
    ice->set_state(np::IceState::Connected);
    engine.tick();
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].state, CallState::kConnected);

    ice->set_state(np::IceState::Disconnected);
    engine.tick();

    // Wait for the grace (5 s) + first backoff (1 s): the engine restarts
    // ICE itself and enters the reconnecting episode.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto reconnecting = [&reports]() {
        return std::count_if(
            reports.begin(), reports.end(),
            [](const auto& r) { return r.state == CallState::kReconnecting; });
    };
    while (reconnecting() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        engine.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Exactly one reconnecting report for the episode.
    ASSERT_EQ(reconnecting(), 1u);
    const auto it = std::find_if(
        reports.begin(), reports.end(),
        [](const auto& r) { return r.state == CallState::kReconnecting; });
    ASSERT_NE(it, reports.end());

    // The N3 details (acceptance 13): the restart just booked its slot.
    EXPECT_EQ(it->restart_count, 1u);
    EXPECT_EQ(it->remaining_budget, 4u);
    EXPECT_FALSE(it->terminal);

    // The N8 path-change context (acceptance 14): the restart recorded a
    // path change (old pair = none selected yet, new pair = still none).
    EXPECT_EQ(it->path_change_reason, "local-address-changed");
    EXPECT_FALSE(it->old_pair.has_value());
    EXPECT_FALSE(it->new_pair.has_value());
    engine.close();
}

// ===========================================================================
// Acceptance 10 / 15 — the N3 terminal fires one kFailed report with
// `failure == kIceFailed`, the N4 reason and the spent budget.
// ===========================================================================

TEST_F(EngineCallStateLive, N3TerminalReportsFailedWithIceFailed) {
    EngineConfig cfg = make_config();
    cfg.ice_recovery_policy = "auto";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    auto* ice = static_cast<StatefulFakeIce*>(kIceFactory->last());
    ASSERT_NE(ice, nullptr);

    std::vector<NimRTCEngine::CallStateReport> reports;
    std::vector<std::pair<std::uint32_t, std::string>> errors;
    engine.set_on_call_state(
        [&reports](const NimRTCEngine::CallStateReport& r) {
            reports.push_back(r);
        });
    engine.set_on_error(
        [&errors](std::uint32_t code, std::string_view msg) {
            errors.emplace_back(code, std::string(msg));
        });

    // The session establishes once, then the network goes *Failed* and stays
    // down: the N3 machine skips the grace window and backs off (1 / 2 / 4 /
    // 8 / 8 s), restarting until the budget is spent — roughly 23 s of wall
    // time, bounded by a generous deadline below.
    ice->set_state(np::IceState::Connected);
    engine.tick();
    ice->set_state(np::IceState::Failed);
    engine.tick();

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(60);
    const auto failed = [&reports]() {
        return std::count_if(
            reports.begin(), reports.end(),
            [](const auto& r) { return r.state == CallState::kFailed; });
    };
    while (failed() == 0 && std::chrono::steady_clock::now() < deadline) {
        engine.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // The recovery episode produced exactly one reconnecting report, and the
    // terminal failure follows it (the ordering is the N3 machine's).
    ASSERT_EQ(failed(), 1u);
    const std::size_t reconnecting = std::count_if(
        reports.begin(), reports.end(),
        [](const auto& r) { return r.state == CallState::kReconnecting; });
    EXPECT_EQ(reconnecting, 1u);
    const auto failed_it = std::find_if(
        reports.begin(), reports.end(),
        [](const auto& r) { return r.state == CallState::kFailed; });
    const auto reconnecting_it = std::find_if(
        reports.begin(), reports.end(),
        [](const auto& r) { return r.state == CallState::kReconnecting; });
    ASSERT_NE(failed_it, reports.end());
    ASSERT_NE(reconnecting_it, reports.end());
    EXPECT_LT(std::distance(reports.begin(), reconnecting_it),
              std::distance(reports.begin(), failed_it));

    // Acceptance 10: the terminal failure is an ICE failure.
    EXPECT_EQ(failed_it->failure, CallFailure::kIceFailed);
    // Acceptance 15: the N4 reason rides along verbatim — for a transport
    // that reports Failed with no candidates and no pair, the projector's
    // decision is "checks-failed".
    EXPECT_EQ(failed_it->failure_reason, "checks-failed");
    // The spent budget (acceptance 13): 5 restarts used, 0 left, terminal.
    EXPECT_EQ(failed_it->restart_count, 5u);
    EXPECT_EQ(failed_it->remaining_budget, 0u);
    EXPECT_TRUE(failed_it->terminal);

    // The engine itself agrees on all three report points (the recovery
    // machine's own surface).
    EXPECT_EQ(ice->restart_count(), 5u);
    EXPECT_EQ(engine.ice_restart_count(), 5u);
    const auto st = engine.stats();
    EXPECT_EQ(st.ice_recovery_state, "terminal");
    EXPECT_EQ(st.ice_recovery_restarts, 5u);
    const std::size_t exhausted = std::count_if(
        errors.begin(), errors.end(),
        [](const auto& e) {
            return e.first == nimrtc::core::kIceRecoveryExhausted;
        });
    EXPECT_EQ(exhausted, 1u);
    engine.close();
}

// ===========================================================================
// Acceptance 20 — a definitive DTLS failure fires kFailed with
// `failure == kDtlsFailed`, and kFailed is absorbing (no kConnected
// resurrection while ICE stays up).
// ===========================================================================

TEST_F(EngineCallStateLive, DtlsFailureReportsFailedAndStaysFailed) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    auto* ice = static_cast<StatefulFakeIce*>(kIceFactory->last());
    ASSERT_NE(ice, nullptr);
    auto* dtls = kDtlsFactory->last();
    ASSERT_NE(dtls, nullptr);

    std::vector<NimRTCEngine::CallStateReport> reports;
    engine.set_on_call_state(
        [&reports](const NimRTCEngine::CallStateReport& r) {
            reports.push_back(r);
        });

    ice->set_state(np::IceState::Connected);
    engine.tick();
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].state, CallState::kConnected);

    // A definitive handshake failure ends the call.
    dtls->set_state(nd::DtlsState::Failed);
    engine.tick();
    ASSERT_EQ(reports.size(), 2u);
    EXPECT_EQ(reports[1].state, CallState::kFailed);
    EXPECT_EQ(reports[1].failure, CallFailure::kDtlsFailed);
    EXPECT_EQ(reports[1].failure_reason, "dtls-handshake-failed");

    // kFailed is absorbing: ICE staying up must not resurrect the report.
    engine.tick();
    EXPECT_EQ(reports.size(), 2u);

    // close() is the only way out — the host hangs up a dead call.
    engine.close();
    ASSERT_EQ(reports.size(), 3u);
    EXPECT_EQ(reports[2].state, CallState::kClosed);
}

// ===========================================================================
// Acceptance 22 / 23 / 25 — the lifecycle methods: start() is
// "configure -> open", stop() / hangup() close in the defined order and
// fire kClosed.
// ===========================================================================

TEST_F(EngineCallStateLive, StartReportsConnectingAndStopReportsClosed) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);

    std::vector<NimRTCEngine::CallStateReport> reports;
    engine.set_on_call_state(
        [&reports](const NimRTCEngine::CallStateReport& r) {
            reports.push_back(r);
        });

    EXPECT_EQ(engine.start(cfg), 0);
    EXPECT_TRUE(engine.is_open());
    ASSERT_GE(reports.size(), 1u);
    EXPECT_EQ(reports.front().state, CallState::kConnecting);

    engine.stop();
    ASSERT_GE(reports.size(), 2u);
    EXPECT_EQ(reports.back().state, CallState::kClosed);
}

TEST_F(EngineCallStateLive, HangupReportsClosedAndIsIdempotent) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);

    std::vector<NimRTCEngine::CallStateReport> reports;
    engine.set_on_call_state(
        [&reports](const NimRTCEngine::CallStateReport& r) {
            reports.push_back(r);
        });

    ASSERT_EQ(engine.open(), 0u);
    EXPECT_TRUE(engine.hangup("user-bye"));
    EXPECT_EQ(reports.back().state, CallState::kClosed);

    // hangup() on an already-closed engine is a no-op.
    EXPECT_FALSE(engine.hangup("again"));
}

// ===========================================================================
// Acceptance 26 — the callback fires on the engine's (tick) thread.
// ===========================================================================

TEST_F(EngineCallStateLive, CallbackFiresOnTheTickThread) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    auto* ice = static_cast<StatefulFakeIce*>(kIceFactory->last());
    ASSERT_NE(ice, nullptr);

    const auto test_thread = std::this_thread::get_id();
    std::thread::id cb_thread;
    std::vector<NimRTCEngine::CallStateReport> reports;
    engine.set_on_call_state([&](const NimRTCEngine::CallStateReport& r) {
        cb_thread = std::this_thread::get_id();
        reports.push_back(r);
    });

    ice->set_state(np::IceState::Connected);
    engine.tick();
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(cb_thread, test_thread)
        << "the report fires on the engine's (tick) thread, not a host "
           "thread, and not deferred to a worker";
    engine.close();
}

}  // namespace
