/**
 * @file src/engine/tests/test_engine_ice_recovery.cpp
 * @brief v1-ice-recovery (N3, ADR-031) — the engine-driven reconnection policy.
 *
 * ## What this file proves
 *
 * The slice has one engine-private collaborator — `detail::IceRecovery`, the
 * reconnection state machine — and one host-facing switch,
 * `detail::resolve_ice_recovery_policy`.  The first half of this file pins
 * both with a virtual clock (every machine entry point takes `now_us`
 * explicitly, so no test sleeps; the same clock discipline as
 * `test_engine_device_policy.cpp` / D4).
 *
 * The second half drives a real `NimRTCEngine` with a *stateful* fake ICE
 * transport (its `state()` is a knob the test turns, and its `restart()` /
 * `restart_count()` are real counters) to prove the wiring end-to-end:
 *
 *   - a drop after a successful connection, grace (5 s) and first backoff
 *     (1 s) → the engine restarts ICE itself and reports it;
 *   - a session that never connected does not trigger a restart storm;
 *   - `restart_ice()` shares the same budget and the terminal state is
 *     reported (stats + `on_error` + the recovery callback);
 *   - `reset_ice_recovery_budget()` clears the terminal state;
 *   - `ice_recovery_policy` "off" / unknown → the surface says "disabled"
 *     and nothing is driven;
 *   - the peer-restart callback path (`set_on_ice_restart`) is unchanged.
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

#include "engine_ice_recovery.hpp"

namespace {

namespace np = nimrtc::plugins;
namespace nd = nimrtc::dtls;

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
using IceRecovery = nimrtc::engine::detail::IceRecovery;
using IceRecoveryPolicy = nimrtc::engine::detail::IceRecoveryPolicy;

constexpr std::int64_t kUsPerSec = 1'000'000;

// ===========================================================================
// The policy switch (spec §六 1–4 / ADR-031 §2.2)
// ===========================================================================

TEST(EngineIceRecovery, PolicyResolution) {
    EXPECT_EQ(nimrtc::engine::detail::resolve_ice_recovery_policy("auto"),
              IceRecoveryPolicy::kAuto);
    EXPECT_EQ(nimrtc::engine::detail::resolve_ice_recovery_policy("off"),
              IceRecoveryPolicy::kOff);
    EXPECT_EQ(nimrtc::engine::detail::resolve_ice_recovery_policy(""),
              IceRecoveryPolicy::kOff);
    // A mistyped key must disarm the engine, never arm it — the opposite
    // fallback to `connectivity_profile`'s "unknown → auto" (the engine's
    // failure mode is "restarts too often", not "fails to connect").
    EXPECT_EQ(nimrtc::engine::detail::resolve_ice_recovery_policy("bogus"),
              IceRecoveryPolicy::kOff);
}

TEST(EngineIceRecovery, ConfigDefaultsToAuto) {
    EngineConfig cfg;
    EXPECT_EQ(cfg.ice_recovery_policy, "auto");
    cfg.ice_recovery_policy = "off";
    EXPECT_EQ(cfg.ice_recovery_policy, "off");
}

// ===========================================================================
// The machine — grace window (spec §方案 1 / ADR-031 §2.4)
// ===========================================================================

TEST(EngineIceRecovery, DisconnectedEntersGraceWithoutImmediateRestart) {
    IceRecovery rec;
    EXPECT_EQ(rec.state(), IceRecovery::State::kIdle);

    rec.note_disconnected(0);
    EXPECT_EQ(rec.state(), IceRecovery::State::kGrace);
    EXPECT_FALSE(rec.due(0)) << "the grace window, not an immediate restart";
    EXPECT_FALSE(rec.grace_expired(0));
    EXPECT_FALSE(rec.grace_expired(4 * kUsPerSec));
    EXPECT_TRUE(rec.grace_expired(5 * kUsPerSec));
    EXPECT_FALSE(rec.terminal());
    EXPECT_EQ(rec.restarts_used(0), 0u);
    EXPECT_EQ(rec.remaining_budget(0), 5u);
    EXPECT_STREQ(std::string(rec.pending_reason()).c_str(),
                 "local-address-changed");
}

TEST(EngineIceRecovery, RepeatDisconnectedInsideGraceKeepsTheWindow) {
    // A second `Disconnected` observation inside the grace window must not
    // extend it: the countdown started at the first drop.
    IceRecovery rec;
    rec.note_disconnected(0);
    rec.note_disconnected(1 * kUsPerSec);
    EXPECT_TRUE(rec.grace_expired(5 * kUsPerSec));
    EXPECT_FALSE(rec.grace_expired(4 * kUsPerSec));
}

TEST(EngineIceRecovery, GraceWindowRecoveryClearsCount) {
    // Semantics #1: ICE came back by itself inside the grace window — no
    // restart was executed, so the budget is untouched and the count clears,
    // even if the window already held restarts from earlier.
    IceRecovery rec;
    ASSERT_TRUE(rec.note_restart_attempted(0, "budget-exhausted"));
    ASSERT_TRUE(rec.note_restart_attempted(10 * kUsPerSec, "budget-exhausted"));
    EXPECT_EQ(rec.restarts_used(20 * kUsPerSec), 2u);

    rec.note_disconnected(20 * kUsPerSec);
    EXPECT_EQ(rec.state(), IceRecovery::State::kGrace);
    rec.note_up(21 * kUsPerSec);
    EXPECT_EQ(rec.state(), IceRecovery::State::kIdle);
    EXPECT_EQ(rec.restarts_used(21 * kUsPerSec), 0u);
    EXPECT_EQ(rec.remaining_budget(21 * kUsPerSec), 5u);
    EXPECT_TRUE(rec.pending_reason().empty());
}

TEST(EngineIceRecovery, GraceExpiryEntersBackoffWithOneSecond) {
    IceRecovery rec;
    rec.note_disconnected(0);
    ASSERT_EQ(rec.state(), IceRecovery::State::kGrace);

    rec.note_grace_expired(5 * kUsPerSec);
    EXPECT_EQ(rec.state(), IceRecovery::State::kBackoff);
    EXPECT_EQ(rec.current_backoff_us(), 1 * kUsPerSec);
    EXPECT_FALSE(rec.due(5 * kUsPerSec));
    EXPECT_FALSE(rec.due(6 * kUsPerSec - 1));
    EXPECT_TRUE(rec.due(6 * kUsPerSec));
    EXPECT_STREQ(std::string(rec.pending_reason()).c_str(),
                 "local-address-changed");
}

TEST(EngineIceRecovery, FailedSkipsGraceWindow) {
    // Explicit failure is not a blip: kIdle jumps straight to kBackoff.
    IceRecovery rec;
    rec.note_failed(0);
    EXPECT_EQ(rec.state(), IceRecovery::State::kBackoff);
    EXPECT_EQ(rec.current_backoff_us(), 1 * kUsPerSec);
    EXPECT_TRUE(rec.due(1 * kUsPerSec));
    EXPECT_STREQ(std::string(rec.pending_reason()).c_str(), "ice-failed");

    // From inside the grace window too: a `Failed` observation abandons the
    // remaining grace and backs off immediately.
    IceRecovery rec2;
    rec2.note_disconnected(0);
    ASSERT_EQ(rec2.state(), IceRecovery::State::kGrace);
    rec2.note_failed(2 * kUsPerSec);
    EXPECT_EQ(rec2.state(), IceRecovery::State::kBackoff);
    EXPECT_STREQ(std::string(rec2.pending_reason()).c_str(), "ice-failed");
}

// ===========================================================================
// The machine — backoff schedule (spec §方案 2 / ADR-031 §2.5)
// ===========================================================================

TEST(EngineIceRecovery, BackoffSequenceDoublesThenCapsAtEightSeconds) {
    IceRecovery rec;
    rec.note_disconnected(0);
    rec.note_grace_expired(5 * kUsPerSec);
    // The wait before the first restart is the initial 1 s (from grace
    // expiry); each attempt doubles the backoff for the next one, capped
    // at 8 s.
    std::int64_t t = 6 * kUsPerSec;  // first attempt is due here
    EXPECT_EQ(rec.current_backoff_us(), 1 * kUsPerSec);

    ASSERT_TRUE(rec.note_restart_attempted(t, "budget-exhausted"));
    EXPECT_EQ(rec.current_backoff_us(), 2 * kUsPerSec);
    t += 2 * kUsPerSec;

    ASSERT_TRUE(rec.note_restart_attempted(t, "budget-exhausted"));
    EXPECT_EQ(rec.current_backoff_us(), 4 * kUsPerSec);
    t += 4 * kUsPerSec;

    ASSERT_TRUE(rec.note_restart_attempted(t, "budget-exhausted"));
    EXPECT_EQ(rec.current_backoff_us(), 8 * kUsPerSec);
    t += 8 * kUsPerSec;

    ASSERT_TRUE(rec.note_restart_attempted(t, "budget-exhausted"));
    EXPECT_EQ(rec.current_backoff_us(), 8 * kUsPerSec) << "capped at 8 s";
    t += 8 * kUsPerSec;

    ASSERT_TRUE(rec.note_restart_attempted(t, "budget-exhausted"));
    EXPECT_EQ(rec.current_backoff_us(), 8 * kUsPerSec);
}

TEST(EngineIceRecovery, FailedRestartConsumesBudgetAndDoublesBackoff) {
    // Semantics #3: a failed restart still books the budget slot and makes
    // the next attempt wait twice as long.
    IceRecovery rec;
    rec.note_disconnected(0);
    rec.note_grace_expired(5 * kUsPerSec);
    ASSERT_TRUE(rec.due(6 * kUsPerSec));

    ASSERT_TRUE(rec.note_restart_attempted(6 * kUsPerSec, "budget-exhausted"));
    rec.note_restart_failed(6 * kUsPerSec);
    EXPECT_EQ(rec.restarts_used(6 * kUsPerSec), 1u);
    EXPECT_EQ(rec.remaining_budget(6 * kUsPerSec), 4u);
    EXPECT_EQ(rec.current_backoff_us(), 2 * kUsPerSec);
    EXPECT_FALSE(rec.due(8 * kUsPerSec - 1));
    EXPECT_TRUE(rec.due(8 * kUsPerSec));
}

// ===========================================================================
// The machine — the sliding budget (spec §方案 2 / ADR-031 §2.5)
// ===========================================================================

TEST(EngineIceRecovery, ConsecutiveRestartsConsumeBudgetThenTerminal) {
    IceRecovery rec;
    std::int64_t t = 0;
    for (std::uint32_t i = 1; i <= 5; ++i) {
        // A fresh outage: drop, grace lapses, the backoff comes due.
        rec.note_disconnected(t);
        rec.note_grace_expired(t + 5 * kUsPerSec);
        EXPECT_TRUE(rec.due(t + 6 * kUsPerSec));
        ASSERT_TRUE(rec.note_restart_attempted(t + 6 * kUsPerSec,
                                               "budget-exhausted"))
            << "attempt " << i << " must be inside the budget";
        EXPECT_EQ(rec.restarts_used(t + 6 * kUsPerSec), i);
        EXPECT_EQ(rec.remaining_budget(t + 6 * kUsPerSec), 5u - i);
        // The restart brought ICE back up; the count stays in the window.
        rec.note_up(t + 7 * kUsPerSec);
        EXPECT_EQ(rec.state(), IceRecovery::State::kIdle);
        t += 10 * kUsPerSec;  // next outage 10 s later, still one window
    }
    EXPECT_FALSE(rec.terminal());

    // The 6th outage: the budget is spent, the machine goes terminal and
    // the count is frozen at 5.
    rec.note_disconnected(t);
    rec.note_grace_expired(t + 5 * kUsPerSec);
    EXPECT_TRUE(rec.due(t + 6 * kUsPerSec));
    EXPECT_FALSE(rec.note_restart_attempted(t + 6 * kUsPerSec,
                                            "budget-exhausted"));
    EXPECT_TRUE(rec.terminal());
    EXPECT_EQ(rec.state(), IceRecovery::State::kTerminal);
    EXPECT_EQ(rec.restarts_used(t + 6 * kUsPerSec), 5u);
    EXPECT_EQ(rec.remaining_budget(t + 6 * kUsPerSec), 0u);
    EXPECT_STREQ(std::string(rec.terminal_reason()).c_str(), "budget-exhausted");
}

TEST(EngineIceRecovery, TerminalStateStopsFurtherRestarts) {
    IceRecovery rec;
    for (std::uint32_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(rec.note_restart_attempted(i * kUsPerSec,
                                               "budget-exhausted"));
    }
    EXPECT_FALSE(rec.note_restart_attempted(6 * kUsPerSec, "budget-exhausted"));
    EXPECT_TRUE(rec.terminal());

    // Terminal is sticky: even long after, and even with a manual reason,
    // the machine refuses — the count is frozen, never decayed.
    EXPECT_FALSE(rec.note_restart_attempted(7 * kUsPerSec, "manual-blocked"));
    EXPECT_FALSE(rec.note_restart_attempted(600 * kUsPerSec, "manual-blocked"));
    EXPECT_EQ(rec.restarts_used(7 * kUsPerSec), 5u);
    EXPECT_EQ(rec.restarts_used(600 * kUsPerSec), 5u);
}

TEST(EngineIceRecovery, SlidingWindowDropsOldRestarts) {
    IceRecovery rec;
    // A burst of four restarts (t = 0 / 10 / 20 / 30 s) shares one window.
    for (std::uint32_t i = 0; i < 4; ++i) {
        ASSERT_TRUE(rec.note_restart_attempted(i * 10 * kUsPerSec,
                                               "budget-exhausted"));
    }
    EXPECT_EQ(rec.restarts_used(30 * kUsPerSec), 4u);

    // More than one full window (600 s) after the last restart: the window
    // slid, the old restarts no longer occupy it — the attempt is allowed
    // and counts as the first of a fresh window.
    const std::int64_t later = 30 * kUsPerSec + 600 * kUsPerSec;
    EXPECT_TRUE(rec.note_restart_attempted(later, "budget-exhausted"));
    EXPECT_EQ(rec.restarts_used(later), 1u);
    EXPECT_EQ(rec.remaining_budget(later), 4u);
    EXPECT_FALSE(rec.terminal());
}

// ===========================================================================
// The machine — reset (spec §四 20 / ADR-031 §2.3)
// ===========================================================================

TEST(EngineIceRecovery, ResetBudgetClearsCountAndTerminalKeepsWindow) {
    IceRecovery rec;
    for (std::uint32_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(rec.note_restart_attempted(i * kUsPerSec,
                                               "budget-exhausted"));
    }
    ASSERT_FALSE(rec.note_restart_attempted(6 * kUsPerSec, "budget-exhausted"));
    ASSERT_TRUE(rec.terminal());

    rec.reset_budget();
    EXPECT_EQ(rec.state(), IceRecovery::State::kIdle);
    EXPECT_FALSE(rec.terminal());
    EXPECT_TRUE(rec.terminal_reason().empty());
    EXPECT_EQ(rec.restarts_used(6 * kUsPerSec), 0u);

    // The window's origin survived the reset: five more restarts (anchoring
    // it at ~14 s) still block the 6th at t = 602 s, because
    // 602 s - 14 s < the 600 s window.  A reset that cleared the origin too
    // would have allowed it (602 s - 0 ≥ 600 s → fresh window).
    for (std::uint32_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(rec.note_restart_attempted((10 + i) * kUsPerSec,
                                               "budget-exhausted"));
    }
    EXPECT_FALSE(rec.note_restart_attempted(602 * kUsPerSec, "budget-exhausted"));
    EXPECT_TRUE(rec.terminal());
    EXPECT_STREQ(std::string(rec.terminal_reason()).c_str(), "budget-exhausted");
}

TEST(EngineIceRecovery, FullResetAlsoClearsTheWindow) {
    IceRecovery rec;
    for (std::uint32_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(rec.note_restart_attempted(i * kUsPerSec,
                                               "budget-exhausted"));
    }
    ASSERT_FALSE(rec.note_restart_attempted(6 * kUsPerSec, "budget-exhausted"));

    rec.reset();
    EXPECT_EQ(rec.state(), IceRecovery::State::kIdle);
    EXPECT_EQ(rec.restarts_used(6 * kUsPerSec), 0u);
    // With the origin cleared, a fresh restart is simply the first of a new
    // window — no history at all.
    EXPECT_TRUE(rec.note_restart_attempted(7 * kUsPerSec, "budget-exhausted"));
    EXPECT_EQ(rec.restarts_used(7 * kUsPerSec), 1u);
}

// ===========================================================================
// A stateful fake ICE transport + a fake DTLS session (the audio_alloc
// pattern), so the live engine tests below drive a real NimRTCEngine.
// ===========================================================================

class StatefulFakeIce final : public np::IICETransport {
public:
    const char* name() const noexcept override { return "test-recovery-ice"; }
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

    // The seam's restart counters — real, so the engine's auto-restarts and
    // the host's manual ones are observable through the engine's own
    // `ice_restart_count()` too.
    np::Status restart() noexcept override {
        ++restarts_;
        return np::kOk;
    }
    std::uint32_t restart_count() const noexcept override { return restarts_; }

    // The peer-restart callback the engine installs during init — kept, so
    // the test can prove the peer-restart path is unchanged (the recovery
    // slice adds no new parameters here).
    void set_on_ice_restart(np::OnIceRestart cb) noexcept override {
        on_ice_restart_ = std::move(cb);
    }
    void fire_peer_restart() noexcept {
        if (on_ice_restart_) on_ice_restart_();
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
    std::string_view id() const noexcept override { return "test-recovery-ice"; }
    std::string_view display_name() const noexcept override {
        return "Stateful fake ICE (ice-recovery test)";
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
    std::string_view id() const noexcept override { return "test-recovery-dtls"; }
    std::string_view display_name() const noexcept override {
        return "Fake DTLS session (ice-recovery test)";
    }
    std::unique_ptr<nd::IDtlsSession> create(const nd::DtlsConfig&) const override {
        return std::make_unique<FakeDtlsSession>();
    }
};

// ===========================================================================
// The live-engine fixture
// ===========================================================================

class EngineIceRecoveryLive : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
        static StatefulFakeIceFactory ice_factory;
        static FakeDtlsFactory dtls_factory;
        auto& reg = PluginRegistry::instance();
        reg.register_ice_transport("test-recovery-ice", &ice_factory);
        reg.register_dtls_session("test-recovery-dtls", &dtls_factory);
        kIceFactory = &ice_factory;
    }

    static EngineConfig make_config() {
        EngineConfig cfg{};
        cfg.transport_name       = "test-recovery-ice";
        cfg.dtls_name            = "test-recovery-dtls";
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
};

StatefulFakeIceFactory* EngineIceRecoveryLive::kIceFactory = nullptr;

// ===========================================================================
// Criteria 13 / 16 — a drop + grace expiry drives an automatic restart,
// reported through the recovery callback with the right reason.
// ===========================================================================

TEST_F(EngineIceRecoveryLive, AutoRestartAfterDropAndGraceExpiry) {
    EngineConfig cfg = make_config();
    cfg.ice_recovery_policy = "auto";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    auto* ice = static_cast<StatefulFakeIce*>(kIceFactory->last());
    ASSERT_NE(ice, nullptr);

    std::vector<NimRTCEngine::IceRecoveryReport> reports;
    engine.set_on_ice_recovery(
        [&reports](const NimRTCEngine::IceRecoveryReport& r) {
            reports.push_back(r);
        });

    // The session establishes once.
    ice->set_state(np::IceState::Connected);
    engine.tick();
    EXPECT_TRUE(engine.is_ice_connected());
    EXPECT_EQ(ice->restart_count(), 0u);

    // The network drops; the recovery machine enters its grace window.
    ice->set_state(np::IceState::Disconnected);
    engine.tick();

    // Tick until the grace (5 s) and the first backoff (1 s) lapse and the
    // engine restarts ICE on its own.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (ice->restart_count() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        engine.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_GT(ice->restart_count(), 0u)
        << "the engine must restart ICE itself after the grace window";
    EXPECT_EQ(engine.ice_restart_count(), ice->restart_count());

    // The very first report carries the drop's reason and the fresh count.
    ASSERT_FALSE(reports.empty());
    EXPECT_EQ(reports.front().reason, "local-address-changed");
    EXPECT_EQ(reports.front().restart_count, 1u);
    EXPECT_EQ(reports.front().remaining_budget, 4u);
    EXPECT_FALSE(reports.front().terminal);
    EXPECT_TRUE(reports.front().terminal_reason.empty());
    engine.close();
}

// ===========================================================================
// Criterion 15 — a session that never connected does not trigger a restart
// storm: a mid-establishment drop is a failed establishment, not a network
// change, so no restart may start even after a grace window would lapse.
// ===========================================================================

TEST_F(EngineIceRecoveryLive, NeverConnectedSessionDoesNotRestart) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    auto* ice = static_cast<StatefulFakeIce*>(kIceFactory->last());
    ASSERT_NE(ice, nullptr);

    // The session is still establishing when it drops — it never reached
    // Connected once.
    ice->set_state(np::IceState::Connecting);
    engine.tick();
    ice->set_state(np::IceState::Disconnected);
    engine.tick();

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(7);
    while (std::chrono::steady_clock::now() < deadline) {
        engine.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_EQ(ice->restart_count(), 0u);
    EXPECT_EQ(engine.ice_restart_count(), 0u);
    engine.close();
}

// ===========================================================================
// Criteria 17 / 18 / 20 — manual restarts share the budget; the terminal
// state is reported three ways; reset frees the machine.
// ===========================================================================

TEST_F(EngineIceRecoveryLive, ManualRestartSharesBudgetAndResetFreesIt) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    auto* ice = static_cast<StatefulFakeIce*>(kIceFactory->last());
    ASSERT_NE(ice, nullptr);

    std::vector<NimRTCEngine::IceRecoveryReport> reports;
    std::vector<std::pair<std::uint32_t, std::string>> errors;
    engine.set_on_ice_recovery(
        [&reports](const NimRTCEngine::IceRecoveryReport& r) {
            reports.push_back(r);
        });
    engine.set_on_error(
        [&errors](std::uint32_t code, std::string_view msg) {
            errors.emplace_back(code, std::string(msg));
        });

    // Five manual restarts succeed: the budget is 5 per sliding 10-min
    // window.
    for (std::uint32_t i = 0; i < 5; ++i) {
        EXPECT_TRUE(engine.restart_ice()) << "manual restart " << i + 1;
    }
    EXPECT_EQ(ice->restart_count(), 5u);
    EXPECT_EQ(engine.ice_restart_count(), 5u);

    // The sixth is blocked by the exhausted budget: the transport is not
    // touched, the machine is terminal, and the terminal episode is
    // reported three ways (on_error + stats + the recovery callback).
    EXPECT_FALSE(engine.restart_ice());
    EXPECT_EQ(ice->restart_count(), 5u);

    const auto st = engine.stats();
    EXPECT_EQ(st.ice_recovery_state, "terminal");
    EXPECT_EQ(st.ice_recovery_restarts, 5u);
    EXPECT_EQ(st.ice_recovery_terminal_reason, "manual-blocked");

    const std::size_t exhausted =
        std::count_if(errors.begin(), errors.end(),
                      [](const auto& e) {
                          return e.first == nimrtc::core::kIceRecoveryExhausted;
                      });
    EXPECT_EQ(exhausted, 1u)
        << "on_error must fire kIceRecoveryExhausted exactly once per "
           "terminal episode";

    ASSERT_FALSE(reports.empty());
    const auto& last_report = reports.back();
    EXPECT_TRUE(last_report.terminal);
    EXPECT_EQ(last_report.reason, "manual");
    EXPECT_EQ(last_report.terminal_reason, "manual-blocked");
    EXPECT_EQ(last_report.restart_count, 5u);
    EXPECT_EQ(last_report.remaining_budget, 0u);

    // reset_ice_recovery_budget() clears the terminal state: the machine
    // restarts and reports a fresh, non-terminal episode.
    engine.reset_ice_recovery_budget();
    EXPECT_TRUE(engine.restart_ice());
    EXPECT_EQ(ice->restart_count(), 6u);

    ASSERT_FALSE(reports.empty());
    const auto& fresh = reports.back();
    EXPECT_FALSE(fresh.terminal);
    EXPECT_EQ(fresh.reason, "manual");
    EXPECT_EQ(fresh.restart_count, 1u);
    EXPECT_EQ(fresh.remaining_budget, 4u);
    engine.close();
}

// ===========================================================================
// Criteria 25 — the policy switch at the engine level: "off" and any
// unknown name resolve to "disabled" and nothing is ever driven.
// ===========================================================================

TEST_F(EngineIceRecoveryLive, PolicyOffReportsDisabledAndNeverRestarts) {
    EngineConfig cfg = make_config();
    cfg.ice_recovery_policy = "off";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    auto* ice = static_cast<StatefulFakeIce*>(kIceFactory->last());
    ASSERT_NE(ice, nullptr);

    // The surface says "disabled" — it does not pretend to track a machine
    // that is never driven.
    const auto st = engine.stats();
    EXPECT_EQ(st.ice_recovery_state, "disabled");
    EXPECT_EQ(st.ice_recovery_restarts, 0u);
    EXPECT_EQ(st.ice_recovery_terminal_reason, "");

    // A drop after a connection drives nothing at all: even after the grace
    // window would have lapsed, no restart starts.
    ice->set_state(np::IceState::Connected);
    engine.tick();
    ice->set_state(np::IceState::Disconnected);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(7);
    while (std::chrono::steady_clock::now() < deadline) {
        engine.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_EQ(ice->restart_count(), 0u);
    EXPECT_EQ(engine.ice_restart_count(), 0u);
    engine.close();
}

TEST_F(EngineIceRecoveryLive, UnknownPolicyResolvesToOff) {
    // ADR-031 §2.2: a mistyped key must disarm the engine, never arm it.
    EngineConfig cfg = make_config();
    cfg.ice_recovery_policy = "no-such-policy";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    EXPECT_EQ(engine.stats().ice_recovery_state, "disabled");
    engine.close();
}

// ===========================================================================
// Criterion 26 — the peer-restart callback path is unchanged: a restart
// the transport initiates still reaches the host through
// `set_on_ice_restart`, with no new parameters added by this slice.
// ===========================================================================

TEST_F(EngineIceRecoveryLive, PeerRestartCallbackUnchanged) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    auto* ice = static_cast<StatefulFakeIce*>(kIceFactory->last());
    ASSERT_NE(ice, nullptr);

    int peer_restarts = 0;
    engine.set_on_ice_restart([&peer_restarts]() { ++peer_restarts; });
    ice->fire_peer_restart();
    EXPECT_EQ(peer_restarts, 1);
    engine.close();
}

// ===========================================================================
// Criteria 21 / 22 / 23 — the auto path reaches the terminal state with
// "budget-exhausted", reported three ways.  The fake stays down, so the
// engine keeps backing off (1 / 2 / 4 / 8 / 8 s, after the 5 s grace) and
// restarting until the budget is spent — roughly 36 s of wall time.
// ===========================================================================

TEST_F(EngineIceRecoveryLive, AutoRestartStopsAtTerminalWithBudgetExhausted) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    auto* ice = static_cast<StatefulFakeIce*>(kIceFactory->last());
    ASSERT_NE(ice, nullptr);

    std::vector<NimRTCEngine::IceRecoveryReport> reports;
    std::vector<std::pair<std::uint32_t, std::string>> errors;
    engine.set_on_ice_recovery(
        [&reports](const NimRTCEngine::IceRecoveryReport& r) {
            reports.push_back(r);
        });
    engine.set_on_error(
        [&errors](std::uint32_t code, std::string_view msg) {
            errors.emplace_back(code, std::string(msg));
        });

    // The session establishes once, then the network drops and stays down.
    ice->set_state(np::IceState::Connected);
    engine.tick();
    ice->set_state(np::IceState::Disconnected);
    engine.tick();

    // Tick until the terminal episode is reported (on_error fires once with
    // kIceRecoveryExhausted).  Bounded by a generous wall-clock deadline.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(60);
    const auto exhausted = [&errors]() {
        return std::count_if(
            errors.begin(), errors.end(),
            [](const auto& e) {
                return e.first == nimrtc::core::kIceRecoveryExhausted;
            });
    };
    while (exhausted() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        engine.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Exactly five restarts happened (the 6th attempt was blocked).
    EXPECT_EQ(ice->restart_count(), 5u);
    EXPECT_EQ(engine.ice_restart_count(), 5u);

    // The three-place terminal report (criteria 21 / 22).
    EXPECT_EQ(exhausted(), 1u)
        << "on_error must fire kIceRecoveryExhausted exactly once";
    const auto st = engine.stats();
    EXPECT_EQ(st.ice_recovery_state, "terminal");
    EXPECT_EQ(st.ice_recovery_restarts, 5u);
    EXPECT_EQ(st.ice_recovery_terminal_reason, "budget-exhausted");

    // The recovery callback reports the terminal episode too.
    ASSERT_FALSE(reports.empty());
    const auto& last_report = reports.back();
    EXPECT_TRUE(last_report.terminal);
    EXPECT_EQ(last_report.reason, "local-address-changed");
    EXPECT_EQ(last_report.terminal_reason, "budget-exhausted");
    EXPECT_EQ(last_report.restart_count, 5u);
    EXPECT_EQ(last_report.remaining_budget, 0u);
    engine.close();
}

// ===========================================================================
// Criterion 27 — a null recovery callback clears it without crashing.
// ===========================================================================

TEST_F(EngineIceRecoveryLive, NullRecoveryCallbackClearsWithoutCrash) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // Install then clear; the report path must tolerate a null callback.
    engine.set_on_ice_recovery(
        [](const NimRTCEngine::IceRecoveryReport&) {});
    engine.set_on_ice_recovery(nullptr);
    ASSERT_TRUE(engine.restart_ice());  // fires the report path — no callback
    engine.close();
}

// ===========================================================================
// Criterion 28 — restart_ice() before the transport exists returns false
// (today's behaviour, unchanged by the budget bookkeeping).
// ===========================================================================

TEST_F(EngineIceRecoveryLive, RestartIceBeforeOpenReturnsFalse) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);  // never opened: no ICE transport
    EXPECT_FALSE(engine.restart_ice());
}

}  // namespace
