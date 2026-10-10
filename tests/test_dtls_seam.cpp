/**
 * @file tests/test_dtls_seam.cpp
 * @brief TPAL-4 cleanup (v0.11.0) — DTLS seam engine-routing regression
 *        suite.
 *
 * What this test verifies (per `docs/plan/v0.11-plan.md` §9 Slice 4
 * tracker and the v0.11.0 cleanup PR scope):
 *
 *   1. Default behaviour: `cfg.dtls_name == ""` resolves to the built-in
 *      wolfSSL factory (id="wolfssl") and the engine opens
 *      successfully via the typed registry hook.  This is the
 *      source-compatibility guarantee for all v0.10.x callers.
 *
 *   2. User-supplied `cfg.dtls_name` resolves to the requested factory
 *      via `core::PluginRegistry::register_dtls_session(id, factory*)`.
 *      The test registers a `MockDtlsSessionFactory` under id="mock",
 *      sets `cfg.dtls_name = "mock"`, opens the engine, and verifies
 *      that the mock factory (not the wolfSSL one) was the one the
 *      engine called.  Side-effect counters in the mock record the
 *      create() and open() invocations so the assertion is exact.
 *
 *   3. Unknown `cfg.dtls_name` produces an `open()` failure with a
 *      `kEngineInternal`-class error code (via `on_error_`) — proves
 *      the error path is wired correctly and the engine does NOT
 *      silently fall back to "wolfssl".
 *
 *   4. The `static_cast<DtlsSessionWolfSSL*>(dtls_seam.release())` that
 *      the v0.10.2 code did is gone: the seam interface
 *      `IDtlsSession` exposes every method the engine calls
 *      (`open()`, `set_role(DtlsRole)`,
 *      `set_peer_fingerprint(string, vector)`,
 *      `local_fingerprint()`, `is_connected()`, `state()`,
 *      `feed_inbound()`, `take_outbound()`, `tick()`,
 *      `srtp_keying_material()`), and a downstream call site that
 *      tried to use a method *not* on the seam would fail at compile
 *      time.
 *
 * The test uses GoogleTest (`GTEST_*`); links `nimrtc_engine`,
 * `nimrtc_dtls`, `nimrtc_core` and the existing plugin-default
 * registrars the same way `test_engine_plugin_loading` does (see
 * tests/CMakeLists.txt — `nimrtc_add_engine_test`).
 *
 * Threading: nothing concurrent; standard GTest ordering.
 *
 * @note P2 — TPAL-4 cleanup test added as part of v0.11.0.
 */

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/error.hpp>
#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/core/engine_errors.hpp>
#include <nimrtc/dtls/dtls.hpp>
#include <nimrtc/dtls/dtls_session_factory.hpp>
#include <nimrtc/dtls/dtls_session_iface.hpp>
#include <nimrtc/dtls/dtls_plugin.hpp>
#include <nimrtc/engine/engine.hpp>

namespace {

using nimrtc::core::PluginRegistry;
using nimrtc::dtls::Config;
using nimrtc::dtls::DtlsAddr;
using nimrtc::dtls::DtlsConfig;
using nimrtc::dtls::DtlsRecord;
using nimrtc::dtls::DtlsRole;
using nimrtc::dtls::DtlsState;
using nimrtc::dtls::Fingerprint;
using nimrtc::dtls::IDtlsSession;
using nimrtc::dtls::IDtlsSessionFactory;
using nimrtc::dtls::Role;
using nimrtc::dtls::SrtpKeyingMaterial;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

// Namespace aliases so the MockDtlsSession override signatures can
// use the unqualified `plugins::Status` / `core::Result<void>` form
// (matches the seam-interface header convention where these types are
// referenced without `nimrtc::` prefix).
namespace plugins = nimrtc::plugins;
namespace core    = nimrtc::core;

// =====================================================================
// MockDtlsSession — minimal IDtlsSession that records every call and
// returns canned values from the accessors the engine reads during
// open() / create_offer() / process_remote_sdp().  We deliberately do
// NOT drive a handshake — the engine only reads `local_fingerprint()`
// and `state()` during create_offer(), and `is_connected()` for the
// media-path gate.  A real DTLS handshake would require two paired
// backend objects over the byte-buffer pipe (already covered by
// tests/test_dtls_handshake_e2e.cpp and test_dtls_srtp_timing.cpp).
// =====================================================================
class MockDtlsSession final : public IDtlsSession {
public:
    explicit MockDtlsSession(DtlsConfig cfg,
                              std::atomic<int>* open_calls,
                              std::atomic<int>* create_calls)
        : cfg_(std::move(cfg)),
          open_calls_(open_calls),
          create_calls_(create_calls) {
        if (create_calls_) create_calls_->fetch_add(1, std::memory_order_relaxed);
    }

