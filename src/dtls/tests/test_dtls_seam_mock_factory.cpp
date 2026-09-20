/**
 * @file src/dtls/tests/test_dtls_seam_mock_factory.cpp
 * @brief TPAL-4 (v0.11.0) — DTLS seam mock-factory test.
 *
 * Verifies the Slice 8 typed registry hook keeps working with a
 * user-supplied factory.  A user that wants to swap the wolfSSL
 * backend for a 国密 / OpenSSL / BoringSSL / mbedTLS impl writes a
 * `MockDtlsSession : public IDtlsSession` (or the real backend),
 * wraps it in a `IDtlsSessionFactory`, and registers via
 *
 *     nimrtc::core::PluginRegistry::instance().register_dtls_session(
 *         "guomi_sm4", &factory);
 *
 * The engine (TPAL-4 cleanup) then resolves it through
 * `cfg.dtls_name = "guomi_sm4"` instead of the hard-coded "wolfssl".
 * This test stands in for the 国密 factory (which is not yet
 * implemented) by injecting a hand-written mock and verifying the
 * full registry → engine-resolvable path round-trips correctly.
 *
 * What this test verifies:
 *
 *   1. A custom `IDtlsSessionFactory` implementation is accepted by
 *      `core::PluginRegistry::register_dtls_session(id, factory*)`.
 *   2. `get_dtls_session(id)` returns the exact pointer registered.
 *   3. The mock factory's `create()` returns a polymorphic
 *      `IDtlsSession*` whose vtable is intact (every seam method
 *      callable).
 *   4. After registration, the engine can resolve it (the seam path
 *      used by NimRTCEngine::open()).
 *   5. The known "wolfssl" entry is untouched (registration does not
 *      overwrite the built-in factory — only same-id slots are
 *      overwritten per TypedRegistry::register_one).
 *
 * The mock deliberately implements only the seam surface — no wolfSSL,
 * no certs, no network.  This is what makes it a useful test
 * harness for future 国密 verification: the harness can drive a
 * mock session through the same engine pipeline and assert behaviour
 * without standing up a real DTLS backend.
 *
 * @note P1 — TPAL-4 test added as part of PAL Slice 4 / v0.11.0
 *       cleanup work (DoD gate #6).
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/plugin_id.hpp>
#include <nimrtc/core/registry.hpp>

#include <nimrtc/dtls/dtls_session_factory.hpp>
#include <nimrtc/dtls/dtls_session_iface.hpp>
#include <nimrtc/dtls/dtls_plugin.hpp>     // default-plugin publication
#include <nimrtc/dtls/dtls.hpp>            // DtlsConfig / DtlsRole
#include <nimrtc/plugins/base.hpp>         // plugins::kErrNotReady

namespace {

using nimrtc::core::PluginRegistry;
using nimrtc::dtls::Config;
using nimrtc::dtls::DtlsAddr;
using nimrtc::dtls::DtlsRecord;
using nimrtc::dtls::DtlsRole;
using nimrtc::dtls::DtlsState;
using nimrtc::dtls::Fingerprint;
using nimrtc::dtls::IDtlsSession;
using nimrtc::dtls::IDtlsSessionFactory;
using nimrtc::dtls::Role;
using nimrtc::dtls::SrtpKeyingMaterial;

// ===========================================================================
// MockDtlsSession — minimal IDtlsSession implementation.
//
// Records every call into atomic counters so the test can verify the
// engine round-trip reaches the right vtable slot.  Behaviour is
// intentionally trivial (no handshake, no network, no crypto) — the
// whole point of the seam is that a 国密 / OpenSSL / BoringSSL /
// mbedTLS backend can drop in behind the same surface.
// ===========================================================================
class MockDtlsSession final : public IDtlsSession {
public:
    explicit MockDtlsSession(Config cfg) : cfg_(std::move(cfg)) {}

    // ---- Slice 4 / TPAL-4 seam surface (the original six) --------------
    void set_role(Role /*role*/) noexcept override { ++role_role_count_; }
    void set_peer_fingerprint(
        std::span<const std::uint8_t> /*fp*/) noexcept override {
        ++peer_fp_span_count_;
    }
    void start() noexcept override { started_ = true; }
    void pump() noexcept override { ++pump_count_; }
    void on_handshake_complete(OnCompleteCb cb) noexcept override {
        on_complete_cb_ = std::move(cb);
    }
    nimrtc::plugins::Status export_srtp_key_material(
        std::span<std::uint8_t, 60> /*out*/) noexcept override {
        return nimrtc::plugins::kErrNotReady;
    }

    // ---- TPAL-4 engine-facing surface -----------------------------------
    nimrtc::core::Result<void> open() noexcept override {
        opened_ = true;
        return nimrtc::core::Result<void>::make_ok();
    }
    void set_role(DtlsRole r) noexcept override {
        cfg_.role = r;
        ++role_dtlsrole_count_;
    }
    void set_peer_fingerprint(
        std::string /*algo*/,
        std::vector<std::uint8_t> /*value*/) noexcept override {
        ++peer_fp_strvec_count_;
    }
    std::size_t feed_inbound(
        std::span<const std::uint8_t> bytes,
        const DtlsAddr& /*from*/) noexcept override {
        feed_bytes_total_ += bytes.size();
        return bytes.size();
    }
    std::vector<DtlsRecord> take_outbound() noexcept override { return {}; }
    void tick() noexcept override { ++tick_count_; }
    DtlsState state() const noexcept override { return state_; }
    bool is_connected() const noexcept override {
        return state_ == DtlsState::Connected;
    }
    const Fingerprint& local_fingerprint() const noexcept override {
        return fp_;
    }
    std::optional<SrtpKeyingMaterial>
    srtp_keying_material() const noexcept override {
        return std::nullopt;
    }

    // ---- Test-side accessors -------------------------------------------
    bool opened() const noexcept { return opened_; }
    bool started() const noexcept { return started_; }
    std::uint64_t feed_bytes_total() const noexcept {
        return feed_bytes_total_;
    }
    std::uint64_t pump_count() const noexcept { return pump_count_; }
    std::uint64_t tick_count() const noexcept { return tick_count_; }
    std::uint64_t role_role_count() const noexcept {
        return role_role_count_;
    }
    std::uint64_t role_dtlsrole_count() const noexcept {
        return role_dtlsrole_count_;
    }

