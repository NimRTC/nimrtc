/**
 * @file tests/test_registry_contract.cpp
 * @brief Architecture-remediation Phase 0.2 — registry consistency guard.
 *
 * ## Why this test exists
 *
 * The architecture-remediation plan (`docs/plan/architecture-remediation-plan.md`
 * §0.2, defect A1 / A2) found that `StackConfig` shipped defaults that do
 * **not** match the ids the modules actually register:
 *
 *   - `StackConfig::rtp_id` defaults to `"nim"`, but the RTP module
 *     registers its factory under `"webrtc"`
 *     (`src/modules/rtp/src/rtp_plugin.cpp:255`).
 *   - `StackConfig::sctp_id` defaults to `"usrsctp"`, but the SCTP module
 *     deliberately registers only `"stub"`
 *     (`src/sctp/src/sctp_stub_factory.hpp:49`).
 *
 * The mismatch was invisible because `WebRtcClassicStackFactory::create()`
 * returned a **half-built** stack instead of `nullptr` on a failed factory
 * lookup (`src/transport/src/webrtc_classic_stack.cpp:239,253,274,290`),
 * so the engine's null-check was a no-op and the defect only surfaced as a
 * null dereference later.
 *
 * This test pins the contract so the mismatch cannot come back:
 *
 *   1. Every non-empty `StackConfig` default id must resolve to a real
 *      factory in the corresponding registry slot.
 *   2. Every registry slot that the engine can resolve must contain at
 *      least one entry after the default registrars run.
 *   3. A factory lookup that fails must produce a **null** stack, never a
 *      partially-constructed one.
 *
 * ## Expected behaviour before the Phase 1 fix
 *
 * Test 1 FAILS on the unfixed tree (the `"nim"` / `"usrsctp"` defaults do
 * not resolve).  Test 3 FAILS too (the factory returns a half-built
 * stack).  That is the point of a guard: it must be red before the fix
 * and green after it.
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "nimrtc/core/registry.hpp"
#include "nimrtc/transport/transport_stack.hpp"

namespace {

using nimrtc::core::PluginRegistry;

/// Populate the registry with every built-in factory.
///
/// The engine calls `core::register_all_default_plugins()` during
/// `open()`.  The test does the same so the registry mirrors the
/// production state.
void RegisterAllDefaults() {
    nimrtc::core::register_all_default_plugins();
}

/// A registry slot's `list_*()` accessor plus a human-readable name.
struct SlotProbe {
    const char* name;
    std::vector<std::string> ids;
};

/// Collect the ids of every slot the engine can resolve through
/// `EngineConfig` / `StackConfig`.
///
/// `raw_id` is deliberately absent: `StackConfig::raw_id` defaults to the
/// empty string, which means "do not enable raw_control()" — an empty
/// slot is the *correct* state for it (see `transport_stack.hpp:86`).
std::vector<SlotProbe> CollectSlots() {
    PluginRegistry& reg = PluginRegistry::instance();
    return {
        {"transport",         reg.list_transports()},
        {"ice_transport",    reg.list_ice_transports()},
        {"rtp",              reg.list_rtp()},
        {"sdp",              reg.list_sdp()},
        {"jb",               reg.list_jb()},
        {"audio3a",          reg.list_audio3a()},
        {"codec",            reg.list_codecs()},
        {"dtls_session",     reg.list_dtls_sessions()},
        {"sctp_socket",      reg.list_sctp_sockets()},
        {"transport_stack",  reg.list_transport_stacks()},
    };
}

/// Register an id carried by a `std::string` that dies on return.
///
/// The registry used to store a `std::string_view` key, so a plugin
/// registering under a temporary string left the registry holding a
/// dangling view.  The probe must therefore destroy the string *before*
/// the id is read back — otherwise the view still points at live memory
/// and the bug stays hidden.
void RegisterUnderTemporaryString(PluginRegistry& reg) {
    const std::string temporary = "nimrtc_contract_probe_temp_id";
    reg.register_datachannel(temporary, nullptr);
    // `temporary` is destroyed here.
}

/// The id the probe registered, as a literal so the comparison does not
/// depend on any live string.
constexpr const char* kProbeId = "nimrtc_contract_probe_temp_id";

} // namespace

// ---------------------------------------------------------------------------
// 1. StackConfig defaults must resolve.
// ---------------------------------------------------------------------------
TEST(RegistryContract, StackConfigDefaultRtpIdResolves) {
    RegisterAllDefaults();
    const nimrtc::StackConfig cfg;
    ASSERT_FALSE(cfg.rtp_id.empty());
    EXPECT_NE(nullptr, PluginRegistry::instance().get_rtp(cfg.rtp_id))
        << "StackConfig::rtp_id defaults to '" << cfg.rtp_id
        << "' but no RTP factory is registered under that id.";
}

TEST(RegistryContract, StackConfigDefaultSctpIdResolves) {
    RegisterAllDefaults();
    const nimrtc::StackConfig cfg;
    ASSERT_FALSE(cfg.sctp_id.empty());
    EXPECT_NE(nullptr, PluginRegistry::instance().get_sctp_socket(cfg.sctp_id))
        << "StackConfig::sctp_id defaults to '" << cfg.sctp_id
        << "' but no SCTP socket factory is registered under that id.";
}

TEST(RegistryContract, StackConfigDefaultIceIdResolves) {
    RegisterAllDefaults();
    const nimrtc::StackConfig cfg;
    EXPECT_NE(nullptr,
              PluginRegistry::instance().get_ice_transport(cfg.ice_id))
        << "StackConfig::ice_id defaults to '" << cfg.ice_id
        << "' but no ICE factory is registered under that id.";
}

TEST(RegistryContract, StackConfigDefaultDtlsIdResolves) {
    RegisterAllDefaults();
    const nimrtc::StackConfig cfg;
    EXPECT_NE(nullptr,
              PluginRegistry::instance().get_dtls_session(cfg.dtls_id))
        << "StackConfig::dtls_id defaults to '" << cfg.dtls_id
        << "' but no DTLS factory is registered under that id.";
}

// ---------------------------------------------------------------------------
// 2. Every resolvable slot must be populated.
// ---------------------------------------------------------------------------

TEST(RegistryContract, EveryResolvableSlotIsPopulated) {
    RegisterAllDefaults();
    for (const SlotProbe& slot : CollectSlots()) {
        EXPECT_FALSE(slot.ids.empty())
            << "Registry slot '" << slot.name
            << "' is empty after register_all_default_plugins(). "
               "A slot the engine can resolve must have at least one "
               "built-in entry.";
    }
}

// ---------------------------------------------------------------------------
// 3. Registration keys must survive the registry's lifetime.
// ---------------------------------------------------------------------------

TEST(RegistryContract, RegisteredIdSurvivesTemporaryStringLifetime) {
    // `TypedRegistry::register_one` used to store a `std::string_view`
    // key.  A third-party plugin registering under a temporary
    // `std::string` would leave the registry holding a dangling view —
    // the id would read as garbage or crash.
    //
    // The fix stores the key by value (`std::string`), so the id must be
    // intact after the temporary dies.
    PluginRegistry& reg = PluginRegistry::instance();

    // The datachannel slot is used as the probe because it is the one
    // slot with a documented test-only removal helper
    // (`unregister_datachannel`, registry.hpp:626) — a probe that leaked
    // into later tests in the same process would be a false positive.
    // The factory pointer may be null; the *key* must still round-trip.
    RegisterUnderTemporaryString(reg);

    bool found = false;
    for (const std::string& id : reg.list_datachannels()) {
        if (id == kProbeId) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found)
        << "A registration id must survive the destruction of the "
           "std::string that carried it.  The registry stored a "
           "string_view into a temporary.";

    reg.unregister_datachannel(kProbeId);
}

// ---------------------------------------------------------------------------
// 4. A failed factory lookup must not yield a half-built stack.
// ---------------------------------------------------------------------------

namespace {

/// The transport-stack factory that actually composes ICE + DTLS + RTP +
/// SCTP — i.e. the one whose `create()` resolves every `StackConfig`
/// field and therefore has to fail loudly when one of them cannot be
/// resolved.
///
/// The registry also carries a "shell" factory (id="default") that
/// composes nothing; probing that one would prove nothing.  The real
/// factory is registered under "webrtc-classic".
const nimrtc::ITransportStackFactory* RealStackFactory() {
    PluginRegistry& reg = PluginRegistry::instance();
    const nimrtc::ITransportStackFactory* factory =
        reg.get_transport_stack("webrtc-classic");
    EXPECT_NE(nullptr, factory)
        << "The 'webrtc-classic' transport-stack factory must be "
           "registered; it is the one that resolves the StackConfig ids.";
    return factory;
}

} // namespace

TEST(RegistryContract, UnknownFactoryIdDoesNotYieldHalfBuiltStack) {
    RegisterAllDefaults();

    const nimrtc::ITransportStackFactory* factory = RealStackFactory();
    ASSERT_NE(nullptr, factory);

    // A config whose RTP id does not exist must fail *loudly* (null),
    // not produce a stack with a null RTP component.
    nimrtc::StackConfig cfg;
    cfg.rtp_id = "nimrtc_no_such_rtp_factory";
    cfg.sctp_id = "stub";

    std::unique_ptr<nimrtc::ITransportStack> stack =
        factory->create(cfg);

    ASSERT_EQ(nullptr, stack)
        << "create() returned a stack for an unresolvable rtp_id.  The "
           "factory must return nullptr so the engine's null-check is "
           "meaningful — otherwise the defect surfaces later as a null "
           "dereference.";
}

TEST(RegistryContract, UnknownSctpIdDoesNotYieldHalfBuiltStack) {
    RegisterAllDefaults();

    const nimrtc::ITransportStackFactory* factory = RealStackFactory();
    ASSERT_NE(nullptr, factory);

    nimrtc::StackConfig cfg;
    cfg.rtp_id = "webrtc";
    cfg.sctp_id = "nimrtc_no_such_sctp_factory";

    std::unique_ptr<nimrtc::ITransportStack> stack =
        factory->create(cfg);

    ASSERT_EQ(nullptr, stack)
        << "create() returned a stack for an unresolvable sctp_id.  The "
           "factory must return nullptr so the engine's null-check is "
           "meaningful.";
}

TEST(RegistryContract, UnknownIceIdDoesNotYieldHalfBuiltStack) {
    RegisterAllDefaults();

    const nimrtc::ITransportStackFactory* factory = RealStackFactory();
    ASSERT_NE(nullptr, factory);

    nimrtc::StackConfig cfg;
    cfg.ice_id = "nimrtc_no_such_ice_factory";

    std::unique_ptr<nimrtc::ITransportStack> stack =
        factory->create(cfg);

    ASSERT_EQ(nullptr, stack)
        << "create() returned a stack for an unresolvable ice_id.  The "
           "factory must return nullptr so the engine's null-check is "
           "meaningful.";
}

TEST(RegistryContract, UnknownDtlsIdDoesNotYieldHalfBuiltStack) {
    RegisterAllDefaults();

    const nimrtc::ITransportStackFactory* factory = RealStackFactory();
    ASSERT_NE(nullptr, factory);

    nimrtc::StackConfig cfg;
    cfg.dtls_id = "nimrtc_no_such_dtls_factory";

    std::unique_ptr<nimrtc::ITransportStack> stack =
        factory->create(cfg);

    ASSERT_EQ(nullptr, stack)
        << "create() returned a stack for an unresolvable dtls_id.  The "
           "factory must return nullptr so the engine's null-check is "
           "meaningful.";
}

TEST(RegistryContract, DefaultStackConfigBuildsCompleteStack) {
    RegisterAllDefaults();

    const nimrtc::ITransportStackFactory* factory = RealStackFactory();
    ASSERT_NE(nullptr, factory);

    // The whole point of fixing the defaults: the default config must
    // produce a stack whose components are all present.
    const nimrtc::StackConfig cfg;
    std::unique_ptr<nimrtc::ITransportStack> stack =
        factory->create(cfg);

    ASSERT_NE(nullptr, stack)
        << "The default StackConfig must build a stack.  It failed because "
           "the default ids do not match the registered ids.";
}