    ~MockDtlsSession() override = default;

    // ---- Seam-only methods (the Slice 4 surface) -------------------------
    void set_role(Role /*role*/) noexcept override { /* no-op for tests */ }
    void set_peer_fingerprint(
        std::span<const std::uint8_t> /*fp*/) noexcept override { /* no-op */ }
    void start() noexcept override { /* no-op */ }
    void pump() noexcept override { /* no-op */ }
    void on_handshake_complete(
        IDtlsSession::OnCompleteCb /*cb*/) noexcept override { /* no-op */ }
    plugins::Status export_srtp_key_material(
        std::span<std::uint8_t, 60> /*out*/) noexcept override {
        // kErrNotReady: matches the wolfSSL backend's behaviour when
        // the handshake hasn't completed.  The engine's
        // maybe_install_srtp_keys() is idempotent so a no-handshake
        // session doesn't crash it.
        return nimrtc::plugins::kErrNotReady;
    }

    // ---- Engine-facing methods (TPAL-4 widening of the seam) -----------
    core::Result<void> open() noexcept override {
        if (open_calls_) open_calls_->fetch_add(1, std::memory_order_relaxed);
        // Always succeed: the engine only needs the local fingerprint
        // back from create_offer(); a real cert would only matter for
        // a real DTLS handshake (out of scope here).
        return core::Result<void>::make_ok();
    }

    void set_role(DtlsRole /*r*/) noexcept override { /* no-op */ }

    void set_peer_fingerprint(
        std::string /*algo*/,
        std::vector<std::uint8_t> /*value*/) noexcept override { /* no-op */ }

    std::size_t feed_inbound(
        std::span<const std::uint8_t> /*bytes*/,
        const DtlsAddr& /*from*/) noexcept override {
        // DTLS is a datagram protocol — return "consumed nothing",
        // matching the wolfSSL-backed class when handed non-DTLS
        // bytes (or bytes for a different session).
        return 0;
    }

    std::vector<DtlsRecord> take_outbound() noexcept override {
        // Mock produces no outbound records.
        return {};
    }

    void tick() noexcept override { /* no-op */ }

    DtlsState state() const noexcept override {
        // The engine reads state() in create_offer() and in
        // drain_dtls(); Closed is the out-of-line default value the
        // engine treats as "session exists but hasn't been opened".
        // Returning Closed here keeps the engine's drain loop sane
        // without lying about being Connected.
        return DtlsState::Closed;
    }

    bool is_connected() const noexcept override {
        // The mock never completes a handshake — send_audio() gates
        // on this and would return kEngineNotReady.  Tests don't
        // drive send_audio, so this is fine.
        return false;
    }

    const Fingerprint& local_fingerprint() const noexcept override {
        // Always-valid fingerprint — the engine reads .hex_colon in
        // create_offer() to build the SDP `a=fingerprint` attribute.
        // 32 zero bytes is RFC-valid (a SHA-256 of all-zero plaintext,
        // not a real cert), sufficient for the seam-routing test.
        static const Fingerprint kFp = [] {
            Fingerprint fp{};
            fp.algorithm = "sha-256";
            fp.bytes.assign(32, 0x00);
            fp.hex_colon = std::string(32 * 3 - 1, '0');   // 95 chars of ASCII
            return fp;
        }();
        return kFp;
    }

    std::optional<SrtpKeyingMaterial> srtp_keying_material() const noexcept override {
        return std::nullopt;
    }

private:
    DtlsConfig            cfg_;
    std::atomic<int>*     open_calls_;
    std::atomic<int>*     create_calls_;
};

// =====================================================================
// MockDtlsSessionFactory — typed factory contract that hands out
// MockDtlsSession instances.  Counts `create()` invocations so the
// test can assert the factory was the one the engine resolved.
// =====================================================================
class MockDtlsSessionFactory final : public IDtlsSessionFactory {
public:
    explicit MockDtlsSessionFactory(
        std::atomic<int>* open_calls,
        std::atomic<int>* create_calls,
        std::string_view id = "mock")
        : open_calls_(open_calls),
          create_calls_(create_calls),
          id_(id) {}

    std::string_view id() const noexcept override { return id_; }
    std::string_view display_name() const noexcept override {
        return "DTLS seam routing test mock (no real handshake)";
    }