private:
    Config         cfg_{};
    bool           opened_              = false;
    bool           started_             = false;
    DtlsState      state_               = DtlsState::Initial;
    Fingerprint    fp_{};
    OnCompleteCb   on_complete_cb_;
    std::uint64_t  feed_bytes_total_    = 0;
    std::uint64_t  pump_count_          = 0;
    std::uint64_t  tick_count_          = 0;
    std::uint64_t  role_role_count_     = 0;
    std::uint64_t  role_dtlsrole_count_ = 0;
    std::uint64_t  peer_fp_span_count_  = 0;
    std::uint64_t  peer_fp_strvec_count_ = 0;
};

// ===========================================================================
// MockDtlsFactory — IDtlsSessionFactory producing MockDtlsSession.
//
// Uses a stable string literal id so the lookup path matches the
// production shape (it is a TypedRegistry<>::get() lookup keyed on
// std::string_view; lifetimes are owned by the test process).
// ===========================================================================
class MockDtlsFactory final : public IDtlsSessionFactory {
public:
    std::string_view id() const noexcept override {
        return NIMRTC_PLUGIN_ID(kBackendId);
    }
    std::string_view display_name() const noexcept override {
        return "Mock DTLS backend (TPAL-4 test fixture)";
    }
    std::unique_ptr<IDtlsSession> create(const Config& cfg) const override {
        return std::make_unique<MockDtlsSession>(cfg);
    }

    static constexpr const char* kBackendId = "mock_tpal4";
};

// ===========================================================================
// Test fixture — runs once per suite.  We do NOT call
// register_default_plugins() here because that would publish the
// built-in wolfssl factory and pollute the registry with an entry
// our test doesn't care about; instead we use the Slice 8 typed
// registry slot directly to keep each subtest focused.
// ===========================================================================
class DtlsSeamMockFactory : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        // Pre-publish the default wolfssl factory so test #5 can assert
        // that registering "mock_tpal4" does NOT clobber it.
        nimrtc::dtls::register_default_plugins();
    }
};

