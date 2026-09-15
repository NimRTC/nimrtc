/**
 * @file src/dtls/tests/test_dtls_factory.cpp
 * @brief PAL Slice 4 — DTLS seam factory smoke test.
 *
 * Verifies the four invariants the Slice 4 DoD requires:
 *
 *   1. `WolfsslDtlsFactory::create(DtlsConfig)` returns a non-null
 *      `unique_ptr<IDtlsSession>` (the factory wires the seam product
 *      correctly).
 *   2. The returned session IS an `IDtlsSession` (the refactor of
 *      `DtlsSessionWolfSSL` to inherit `IDtlsSession` succeeded — no
 *      missing-virtual-method compile error in the .cpp).
 *   3. `WolfsslDtlsFactory::id()` returns the literal "wolfssl"
 *      (non-empty by construction so PAL Slice 3's
 *      NIMRTC_PLUGIN_ID("wolfssl") compile-time validator accepts it).
 *   4. `nimrtc::dtls::register_default_plugins()` is idempotent
 *      (Meyer-singleton latch; matches the ice/rtp/audio3a pattern).
 *
 * Tests deliberately do NOT drive a real DTLS handshake — that would
 * require two paired `DtlsSessionWolfSSL` objects running the byte-
 * buffer handshake state machine over each other's output and is
 * already covered by `tests/test_dtls_handshake_e2e.cpp` and
 * `tests/test_dtls_srtp_timing.cpp` at the repo root.  This test is
 * the Slice 4 seam-only validation: it proves the seam compiles,
 * links, and produces the expected factory surface.
 *
 * @note P1 — Slice 4 test added as part of PAL Slice 4 (v0.11.0).
 */

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include <nimrtc/dtls/dtls_session_factory.hpp>
#include <nimrtc/dtls/dtls_session_iface.hpp>
#include <nimrtc/dtls/dtls_plugin.hpp>
#include <nimrtc/dtls/dtls.hpp>                    // DtlsConfig (alias of Config)
#include <nimrtc/dtls/dtls_wolfssl_session.hpp>    // refactored to inherit IDtlsSession
#include <nimrtc/plugins/base.hpp>                  // kErrNotReady

namespace {

using nimrtc::dtls::DtlsConfig;
using nimrtc::dtls::DtlsRole;
using nimrtc::dtls::DtlsSessionWolfSSL;
using nimrtc::dtls::IDtlsSession;
using nimrtc::dtls::IDtlsSessionFactory;
using nimrtc::dtls::Role;

// The factory is published by `register_default_plugins()` through the
// `nimrtc::dtls::test_only::` accessor — declared in
// <nimrtc/dtls/dtls_plugin.hpp>.  Slice 7 will route the canonical
// lookup through core::PluginRegistry; for Slice 4 the test-only
// accessor is the documented seam-local path.

// ===========================================================================
// Test fixture — keeps each subtest isolated and the registration
// idempotency check deterministic (we call register_default_plugins()
// once per process so subsequent calls can verify the latch).
// ===========================================================================
class DtlsSeamFactory : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        // Mirror the call sites that consumers will use.
        nimrtc::dtls::register_default_plugins();
    }
};

// ---------------------------------------------------------------------------
// Test 1: factory creates an IDtlsSession
// ---------------------------------------------------------------------------
TEST_F(DtlsSeamFactory, factory_create_returns_idtls_session) {
    const IDtlsSessionFactory* factory =
        nimrtc::dtls::test_only::get_wolfssl_factory();
    ASSERT_NE(factory, nullptr)
        << "factory is nullptr after register_default_plugins() — "
           "the test-only publication hook in dtls_plugin.cpp is broken";

    DtlsConfig cfg;
    cfg.role = DtlsRole::Server;

    std::unique_ptr<IDtlsSession> session = factory->create(cfg);
    ASSERT_NE(session, nullptr)
        << "factory.create() returned nullptr — factory wiring is broken";

    // Calling a seam method on the returned session proves the
    // upcast from the concrete DtlsSessionWolfSSL worked AND the
    // IDtlsSession vtable is fully populated (no missing overrides).
    // We deliberately do NOT call start() — that would load the
    // bundled ECC cert from disk and fail in sandboxed test envs
    // that don't have the cert fallback chain.  set_role is a pure
    // setter that survives even with state == Initial.
    session->set_role(Role::Client);
    EXPECT_NE(session.get(), nullptr);
}