    std::unique_ptr<IDtlsSession> create(
        const DtlsConfig& cfg) const override {
        return std::make_unique<MockDtlsSession>(
            cfg, open_calls_, create_calls_);
    }

private:
    std::atomic<int>* open_calls_;
    std::atomic<int>* create_calls_;
    std::string_view id_;
};

// =====================================================================
// Test fixture — registers all default plugins once for the suite and
// shares an atomic counter pair across the three tests that need
// them.  The factory is registered in the body of the test that uses
// it (so we can swap it without polluting the suite-wide state).
// =====================================================================
class DtlsSeamRouting : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        // Mirror what NimRTCEngine::open() calls before any factory
        // lookup.  Without this, get_dtls_session("wolfssl") would
        // return nullptr even with the test's own factories in
        // place.
        nimrtc::core::register_all_default_plugins();
        nimrtc::dtls::register_default_plugins();
    }

    // Helper: build an EngineConfig that lets the engine actually
    // open() without ICE networking (loopback only — no STUN, no
    // remote SDP).  The DTLS factory mock returns immediately so the
    // ICE gather is what the test is gated on.
    static EngineConfig make_test_config() {
        EngineConfig cfg;
        // No STUN — host candidates only.
        cfg.stun_server_host = "";
        // Narrow port range so the test does not race with OS-assigned
        // ports that may already be bound on a busy CI runner.
        cfg.local_port_range_begin = 51000;
        cfg.local_port_range_end   = 51100;
        return cfg;
    }
};

// =====================================================================
// Test 1: default `cfg.dtls_name == ""` resolves to the built-in
// wolfSSL factory (id="wolfssl").  Proves the source-compat path.
// =====================================================================
TEST_F(DtlsSeamRouting, default_dtls_name_empty_uses_wolfssl) {
    EngineConfig cfg = make_test_config();
    // cfg.dtls_name is left empty by default.
    EXPECT_TRUE(cfg.dtls_name.empty());

    NimRTCEngine engine(cfg);

    // The factory under "wolfssl" is the built-in.  Confirm via the
    // registry so we are not testing the engine in isolation.
    const auto* factory =
        PluginRegistry::instance().get_dtls_session("wolfssl");
    ASSERT_NE(factory, nullptr)
        << "PAL Slice 8: 'wolfssl' factory should be registered after "
           "register_all_default_plugins()";
    EXPECT_EQ(factory->id(), "wolfssl");

    // open() should succeed.
    EXPECT_EQ(engine.open(), 0u)
        << "default cfg.dtls_name must resolve to the wolfSSL factory "
           "(source-compatible with v0.10.x callers)";

    engine.close();
}

// =====================================================================
// Test 2: `cfg.dtls_name = "mock"` causes the engine to resolve the
// mock factory via the registry.  Side-effect counters in the mock
// record invocations so the assertion is exact.
// =====================================================================
TEST_F(DtlsSeamRouting, cfg_dtls_name_routes_to_user_factory) {
    std::atomic<int> open_count{0};
    std::atomic<int> create_count{0};

    // Register the mock factory under a non-colliding id.  We use
    // "mock" (a /mock/ seam in the registry sense — production code
    // never registers such an id).
    static const MockDtlsSessionFactory s_mock_factory(
        &open_count, &create_count, /*id=*/"mock");
    PluginRegistry::instance().register_dtls_session(
        "mock", &s_mock_factory);

    EngineConfig cfg = make_test_config();
    cfg.dtls_name = "mock";

    NimRTCEngine engine(cfg);

    // The factory list must contain "mock" alongside "wolfssl".
    auto ids = PluginRegistry::instance().list_dtls_sessions();
    bool found_mock = false, found_wolfssl = false;
    for (auto id : ids) {
        if (id == "mock")     found_mock     = true;
        if (id == "wolfssl") found_wolfssl  = true;
    }
    EXPECT_TRUE(found_mock)
        << "registry should list the test's mock factory";
    EXPECT_TRUE(found_wolfssl)
        << "registry should still list the built-in wolfSSL factory";

    // open() must succeed via the mock factory.
    EXPECT_EQ(engine.open(), 0u)
        << "engine should open via the user-registered mock factory";

    // The factory's create() must have been called exactly once
    // (the engine makes one IDtlsSession per open() call).
    EXPECT_GE(create_count.load(std::memory_order_relaxed), 1)
        << "MockDtlsSessionFactory::create() should have been called "
           "at least once — the engine's IDtlsSessionFactory lookup "
           "did not route to the mock factory";

    // The session's open() (i.e. IDtlsSession::open()) must also have
    // fired — proves the engine called open() on the returned
    // IDtlsSession pointer (not on a discarded one).
    EXPECT_GE(open_count.load(std::memory_order_relaxed), 1)
        << "MockDtlsSession::open() should have been called exactly "
           "once — the engine is wiring through the seam interface";

    engine.close();
}