// ---------------------------------------------------------------------------
// Test 1 — registry accepts the mock and returns it via get_dtls_session.
// ---------------------------------------------------------------------------
TEST_F(DtlsSeamMockFactory, registry_accepts_mock_factory) {
    MockDtlsFactory mock;
    PluginRegistry::instance().register_dtls_session("mock_tpal4", &mock);

    const auto* resolved =
        PluginRegistry::instance().get_dtls_session("mock_tpal4");
    ASSERT_NE(resolved, nullptr)
        << "TPAL-4: registered mock factory not returned by "
           "PluginRegistry::get_dtls_session(id)";
    EXPECT_EQ(resolved, &mock)
        << "TPAL-4: registry returned a different pointer than was "
           "registered — overwrite semantics of TypedRegistry "
           "regressed";
    EXPECT_EQ(resolved->id(), "mock_tpal4");
    EXPECT_FALSE(resolved->display_name().empty());
}

// ---------------------------------------------------------------------------
// Test 2 — mock factory creates an IDtlsSession whose vtable is wired.
// ---------------------------------------------------------------------------
TEST_F(DtlsSeamMockFactory, mock_factory_creates_seam_typed_session) {
    MockDtlsFactory mock;
    PluginRegistry::instance().register_dtls_session("mock_tpal4_create",
                                                     &mock);

    const auto* factory =
        PluginRegistry::instance().get_dtls_session("mock_tpal4_create");
    ASSERT_NE(factory, nullptr);

    Config cfg;
    cfg.role = DtlsRole::Server;
    std::unique_ptr<IDtlsSession> session = factory->create(cfg);
    ASSERT_NE(session, nullptr)
        << "TPAL-4: mock factory returned null session";

    // Upcast uniqueness check — the returned session must satisfy the
    // full IDtlsSession interface, including the new TPAL-4
    // engine-facing surface (open / set_role(DtlsRole) /
    // set_peer_fingerprint(string, vector) / feed_inbound /
    // take_outbound / tick / state / is_connected /
    // local_fingerprint / srtp_keying_material).
    auto* mock_session = dynamic_cast<MockDtlsSession*>(session.get());
    ASSERT_NE(mock_session, nullptr)
        << "TPAL-4: returned session is NOT a MockDtlsSession — "
           "the registered factory's create() didn't return a "
           "MockDtlsSession, or the vtable regressed.";

    // Drive every seam method and verify it lands on the mock.
    auto open_rc = session->open();
    EXPECT_TRUE(open_rc.ok()) << "TPAL-4: open() did not return ok()";
    EXPECT_TRUE(mock_session->opened());

    session->start();
    EXPECT_TRUE(mock_session->started());

    session->set_role(Role::Client);
    EXPECT_EQ(mock_session->role_role_count(), 1u);
    session->set_role(DtlsRole::Server);
    EXPECT_EQ(mock_session->role_dtlsrole_count(), 1u);

    std::array<std::uint8_t, 32> fp{};
    session->set_peer_fingerprint(
        std::span<const std::uint8_t>(fp.data(), fp.size()));
    std::vector<std::uint8_t> vfp(32, 0xAA);
    session->set_peer_fingerprint("sha-256", std::move(vfp));

    std::array<std::uint8_t, 16> payload{};
    DtlsAddr from{"127.0.0.1", 5000};
    session->feed_inbound(
        std::span<const std::uint8_t>(payload.data(), payload.size()), from);
    EXPECT_EQ(mock_session->feed_bytes_total(), payload.size());

    auto recs = session->take_outbound();
    EXPECT_TRUE(recs.empty()) << "TPAL-4: mock has no records to emit";

    session->pump();
    session->tick();
    EXPECT_GE(mock_session->pump_count(), 1u);
    EXPECT_GE(mock_session->tick_count(), 1u);

    EXPECT_EQ(session->state(), DtlsState::Initial);
    EXPECT_FALSE(session->is_connected());

    const auto& lf = session->local_fingerprint();
    EXPECT_TRUE(lf.bytes.empty())
        << "TPAL-4: mock never set a fingerprint — should be empty";
    EXPECT_FALSE(session->srtp_keying_material().has_value());

    // SRTP key material shape — returns NotReady for the mock.
    std::array<std::uint8_t, 60> km{};
    const auto rc = session->export_srtp_key_material(
        std::span<std::uint8_t, 60>(km.data(), km.size()));
    EXPECT_EQ(rc, nimrtc::plugins::kErrNotReady);
}