// ---------------------------------------------------------------------------
// Test 2: returned session IS-A IDtlsSession (dynamic_cast verification)
// ---------------------------------------------------------------------------
//
// A pure interface-check via dynamic_cast would be redundant given the
// factory return type is already `unique_ptr<IDtlsSession>`.  The
// value of this test is verifying the *concrete* class still inherits
// IDtlsSession in addition to the factory producing it: if a future
// refactor accidentally drops the inheritance, this test fails at
// compile time.
TEST_F(DtlsSeamFactory, concrete_session_inherits_idtls_session) {
    DtlsConfig cfg;
    cfg.role = DtlsRole::Server;

    // Heap-allocate a concrete session — same construction the
    // factory performs internally.
    std::unique_ptr<DtlsSessionWolfSSL> concrete =
        std::make_unique<DtlsSessionWolfSSL>(cfg);
    ASSERT_NE(concrete, nullptr);

    // Implicit upcast to IDtlsSession — must succeed at compile time.
    std::unique_ptr<IDtlsSession> seam =
        std::unique_ptr<IDtlsSession>(std::move(concrete));
    ASSERT_NE(seam, nullptr);

    // Verify the IDtlsSession vtable is fully wired by calling every
    // seam method (noexcept — none should throw).  We don't pass any
    // payload; just exercise the dispatch path.
    seam->set_role(Role::Client);
    std::array<std::uint8_t, 32> dummy_fp{};
    seam->set_peer_fingerprint(
        std::span<const std::uint8_t>(dummy_fp.data(), dummy_fp.size()));
    seam->start();
    seam->pump();
    seam->on_handshake_complete(IDtlsSession::OnCompleteCb{});
    std::array<std::uint8_t, 60> dummy_km{};
    const auto rc = seam->export_srtp_key_material(
        std::span<std::uint8_t, 60>(dummy_km.data(), dummy_km.size()));
    // Not Ready yet — handshake never ran.
    EXPECT_EQ(rc, nimrtc::plugins::kErrNotReady);
}

// ---------------------------------------------------------------------------
// Test 3: factory id() returns the "wolfssl" literal
// ---------------------------------------------------------------------------
TEST_F(DtlsSeamFactory, factory_id_is_wolfssl) {
    const IDtlsSessionFactory* factory =
        nimrtc::dtls::test_only::get_wolfssl_factory();
    ASSERT_NE(factory, nullptr);

    const std::string_view id = factory->id();
    EXPECT_FALSE(id.empty())
        << "factory id must be non-empty (PAL Slice 3 NIMRTC_PLUGIN_ID "
           "validator would reject empty literals)";
    EXPECT_EQ(id, "wolfssl")
        << "factory id mismatch — Slice 7 / Slice 8 will look up this "
           "exact id from StackConfig::dtls_id and the JSON Profile "
           "`transport.dtls` segment";
}

// ---------------------------------------------------------------------------
// Test 4: register_default_plugins() is idempotent
// ---------------------------------------------------------------------------
TEST_F(DtlsSeamFactory, register_default_plugins_is_idempotent) {
    // SetUpTestSuite already called once.  Call twice more here; the
    // Meyer-singleton latch must short-circuit the second+ calls
    // without observable effect (no double-register, no log spam).
    //
    // We can't observe "no double-register" without a registry
    // counter — Slice 7 introduces the typed DTLS registry slot.
    // For Slice 4 we verify the property is true at compile time
    // (the static-local latch compiles) and at run time (no crash
    // on repeated invocation).
    EXPECT_NO_THROW({
        nimrtc::dtls::register_default_plugins();
        nimrtc::dtls::register_default_plugins();
        nimrtc::dtls::register_default_plugins();
    });

    // Sanity check: factory is still reachable after repeated
    // registration.  If the latch were broken this would re-construct
    // the factory (acceptable behaviour, but the static-local latch
    // guarantees it's only constructed once).
    const IDtlsSessionFactory* factory =
        nimrtc::dtls::test_only::get_wolfssl_factory();
    ASSERT_NE(factory, nullptr);
    EXPECT_EQ(factory->id(), "wolfssl");
}

} // anonymous namespace