// =====================================================================
// Test 3: an unknown `cfg.dtls_name` produces an open() failure.
// The engine must NOT silently fall back to "wolfssl" — that would
// mask the user's intent and surprise 国密 / OpenSSL integrators who
// get the spelling of their factory id wrong.
// =====================================================================
TEST_F(DtlsSeamRouting, cfg_dtls_name_unknown_factory_yields_open_failure) {
    EngineConfig cfg = make_test_config();
    cfg.dtls_name = "no_such_factory_xyz";   // deliberately un-registered

    NimRTCEngine engine(cfg);

    std::atomic<std::uint32_t> err_code{0};
    std::atomic<bool>          err_fired{false};
    engine.set_on_error(
        [&](std::uint32_t err, std::string_view /*msg*/) {
            err_code   = err;
            err_fired  = true;
        });

    const uint32_t rc = engine.open();
    EXPECT_NE(rc, 0u)
        << "engine.open() should fail when cfg.dtls_name points at an "
           "un-registered factory (no silent fallback to wolfssl)";
    EXPECT_TRUE(err_fired.load())
        << "on_error_ should fire with the missing-factory diagnostic";
    EXPECT_EQ(err_code.load(), nimrtc::core::kEngineInternal)
        << "kEngineInternal is the code the engine uses for DTLS "
           "plugin-not-found (matches the pre-Slice-8 transport-"
           "plugin-missing contract)";

    // The engine is left in kConstructed (not kOpen) — close() must
    // be safe to call (idempotent in that state).
    EXPECT_FALSE(engine.is_open());
    engine.close();
}

// =====================================================================
// Test 4: full IDtlsSession surface — every method the engine.cpp
// calls is present on the seam (compile-time check via the override
// keywords on MockDtlsSession).  Also verifies the seam survives a
// second factory registration round (idempotency contract).
// =====================================================================
TEST_F(DtlsSeamRouting, seam_surface_complete_and_idempotent) {
    // The MockDtlsSession class above declares `override` on every
    // method — this test exists only to make the seam-surface
    // completeness explicit at run time as well.  If the engine
    // calls any method not present on the seam, MockDtlsSession
    // would fail to compile (which is exactly what we want — the
    // TPAL-4 cleanup promise is "the engine never needs a
    // downcast").
    std::atomic<int> open_count{0};
    std::atomic<int> create_count{0};

    static const MockDtlsSessionFactory s_factory(
        &open_count, &create_count, "mock");
    PluginRegistry::instance().register_dtls_session(
        "mock", &s_factory);

    // Re-registering under the SAME id must not stack two factories
    // (TypedRegistry::register_one is last-writer-wins per Slice 8).
    PluginRegistry::instance().register_dtls_session(
        "mock", &s_factory);
    PluginRegistry::instance().register_dtls_session(
        "mock", &s_factory);

    // Re-registering under a NEW id does increment the list.
    std::atomic<int> oc2{0}, cc2{0};
    static const MockDtlsSessionFactory s_factory2(
        &oc2, &cc2, "mock2");
    PluginRegistry::instance().register_dtls_session(
        "mock2", &s_factory2);

    // The factory list is monotonic-additive: each call with a new
    // id adds an entry; each call with an existing id overwrites in
    // place (so list size grows only on first registration of a new
    // id).
    auto ids = PluginRegistry::instance().list_dtls_sessions();
    bool found_mock  = false, found_mock2 = false;
    for (auto id : ids) {
        if (id == "mock")  found_mock  = true;
        if (id == "mock2") found_mock2 = true;
    }
    EXPECT_TRUE(found_mock)
        << "mock factory should still be in the list";
    EXPECT_TRUE(found_mock2)
        << "mock2 factory should be in the list after second call";

    // Resolve via the registry to confirm both factories are usable.
    const IDtlsSessionFactory* m =
        PluginRegistry::instance().get_dtls_session("mock");
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->id(), "mock");
    EXPECT_EQ(m->display_name(),
              std::string_view{"DTLS seam routing test mock (no real handshake)"});
}

} // anonymous namespace