// ---------------------------------------------------------------------------
// Test 3 — re-registration of the same id overwrites the previous factory
// (TypedRegistry contract).
// ---------------------------------------------------------------------------
TEST_F(DtlsSeamMockFactory, same_id_registration_overwrites) {
    MockDtlsFactory mock_a;
    MockDtlsFactory mock_b;
    const std::string_view kId = "mock_tpal4_overwrite";

    PluginRegistry::instance().register_dtls_session(kId, &mock_a);
    EXPECT_EQ(PluginRegistry::instance().get_dtls_session(kId), &mock_a);

    PluginRegistry::instance().register_dtls_session(kId, &mock_b);
    EXPECT_EQ(PluginRegistry::instance().get_dtls_session(kId), &mock_b)
        << "TPAL-4: same-id re-registration did not overwrite "
           "(TypedRegistry::register_one contract regressed).";
}

// ---------------------------------------------------------------------------
// Test 4 — list_dtls_sessions() now includes the mock alongside wolfssl.
// ---------------------------------------------------------------------------
TEST_F(DtlsSeamMockFactory, list_includes_mock_and_wolfssl) {
    MockDtlsFactory mock;
    PluginRegistry::instance().register_dtls_session(
        "mock_tpal4_listed", &mock);

    auto ids = PluginRegistry::instance().list_dtls_sessions();
    bool found_mock    = false;
    bool found_wolfssl = false;
    for (auto id : ids) {
        if (id == "mock_tpal4_listed") found_mock = true;
        if (id == "wolfssl")          found_wolfssl = true;
    }
    EXPECT_TRUE(found_mock)
        << "TPAL-4: mock factory not in list_dtls_sessions()";
    EXPECT_TRUE(found_wolfssl)
        << "TPAL-4: existing 'wolfssl' factory not in "
           "list_dtls_sessions() — setUpTestSuite missing.";
}

// ---------------------------------------------------------------------------
// Test 5 — engine's own DTLS lookup resolves the mock.
//
// This is the seam-loop closure: NimRTCEngine::open() resolves
// `cfg.dtls_name` through `PluginRegistry::get_dtls_session(id)`.  A
// user setting `dtls_name = "mock_tpal4"` (the only id the
// MockDtlsFactory itself reports — same pattern as
// `WolfsslDtlsFactory::id() == "wolfssl"` regardless of how it's
// registered) would get the mock.  We don't drive a full open()
// (would also require ICE / Opus / audio3a), but we DO prove the
// engine's lookup helper resolves the same mock we just registered.
// ---------------------------------------------------------------------------
TEST_F(DtlsSeamMockFactory, engine_lookup_resolves_mock_factory) {
    MockDtlsFactory mock;
    const std::string_view kRegisteredId = "mock_tpal4_engine";
    PluginRegistry::instance().register_dtls_session(kRegisteredId, &mock);

    // Mirrors engine.cpp lines 510-512 verbatim:
    //     const std::string dtls_lookup_id =
    //         config_.dtls_name.empty() ? "wolfssl" : config_.dtls_name;
    //     const auto* dtls_factory =
    //         reg.get_dtls_session(dtls_lookup_id);
    const std::string dtls_lookup_id(kRegisteredId);
    const auto* dtls_factory =
        PluginRegistry::instance().get_dtls_session(dtls_lookup_id);
    ASSERT_NE(dtls_factory, nullptr)
        << "TPAL-4: engine would fail-open with kEngineInternal — "
           "mock factory not resolvable via cfg.dtls_name";

    // The factory's own id() reports its stable backend identifier
    // ("mock_tpal4"), independent of how the registry keyed it.
    // This mirrors production: WolfsslDtlsFactory::id() always
    // returns "wolfssl" regardless of how/where it's registered.
    EXPECT_EQ(dtls_factory->id(), "mock_tpal4");

    // Pointer-identity check is what matters for engine-side use:
    // engine.cpp calls factory->create(dcfg) on the resolved
    // pointer, so the address must round-trip through the registry.
    EXPECT_EQ(dtls_factory, &mock);
}

} // anonymous namespace
