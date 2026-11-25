/**
 * @file src/engine/tests/test_engine_turn_credentials.cpp
 * @brief v1-turn-credentials (N7, ADR-032) — TURN credentials as a provider seam.
 *
 * ## What this file proves
 *
 * The slice has one engine-private collaborator — `detail::TurnCredentialsState`,
 * the pull bookkeeping — and one host-facing callback type,
 * `nimrtc::engine::TurnCredentialsProvider`.  The first half of this file pins
 * the collaborator with a virtual clock (every machine entry point takes
 * `now_us` explicitly, so no test sleeps; the same clock discipline as
 * `test_engine_ice_recovery.cpp` / N3), and the compile-time `static_assert`s
 * pin the public surface: the callback is a raw function pointer (not
 * `std::function`, spec §一 3), the six `TurnCredentials` fields exist, and
 * the `EngineConfig` fields carry exactly the seam's types.
 *
 * The second half drives a real `NimRTCEngine` with a *recording* fake ICE
 * transport (every `add_turn_server` / `add_turn_server_with_transport` call
 * is captured) to prove the wiring end-to-end:
 *
 *   - `open()` consults the provider exactly once and applies the pulled
 *     credentials (spec §二 4 / §二 6);
 *   - a null provider means "no consultation, nothing to report" (spec §四 15);
 *   - a static server list wins over the provider (spec §二 7);
 *   - a failed pull (provider false / empty host) keeps the session direct and
 *     reports "unavailable" (spec §二 5 / §四 16);
 *   - an N3 reconnection restart re-consults the provider and applies a
 *     changed credential set (spec §二 8 / §二 9), or keeps the old one when
 *     the pull fails (spec §二 10).
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <nimrtc/core/engine_errors.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/dtls/dtls_session_factory.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/ice_transport.hpp>

#include "engine_turn_credentials.hpp"

namespace {

namespace np = nimrtc::plugins;
namespace nd = nimrtc::dtls;
namespace detail = nimrtc::engine::detail;

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
using nimrtc::engine::TurnCredentials;
using TurnCredentialsState = nimrtc::engine::detail::TurnCredentialsState;

// ===========================================================================
// The public surface — compile-time pins (spec §一 / interface-change table)
// ===========================================================================

// The callback type is a raw function pointer, not `std::function`
// (spec §一 3 / ADR-015: the public header must stay ABI-stable).  A
// `std::function` would fail this `is_same` check.
static_assert(std::is_same_v<nimrtc::engine::TurnCredentialsProvider,
                             bool (*)(void*, TurnCredentials*) noexcept>);

// EngineConfig carries the seam as exactly that type, with a plain void*
// context handle.
static_assert(std::is_same_v<decltype(EngineConfig::turn_credentials),
                             nimrtc::engine::TurnCredentialsProvider>);
static_assert(std::is_same_v<decltype(EngineConfig::turn_credentials_ctx),
                             void*>);

// TurnCredentials has the six fields of the spec's interface-change table.
static_assert(std::is_same_v<decltype(TurnCredentials::host), std::string>);
static_assert(std::is_same_v<decltype(TurnCredentials::port), std::uint16_t>);
static_assert(std::is_same_v<decltype(TurnCredentials::username), std::string>);
static_assert(std::is_same_v<decltype(TurnCredentials::password), std::string>);
static_assert(std::is_same_v<decltype(TurnCredentials::transport),
                             np::TurnTransport>);
static_assert(std::is_same_v<decltype(TurnCredentials::tls_server_name),
                             std::string>);

// v1-turn-tls-trust (ADR-037): the two trust-policy fields ride on the
// credential set, so a host's "换中继就换信任锚" is one change to one set.
static_assert(std::is_same_v<decltype(TurnCredentials::turn_tls_verify_peer),
                             bool>);
static_assert(std::is_same_v<decltype(TurnCredentials::turn_tls_ca_file),
                             std::string>);

TEST(EngineTurnCredentials, PublicConfigDefaults) {
    EngineConfig cfg;
    EXPECT_EQ(cfg.turn_credentials, nullptr);
    EXPECT_EQ(cfg.turn_credentials_ctx, nullptr);

    // The struct's defaults: the TURN/UDP 3478 default port and the Udp
    // transport (spec §设计 一), and the safe TURN/TLS trust defaults
    // (v1-turn-tls-trust: verify on, no trust anchor loaded).
    TurnCredentials c;
    EXPECT_TRUE(c.host.empty());
    EXPECT_EQ(c.port, 3478);
    EXPECT_TRUE(c.username.empty());
    EXPECT_TRUE(c.password.empty());
    EXPECT_EQ(c.transport, np::TurnTransport::Udp);
    EXPECT_TRUE(c.tls_server_name.empty());
    EXPECT_TRUE(c.turn_tls_verify_peer);
    EXPECT_TRUE(c.turn_tls_ca_file.empty());
}

// ===========================================================================
// The provider trampoline — a plain function pointer + context struct, the
// shape the seam demands.
// ===========================================================================

struct ProviderCtl {
    std::uint32_t calls = 0;
    bool result = true;
    TurnCredentials creds;
};

bool provider_trampoline(void* ctx, TurnCredentials* out) noexcept {
    auto* ctl = static_cast<ProviderCtl*>(ctx);
    ++ctl->calls;
    *out = ctl->creds;
    return ctl->result;
}

// ===========================================================================
// The collaborator — pull bookkeeping (spec §测试策略: 拉取记账)
// ===========================================================================

TEST(TurnCredentialsState, NullProviderIsNotConsultedAndKeepsIdle) {
    TurnCredentialsState tc;
    TurnCredentials out;
    EXPECT_EQ(tc.state_string(), "idle");
    EXPECT_FALSE(tc.refresh(0, nullptr, nullptr, &out));
    EXPECT_EQ(tc.state(), TurnCredentialsState::State::kIdle);
    EXPECT_EQ(tc.state_string(), "idle");
    EXPECT_FALSE(tc.consecutive_failures());
    EXPECT_EQ(tc.consecutive_failures_count(), 0u);
    EXPECT_EQ(tc.last(), nullptr);
}

TEST(TurnCredentialsState, SuccessfulPullRecordsStateAndNormalizesPort) {
    ProviderCtl ctl;
    ctl.creds.host = "relay.example.com";
    ctl.creds.port = 0;  // "the default" — must normalize to 3478
    ctl.creds.username = "user";
    ctl.creds.password = "pass";
    ctl.creds.transport = np::TurnTransport::Tcp;

    TurnCredentialsState tc;
    TurnCredentials out;
    ASSERT_TRUE(tc.refresh(0, provider_trampoline, &ctl, &out));
    EXPECT_EQ(ctl.calls, 1u);
    EXPECT_EQ(out.host, "relay.example.com");
    EXPECT_EQ(out.port, 3478);
    EXPECT_EQ(out.username, "user");
    EXPECT_EQ(out.password, "pass");
    EXPECT_EQ(out.transport, np::TurnTransport::Tcp);

    EXPECT_EQ(tc.state(), TurnCredentialsState::State::kOk);
    EXPECT_EQ(tc.state_string(), "ok");
    EXPECT_FALSE(tc.consecutive_failures());
    EXPECT_EQ(tc.consecutive_failures_count(), 0u);
    ASSERT_NE(tc.last(), nullptr);
    EXPECT_EQ(tc.last()->port, 3478) << "the normalized value is recorded";
}

TEST(TurnCredentialsState, FailedPullCountsConsecutiveFailure) {
    ProviderCtl ctl;
    ctl.result = false;
    TurnCredentialsState tc;
    TurnCredentials out;
    EXPECT_FALSE(tc.refresh(0, provider_trampoline, &ctl, &out));
    EXPECT_EQ(tc.state(), TurnCredentialsState::State::kUnavailable);
    EXPECT_EQ(tc.state_string(), "unavailable");
    EXPECT_TRUE(tc.consecutive_failures());
    EXPECT_EQ(tc.consecutive_failures_count(), 1u);
    EXPECT_EQ(tc.last(), nullptr);
}

TEST(TurnCredentialsState, EmptyHostIsTreatedAsUnavailable) {
    // Spec §四 16: a provider that returns true with an empty host is
    // treated exactly like "cannot get one" — the engine must not
    // configure TURN with an empty address.
    ProviderCtl ctl;
    ctl.creds.host = "";  // default — empty on purpose
    TurnCredentialsState tc;
    TurnCredentials out;
    EXPECT_FALSE(tc.refresh(0, provider_trampoline, &ctl, &out));
    EXPECT_EQ(tc.state_string(), "unavailable");
    EXPECT_TRUE(tc.consecutive_failures());
    EXPECT_EQ(tc.consecutive_failures_count(), 1u);
}

TEST(TurnCredentialsState, SuccessClearsConsecutiveFailures) {
    ProviderCtl ctl;
    ctl.result = false;
    TurnCredentialsState tc;
    TurnCredentials out;
    ASSERT_FALSE(tc.refresh(0, provider_trampoline, &ctl, &out));
    ASSERT_FALSE(tc.refresh(0, provider_trampoline, &ctl, &out));
    EXPECT_EQ(tc.consecutive_failures_count(), 2u);

    ctl.result = true;
    ctl.creds.host = "relay.example.com";
    ASSERT_TRUE(tc.refresh(0, provider_trampoline, &ctl, &out));
    EXPECT_FALSE(tc.consecutive_failures());
    EXPECT_EQ(tc.consecutive_failures_count(), 0u);
    EXPECT_EQ(tc.state_string(), "ok");
}

TEST(TurnCredentialsState, RejectedLatchIsSticky) {
    // Spec §测试策略 calls the rejection a 闩锁 (latch): once the transport
    // reports 401/403, the fact outlives the pull that replaced the
    // credentials — a later successful pull does not clear it.
    TurnCredentialsState tc;
    EXPECT_FALSE(tc.rejected());
    tc.note_rejected();
    EXPECT_TRUE(tc.rejected());
    EXPECT_EQ(tc.state(), TurnCredentialsState::State::kRejected);
    EXPECT_EQ(tc.state_string(), "rejected");

    ProviderCtl ctl;
    ctl.creds.host = "relay.example.com";
    TurnCredentials out;
    ASSERT_TRUE(tc.refresh(0, provider_trampoline, &ctl, &out));
    EXPECT_TRUE(tc.rejected()) << "the rejection latch is sticky";
    EXPECT_EQ(tc.state_string(), "rejected");
}

TEST(TurnCredentialsState, ChangeDetectionTracksLastApplied) {
    ProviderCtl ctl;
    ctl.creds.host = "relay-a.example.com";

    TurnCredentialsState tc;
    TurnCredentials out;
    ASSERT_TRUE(tc.refresh(0, provider_trampoline, &ctl, &out));
    EXPECT_TRUE(tc.credentials_changed())
        << "nothing applied yet — the first pull must be applied";
    tc.note_applied();
    EXPECT_FALSE(tc.credentials_changed());

    // A pull with a different host is a change; the same host again is not.
    ctl.creds.host = "relay-b.example.com";
    ASSERT_TRUE(tc.refresh(0, provider_trampoline, &ctl, &out));
    EXPECT_TRUE(tc.credentials_changed());
    tc.note_applied();
    EXPECT_FALSE(tc.credentials_changed());

    // A failed pull leaves the last pull unchanged — still the applied one.
    ctl.result = false;
    ASSERT_FALSE(tc.refresh(0, provider_trampoline, &ctl, &out));
    EXPECT_FALSE(tc.credentials_changed());
}

TEST(TurnCredentialsState, ChangeDetectionIncludesTrustKnobs) {
    // v1-turn-tls-trust (spec §三 10-12): the two trust-policy fields are
    // part of "are these the same credentials".  A host that changes only
    // the trust anchor or only the verify switch changed its trust policy
    // — the engine must re-apply, or a stale anchor would keep the relay
    // rejected with no visible reason.
    ProviderCtl ctl;
    ctl.creds.host = "relay.example.com";
    ctl.creds.transport = np::TurnTransport::Tls;
    ctl.creds.turn_tls_verify_peer = true;

    TurnCredentialsState tc;
    TurnCredentials out;
    ASSERT_TRUE(tc.refresh(0, provider_trampoline, &ctl, &out));
    tc.note_applied();
    EXPECT_FALSE(tc.credentials_changed());

    // Only the ca_file changed → the credentials changed (判据 10).
    ctl.creds.turn_tls_ca_file = "anchor.pem";
    ASSERT_TRUE(tc.refresh(0, provider_trampoline, &ctl, &out));
    EXPECT_TRUE(tc.credentials_changed())
        << "a changed ca_file is a changed credential set";
    tc.note_applied();
    EXPECT_FALSE(tc.credentials_changed());

    // Only the verify switch changed → the credentials changed (判据 11).
    ctl.creds.turn_tls_verify_peer = false;
    ASSERT_TRUE(tc.refresh(0, provider_trampoline, &ctl, &out));
    EXPECT_TRUE(tc.credentials_changed())
        << "a changed verify switch is a changed credential set";
    tc.note_applied();
    EXPECT_FALSE(tc.credentials_changed());

    // All eight fields identical again → not a change (判据 12).
    ASSERT_TRUE(tc.refresh(0, provider_trampoline, &ctl, &out));
    EXPECT_FALSE(tc.credentials_changed());
}

TEST(TurnCredentialsState, TurnTlsVerificationFactStates) {
    // v1-turn-tls-trust (spec §五 16-17): the trust policy of the applied
    // credentials is a queryable fact, and the three states are mutually
    // distinguishable.
    TurnCredentialsState tc;
    EXPECT_EQ(tc.turn_tls_peer_verification(), "");

    // No TURN/TLS (Udp) → nothing to report.
    ProviderCtl ctl;
    ctl.creds.host = "relay.example.com";
    ctl.creds.transport = np::TurnTransport::Udp;
    ctl.creds.turn_tls_verify_peer = true;
    TurnCredentials out;
    ASSERT_TRUE(tc.refresh(0, provider_trampoline, &ctl, &out));
    tc.note_applied();
    EXPECT_EQ(tc.turn_tls_peer_verification(), "");

    // verify_peer == false → "unverified" (判据 16).
    ctl.creds.transport = np::TurnTransport::Tls;
    ctl.creds.turn_tls_verify_peer = false;
    ASSERT_TRUE(tc.refresh(0, provider_trampoline, &ctl, &out));
    tc.note_applied();
    EXPECT_EQ(tc.turn_tls_peer_verification(), "unverified");

    // verify_peer == true with a loadable anchor → "verified" (判据 17).
    ctl.creds.turn_tls_verify_peer = true;
    ctl.creds.turn_tls_ca_file = "anchor.pem";
    ASSERT_TRUE(tc.refresh(0, provider_trampoline, &ctl, &out));
    tc.note_applied();
    EXPECT_EQ(tc.turn_tls_peer_verification(), "verified");

    // The engine recorded the anchor as broken → "failed", distinct from
    // both "verified" and "unverified" (判据 17).
    tc.note_turn_tls_anchor_failed();
    EXPECT_EQ(tc.turn_tls_peer_verification(), "failed");

    // A later apply with a loadable anchor clears the failure (spec §三).
    ASSERT_TRUE(tc.refresh(0, provider_trampoline, &ctl, &out));
    tc.note_applied();
    EXPECT_EQ(tc.turn_tls_peer_verification(), "verified");
}

TEST(TurnTlsAnchorCheck, VerdictsAndMessages) {
    // v1-turn-tls-trust (spec §四 13-15): the engine's own anchor check
    // must tell the host *which file* is broken and *how*.
    namespace fs = std::filesystem;
    const fs::path missing =
        fs::temp_directory_path() / "nimrtc_anchor_missing.pem";
    std::error_code ec;
    fs::remove(missing, ec);
    EXPECT_EQ(detail::check_turn_tls_ca_file(missing.string()),
              detail::TurnTlsCaFileVerdict::kMissing);

    const fs::path notPem =
        fs::temp_directory_path() / "nimrtc_anchor_not_pem.txt";
    {
        std::ofstream out{notPem, std::ios::binary};
        out << "this is not a certificate\n";
    }
    EXPECT_EQ(detail::check_turn_tls_ca_file(notPem.string()),
              detail::TurnTlsCaFileVerdict::kNotPem);

    const fs::path pem =
        fs::temp_directory_path() / "nimrtc_anchor_ok.pem";
    {
        std::ofstream out{pem, std::ios::binary};
        out << "-----BEGIN CERTIFICATE-----\n"
            << "AAAA\n"
            << "-----END CERTIFICATE-----\n";
    }
    EXPECT_EQ(detail::check_turn_tls_ca_file(pem.string()),
              detail::TurnTlsCaFileVerdict::kOk);
    EXPECT_EQ(detail::check_turn_tls_ca_file(""),
              detail::TurnTlsCaFileVerdict::kOk)
        << "an empty path is nothing to check";

    // The error names the file and the problem (判据 13/14).
    const std::string missingMsg =
        detail::turn_tls_ca_file_error(missing.string(),
                                       detail::TurnTlsCaFileVerdict::kMissing);
    EXPECT_NE(missingMsg.find(missing.string()), std::string::npos);
    EXPECT_NE(missingMsg.find("does not exist"), std::string::npos);

    const std::string notPemMsg =
        detail::turn_tls_ca_file_error(notPem.string(),
                                       detail::TurnTlsCaFileVerdict::kNotPem);
    EXPECT_NE(notPemMsg.find(notPem.string()), std::string::npos);
    EXPECT_NE(notPemMsg.find("PEM trust anchor"), std::string::npos);

    fs::remove(notPem, ec);
    fs::remove(pem, ec);
}

TEST(TurnCredentialsState, ResetClearsEveryObservation) {
    ProviderCtl ctl;
    ctl.creds.host = "relay.example.com";
    TurnCredentialsState tc;
    TurnCredentials out;
    ASSERT_TRUE(tc.refresh(0, provider_trampoline, &ctl, &out));
    tc.note_rejected();

    tc.reset();
    EXPECT_EQ(tc.state(), TurnCredentialsState::State::kIdle);
    EXPECT_EQ(tc.state_string(), "idle");
    EXPECT_FALSE(tc.rejected());
    EXPECT_EQ(tc.consecutive_failures_count(), 0u);
    EXPECT_EQ(tc.last(), nullptr);
    EXPECT_FALSE(tc.credentials_changed());
}

// ===========================================================================
// A recording fake ICE transport + a fake DTLS session (the recovery-test
// pattern), so the live engine tests below drive a real NimRTCEngine and
// can see exactly what TURN servers the engine applied.
// ===========================================================================

class RecordingFakeIce final : public np::IICETransport {
public:
    struct TurnCall {
        std::string host;
        std::uint16_t port = 0;
        std::string username;
        std::string password;
        np::TurnTransport transport = np::TurnTransport::Udp;
        bool with_transport = false;
        bool verify_peer = true;  // the inherited seam knob, recorded on each call
        std::string ca_file;
    };

    const char* name() const noexcept override { return "test-turn-ice"; }
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

    void add_turn_server(std::string_view host, std::uint16_t port,
                         std::string_view username,
                         std::string_view password) noexcept override {
        TurnCall c;
        c.host = std::string(host);
        c.port = port;
        c.username = std::string(username);
        c.password = std::string(password);
        turn_calls_.push_back(std::move(c));
    }

    void add_turn_server_with_transport(std::string_view host,
                                        std::uint16_t port,
                                        std::string_view username,
                                        std::string_view password,
                                        np::TurnTransport transport) noexcept override {
        TurnCall c;
        c.host = std::string(host);
        c.port = port;
        c.username = std::string(username);
        c.password = std::string(password);
        c.transport = transport;
        c.with_transport = true;
        // v1-turn-tls-trust: the concrete class does not override the 7-arg
        // default, so the base stored the trust knobs — record them exactly
        // as the engine configured them.
        c.verify_peer = turn_tls_verify_peer_;
        c.ca_file = turn_tls_ca_file_;
        turn_calls_.push_back(std::move(c));
    }

    void set_bwe(np::IBwe*) noexcept override {}
    void set_scheduler(np::IScheduler*) noexcept override {}

    // The seam's restart counters — real, so the engine's auto-restart is
    // observable through the recovery machine.
    np::Status restart() noexcept override {
        ++restarts_;
        return np::kOk;
    }
    std::uint32_t restart_count() const noexcept override { return restarts_; }

    void set_on_ice_restart(np::OnIceRestart) noexcept override {}

    // The test's knob: flip the ICE state to simulate the network.
    void set_state(np::IceState s) noexcept { state_ = s; }

    const std::vector<TurnCall>& turn_calls() const noexcept { return turn_calls_; }

private:
    np::RecvCallback on_recv_;
    np::ErrorCallback on_error_;
    np::IceState state_ = np::IceState::Disconnected;
    std::uint32_t restarts_ = 0;
    std::vector<TurnCall> turn_calls_;
};

class RecordingFakeIceFactory final : public np::IICETransportFactory {
public:
    std::string_view id() const noexcept override { return "test-turn-ice"; }
    std::string_view display_name() const noexcept override {
        return "Recording fake ICE (turn-credentials test)";
    }
    np::IICETransport* create_ice() const override {
        auto* ice = new RecordingFakeIce();
        last_ = ice;
        return ice;
    }
    RecordingFakeIce* last() const noexcept { return last_; }

private:
    mutable RecordingFakeIce* last_ = nullptr;
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
    std::string_view id() const noexcept override { return "test-turn-dtls"; }
    std::string_view display_name() const noexcept override {
        return "Fake DTLS session (turn-credentials test)";
    }
    std::unique_ptr<nd::IDtlsSession> create(const nd::DtlsConfig&) const override {
        return std::make_unique<FakeDtlsSession>();
    }
};

// ===========================================================================
// The live-engine fixture
// ===========================================================================

class EngineTurnCredentialsLive : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
        static RecordingFakeIceFactory ice_factory;
        static FakeDtlsFactory dtls_factory;
        auto& reg = PluginRegistry::instance();
        reg.register_ice_transport("test-turn-ice", &ice_factory);
        reg.register_dtls_session("test-turn-dtls", &dtls_factory);
        kIceFactory = &ice_factory;
    }

    static EngineConfig make_config() {
        EngineConfig cfg{};
        cfg.transport_name       = "test-turn-ice";
        cfg.dtls_name            = "test-turn-dtls";
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

    static RecordingFakeIceFactory* kIceFactory;
};

RecordingFakeIceFactory* EngineTurnCredentialsLive::kIceFactory = nullptr;

// ===========================================================================
// Criteria 4 / 6 — open() consults the provider once and applies the pulled
// credentials, transport included.
// ===========================================================================

TEST_F(EngineTurnCredentialsLive, OpenConsultsProviderOnceAndAppliesCredentials) {
    ProviderCtl ctl;
    ctl.creds.host = "relay.example.com";
    ctl.creds.port = 3478;
    ctl.creds.username = "user";
    ctl.creds.password = "pass";
    ctl.creds.transport = np::TurnTransport::Tcp;

    EngineConfig cfg = make_config();
    cfg.turn_credentials = provider_trampoline;
    cfg.turn_credentials_ctx = &ctl;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    EXPECT_EQ(ctl.calls, 1u) << "open() must consult the provider exactly once";
    auto* ice = static_cast<RecordingFakeIce*>(kIceFactory->last());
    ASSERT_NE(ice, nullptr);
    const auto& calls = ice->turn_calls();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].host, "relay.example.com");
    EXPECT_EQ(calls[0].port, 3478);
    EXPECT_EQ(calls[0].username, "user");
    EXPECT_EQ(calls[0].password, "pass");
    EXPECT_EQ(calls[0].transport, np::TurnTransport::Tcp);
    EXPECT_TRUE(calls[0].with_transport);

    EXPECT_EQ(engine.stats().turn_credentials_state, "ok");
    engine.close();
}

// ===========================================================================
// Criterion 14 / 15 — no provider: no consultation, the static path is
// untouched, and the surface says "" (nothing to report).
// ===========================================================================

TEST_F(EngineTurnCredentialsLive, NoProviderReportsEmptyAndAddsNothing) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    auto* ice = static_cast<RecordingFakeIce*>(kIceFactory->last());
    EXPECT_TRUE(ice->turn_calls().empty());
    EXPECT_EQ(engine.stats().turn_credentials_state, "");
    engine.close();
}

// ===========================================================================
// Criterion 7 — a static server list wins over the provider: the provider is
// never consulted, and the surface says "idle" (configured, never pulled).
// ===========================================================================

TEST_F(EngineTurnCredentialsLive, StaticServerWinsAndProviderIsNotConsulted) {
    ProviderCtl ctl;
    ctl.creds.host = "relay.example.com";

    EngineConfig cfg = make_config();
    cfg.turn_credentials = provider_trampoline;
    cfg.turn_credentials_ctx = &ctl;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.pre_open(), 0u);
    ASSERT_EQ(engine.add_turn_server("static.example.com", 3478, "s", "p"), 0);
    ASSERT_EQ(engine.open(), 0u);

    EXPECT_EQ(ctl.calls, 0u) << "static servers win — the provider is not consulted";
    auto* ice = static_cast<RecordingFakeIce*>(kIceFactory->last());
    const auto& calls = ice->turn_calls();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].host, "static.example.com");
    EXPECT_EQ(calls[0].username, "s");
    EXPECT_EQ(engine.stats().turn_credentials_state, "idle");
    engine.close();
}

// ===========================================================================
// Criteria 5 / 16 — a failed pull (provider false / empty host) keeps the
// session running direct and reports "unavailable".
// ===========================================================================

TEST_F(EngineTurnCredentialsLive, ProviderFalseKeepsSessionDirectAndUnavailable) {
    ProviderCtl ctl;
    ctl.result = false;

    EngineConfig cfg = make_config();
    cfg.turn_credentials = provider_trampoline;
    cfg.turn_credentials_ctx = &ctl;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u) << "a failed pull must not fail open()";
    EXPECT_EQ(ctl.calls, 1u);
    auto* ice = static_cast<RecordingFakeIce*>(kIceFactory->last());
    EXPECT_TRUE(ice->turn_calls().empty()) << "no TURN is configured";
    EXPECT_EQ(engine.stats().turn_credentials_state, "unavailable");
    engine.close();
}

TEST_F(EngineTurnCredentialsLive, ProviderEmptyHostIsTreatedAsUnavailable) {
    ProviderCtl ctl;
    ctl.creds.host = "";  // empty on purpose

    EngineConfig cfg = make_config();
    cfg.turn_credentials = provider_trampoline;
    cfg.turn_credentials_ctx = &ctl;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    auto* ice = static_cast<RecordingFakeIce*>(kIceFactory->last());
    EXPECT_TRUE(ice->turn_calls().empty());
    EXPECT_EQ(engine.stats().turn_credentials_state, "unavailable");
    engine.close();
}

// ===========================================================================
// Criteria 8 / 9 — an N3 reconnection restart re-consults the provider and
// applies the changed credential set.
// ===========================================================================

TEST_F(EngineTurnCredentialsLive, ReconnectReconsultsProviderAndAppliesChange) {
    ProviderCtl ctl;
    ctl.creds.host = "relay-a.example.com";

    EngineConfig cfg = make_config();
    cfg.turn_credentials = provider_trampoline;
    cfg.turn_credentials_ctx = &ctl;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    auto* ice = static_cast<RecordingFakeIce*>(kIceFactory->last());
    ASSERT_NE(ice, nullptr);
    EXPECT_EQ(ctl.calls, 1u);
    EXPECT_EQ(ice->turn_calls().size(), 1u);

    // The session establishes once, then the network drops.
    ice->set_state(np::IceState::Connected);
    engine.tick();
    EXPECT_TRUE(engine.is_ice_connected());
    ice->set_state(np::IceState::Disconnected);
    engine.tick();

    // Before the restart fires (grace 5 s + backoff 1 s), rotate the
    // provider's credentials: the reconnect must pull and apply the new set.
    ctl.creds.host = "relay-b.example.com";

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (ice->restart_count() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        engine.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_GT(ice->restart_count(), 0u)
        << "the engine must restart ICE itself after the grace window";

    EXPECT_EQ(ctl.calls, 2u) << "the reconnect must re-consult the provider";
    const auto& calls = ice->turn_calls();
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0].host, "relay-a.example.com");
    EXPECT_EQ(calls[1].host, "relay-b.example.com") << "the new set is applied";
    EXPECT_EQ(engine.stats().turn_credentials_state, "ok");
    engine.close();
}

// ===========================================================================
// Criterion 10 — a failed pull on reconnect keeps the old credentials (they
// may still be valid) and reports "unavailable".
// ===========================================================================

TEST_F(EngineTurnCredentialsLive, ReconnectProviderFailureKeepsOldCredentials) {
    ProviderCtl ctl;
    ctl.creds.host = "relay-a.example.com";

    EngineConfig cfg = make_config();
    cfg.turn_credentials = provider_trampoline;
    cfg.turn_credentials_ctx = &ctl;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    auto* ice = static_cast<RecordingFakeIce*>(kIceFactory->last());
    ASSERT_NE(ice, nullptr);

    ice->set_state(np::IceState::Connected);
    engine.tick();
    ice->set_state(np::IceState::Disconnected);
    engine.tick();

    // The reconnect pull will fail: no new credentials, no re-add.
    ctl.result = false;

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (ice->restart_count() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        engine.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_GT(ice->restart_count(), 0u);

    EXPECT_EQ(ctl.calls, 2u);
    EXPECT_EQ(ice->turn_calls().size(), 1u) << "no re-add — the old set is kept";
    EXPECT_EQ(engine.stats().turn_credentials_state, "unavailable");
    engine.close();
}

// ===========================================================================
// v1-turn-tls-trust — the two trust knobs reach the plugin seam exactly as
// configured, and the verification fact is queryable in all three states.
// ===========================================================================

TEST_F(EngineTurnCredentialsLive, TrustKnobsReachPluginSeamAndVerificationFact) {
    ProviderCtl ctl;
    ctl.creds.host = "relay.example.com";
    ctl.creds.port = 3478;
    ctl.creds.username = "user";
    ctl.creds.password = "pass";
    ctl.creds.transport = np::TurnTransport::Tls;
    ctl.creds.turn_tls_verify_peer = false;
    ctl.creds.turn_tls_ca_file = "relay-anchor.pem";

    EngineConfig cfg = make_config();
    cfg.turn_credentials = provider_trampoline;
    cfg.turn_credentials_ctx = &ctl;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    auto* ice = static_cast<RecordingFakeIce*>(kIceFactory->last());
    ASSERT_NE(ice, nullptr);
    const auto& calls = ice->turn_calls();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].transport, np::TurnTransport::Tls);
    EXPECT_FALSE(calls[0].verify_peer)
        << "verify_peer=false must reach the plugin seam (判据 8)";
    EXPECT_EQ(calls[0].ca_file, "relay-anchor.pem")
        << "ca_file must reach the plugin seam (判据 7)";

    // verify_peer=false ignores the ca_file (spec §设计 一) — a bogus path
    // must not surface an error, and the fact must say "unverified"
    // (判据 16).
    EXPECT_EQ(engine.turn_tls_peer_verification(), "unverified");
    engine.close();
}

TEST_F(EngineTurnCredentialsLive, BrokenCaFileSurfacesErrorAndMarksFailed) {
    ProviderCtl ctl;
    ctl.creds.host = "relay.example.com";
    ctl.creds.transport = np::TurnTransport::Tls;
    ctl.creds.turn_tls_verify_peer = true;
    // A path that cannot exist: the engine must fail loudly, not silently.
    const std::string missing =
        (std::filesystem::temp_directory_path() / "nimrtc_missing_anchor.pem")
            .string();
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path{missing}, ec);
    ctl.creds.turn_tls_ca_file = missing;

    EngineConfig cfg = make_config();
    cfg.turn_credentials = provider_trampoline;
    cfg.turn_credentials_ctx = &ctl;

    // The live fixture resolves some video plugins with an empty id, which
    // fires a spurious `on_error_` during open() (pre-existing engine
    // behaviour, unrelated to this slice).  Accumulate every callback so the
    // assertions below look for the ca-file error among them, not for a
    // clean slate.
    std::vector<std::pair<std::uint32_t, std::string>> errors;
    NimRTCEngine engine(cfg);
    engine.set_on_error([&](std::uint32_t e, std::string_view m) {
        errors.emplace_back(e, std::string(m));
    });
    ASSERT_EQ(engine.open(), 0u);

    auto* ice = static_cast<RecordingFakeIce*>(kIceFactory->last());
    const auto& calls = ice->turn_calls();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_TRUE(calls[0].verify_peer);
    EXPECT_EQ(calls[0].ca_file, missing);

    const bool surfaced = std::any_of(
        errors.begin(), errors.end(), [&](const auto& p) {
            return p.first == nimrtc::core::kEngineInvalidParam &&
                   p.second.find(missing) != std::string::npos &&
                   p.second.find("does not exist") != std::string::npos;
        });
    EXPECT_TRUE(surfaced)
        << "a broken anchor is a host-facing configuration error (判据 13): "
        << "expected kEngineInvalidParam naming '" << missing << "'; got "
        << (errors.empty() ? std::string{"no on_error fired"} : errors.back().second);
    EXPECT_EQ(engine.turn_tls_peer_verification(), "failed")
        << "the fact distinguishes 'failed' from 'verified' (判据 17)";
    engine.close();
}

TEST_F(EngineTurnCredentialsLive, ValidCaFileMarksVerified) {
    const auto path =
        std::filesystem::temp_directory_path() / "nimrtc_test_anchor.pem";
    {
        std::ofstream out{path, std::ios::binary};
        out << "-----BEGIN CERTIFICATE-----\n"
            << "AAAA\n"
            << "-----END CERTIFICATE-----\n";
    }

    ProviderCtl ctl;
    ctl.creds.host = "relay.example.com";
    ctl.creds.transport = np::TurnTransport::Tls;
    ctl.creds.turn_tls_verify_peer = true;
    ctl.creds.turn_tls_ca_file = path.string();

    EngineConfig cfg = make_config();
    cfg.turn_credentials = provider_trampoline;
    cfg.turn_credentials_ctx = &ctl;

    std::vector<std::pair<std::uint32_t, std::string>> errors;
    NimRTCEngine engine(cfg);
    engine.set_on_error([&](std::uint32_t e, std::string_view m) {
        errors.emplace_back(e, std::string(m));
    });
    ASSERT_EQ(engine.open(), 0u);

    auto* ice = static_cast<RecordingFakeIce*>(kIceFactory->last());
    const auto& calls = ice->turn_calls();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_TRUE(calls[0].verify_peer);
    EXPECT_EQ(calls[0].ca_file, path.string());

    // Unrelated open() diagnostics may fire (see the sibling test) — the
    // assertion is that *none* of them names our loadable anchor.
    const bool mentionsAnchor = std::any_of(
        errors.begin(), errors.end(),
        [&](const auto& p) { return p.second.find(path.string()) != std::string::npos; });
    EXPECT_FALSE(mentionsAnchor)
        << "a loadable anchor must not fire on_error_ (判据 17)";
    EXPECT_EQ(engine.turn_tls_peer_verification(), "verified")
        << "the fact distinguishes 'verified' from 'unverified' (判据 17)";
    engine.close();

    std::filesystem::remove(path);
}

TEST_F(EngineTurnCredentialsLive, NotPemCaFileSurfacesErrorAndMarksFailed) {
    // A present file that is not a PEM trust anchor is a config error too
    // (判据 14) — distinct from the missing-file case, and the fact says
    // "failed", not "verified".
    const auto path =
        std::filesystem::temp_directory_path() / "nimrtc_not_pem_anchor.txt";
    {
        std::ofstream out{path, std::ios::binary};
        out << "this file holds no certificate\n";
    }

    ProviderCtl ctl;
    ctl.creds.host = "relay.example.com";
    ctl.creds.transport = np::TurnTransport::Tls;
    ctl.creds.turn_tls_verify_peer = true;
    ctl.creds.turn_tls_ca_file = path.string();

    EngineConfig cfg = make_config();
    cfg.turn_credentials = provider_trampoline;
    cfg.turn_credentials_ctx = &ctl;

    std::vector<std::pair<std::uint32_t, std::string>> errors;
    NimRTCEngine engine(cfg);
    engine.set_on_error([&](std::uint32_t e, std::string_view m) {
        errors.emplace_back(e, std::string(m));
    });
    ASSERT_EQ(engine.open(), 0u);

    auto* ice = static_cast<RecordingFakeIce*>(kIceFactory->last());
    ASSERT_NE(ice, nullptr);
    const auto& calls = ice->turn_calls();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_TRUE(calls[0].verify_peer);
    EXPECT_EQ(calls[0].ca_file, path.string());

    const bool surfaced = std::any_of(
        errors.begin(), errors.end(), [&](const auto& p) {
            return p.first == nimrtc::core::kEngineInvalidParam &&
                   p.second.find(path.string()) != std::string::npos &&
                   p.second.find("PEM trust anchor") != std::string::npos;
        });
    EXPECT_TRUE(surfaced)
        << "a non-PEM trust anchor is a host-facing configuration error (判据 14): "
        << "expected kEngineInvalidParam naming '" << path.string() << "'; got "
        << (errors.empty() ? std::string{"no on_error fired"} : errors.back().second);
    EXPECT_EQ(engine.turn_tls_peer_verification(), "failed");
    engine.close();

    std::filesystem::remove(path);
}

TEST_F(EngineTurnCredentialsLive, NonTlsCredentialsReportEmptyFact) {
    // A UDP TURN server has no certificate to verify — the fact must be ""
    // (nothing to report), and the ca_file is meaningless on that path.
    ProviderCtl ctl;
    ctl.creds.host = "relay.example.com";
    ctl.creds.transport = np::TurnTransport::Udp;
    ctl.creds.turn_tls_verify_peer = true;
    ctl.creds.turn_tls_ca_file = "ignored.pem";

    EngineConfig cfg = make_config();
    cfg.turn_credentials = provider_trampoline;
    cfg.turn_credentials_ctx = &ctl;

    std::vector<std::pair<std::uint32_t, std::string>> errors;
    NimRTCEngine engine(cfg);
    engine.set_on_error([&](std::uint32_t e, std::string_view m) {
        errors.emplace_back(e, std::string(m));
    });
    ASSERT_EQ(engine.open(), 0u);

    auto* ice = static_cast<RecordingFakeIce*>(kIceFactory->last());
    const auto& calls = ice->turn_calls();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].transport, np::TurnTransport::Udp);
    EXPECT_EQ(calls[0].ca_file, "ignored.pem")
        << "the knob still rides on the call, but...";

    // A UDP relay has no certificate to verify — a ca_file is ignored, so
    // no error may name it, however unrelated open() diagnostics may fire.
    const bool mentionsAnchor = std::any_of(
        errors.begin(), errors.end(),
        [&](const auto& p) { return p.second.find("ignored.pem") != std::string::npos; });
    EXPECT_FALSE(mentionsAnchor)
        << "...it is ignored for a UDP relay — no spurious anchor error";
    EXPECT_EQ(engine.turn_tls_peer_verification(), "")
        << "...a non-TLS relay reports nothing to verify";
    engine.close();
}

}  // namespace
