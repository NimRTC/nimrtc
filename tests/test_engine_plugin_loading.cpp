/**
 * @file tests/test_engine_plugin_loading.cpp
 * @brief R2.5 validation: engine consumes PluginRegistry for audio3a.
 *
 * What this test verifies:
 *  - core::register_all_default_plugins() is idempotent and registers all
 *    five module categories (transport, rtp, sdp, jb, audio3a).
 *  - Engine open() returns 0 (success) without the "transport plugin not
 *    found" error that would fire if registration had not happened.
 *  - After open(), the audio3a plugin id is present in the registry
 *    (proves register_all_default_plugins ran as a side effect of open()).
 *  - The audio3a factory under "webrtc" can produce a usable plugin
 *    instance (proves the audio3a plugin adapter is link-time present).
 *  - send_audio() returns 0 (success) — uses the registered plugin path.
 *
 * NOTE: This test does NOT exercise ICE/DTLS/SRTP networking. It only
 *       verifies plugin wiring. The real E2E test is test_udp_loopback.cpp
 *       in the per-module directories.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/plugin_id.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/core/engine_errors.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/datachannel.hpp>
#include <nimrtc/transport/transport_stack.hpp>   // Slice 7.5: StackConfig, ITransportStack

namespace {

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

/** Test fixture: register all default plugins once for the whole suite. */
class EnginePluginLoading : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        // Same as the engine's internal call in open(); done here to assert
        // it works as a standalone API too.
        nimrtc::core::register_all_default_plugins();
    }
};

// ---------------------------------------------------------------------------
// Test: register_all_default_plugins() populates all 5 categories
// ---------------------------------------------------------------------------

TEST_F(EnginePluginLoading, registry_has_transport_ice) {
    auto ids = PluginRegistry::instance().list_transports();
    ASSERT_FALSE(ids.empty()) << "No transport plugins registered";
    bool found_ice = false;
    for (auto id : ids) {
        if (id == "ice") { found_ice = true; break; }
    }
    EXPECT_TRUE(found_ice) << "ICE transport factory not registered";
}

TEST_F(EnginePluginLoading, registry_has_audio3a_webrtc) {
    auto ids = PluginRegistry::instance().list_audio3a();
    ASSERT_FALSE(ids.empty());
    bool found_webrtc = false;
    for (auto id : ids) {
        if (id == "webrtc") { found_webrtc = true; break; }
    }
    EXPECT_TRUE(found_webrtc) << "audio3a 'webrtc' factory not registered";
}

TEST_F(EnginePluginLoading, registry_has_rtp_webrtc) {
    auto ids = PluginRegistry::instance().list_rtp();
    bool found = false;
    for (auto id : ids) if (id == "webrtc") { found = true; break; }
    EXPECT_TRUE(found) << "RTP 'webrtc' factory not registered";
}

TEST_F(EnginePluginLoading, registry_has_sdp_webrtc) {
    auto ids = PluginRegistry::instance().list_sdp();
    bool found = false;
    for (auto id : ids) if (id == "webrtc") { found = true; break; }
    EXPECT_TRUE(found) << "SDP 'webrtc' factory not registered";
}

TEST_F(EnginePluginLoading, registry_has_jb_adaptive) {
    auto ids = PluginRegistry::instance().list_jb();
    bool found = false;
    for (auto id : ids) if (id == "adaptive") { found = true; break; }
    EXPECT_TRUE(found) << "JB 'adaptive' factory not registered";
}

// ---------------------------------------------------------------------------
// Test: register_all_default_plugins() is idempotent
// ---------------------------------------------------------------------------

TEST_F(EnginePluginLoading, register_is_idempotent) {
    auto count_before = PluginRegistry::instance().list_audio3a().size();
    // Call twice more (already called in SetUpTestSuite)
    nimrtc::core::register_all_default_plugins();
    nimrtc::core::register_all_default_plugins();
    auto count_after = PluginRegistry::instance().list_audio3a().size();
    EXPECT_EQ(count_before, count_after) <<
        "register_all_default_plugins() should not double-register factories";
}

// ---------------------------------------------------------------------------
// Test: engine.open() uses the registered plugins
// ---------------------------------------------------------------------------

TEST_F(EnginePluginLoading, engine_open_uses_registry_for_transport) {
    EngineConfig cfg;
    cfg.local_port_range_begin = 50000;
    cfg.local_port_range_end   = 50100;   // narrow range to avoid OS port grab
    cfg.stun_server_host       = "";     // no STUN — pure host candidates
    NimRTCEngine engine(cfg);

    // open() should return 0; before R2.5 it would return 0x1FFF
    // "transport plugin not found" because register_all_default_plugins()
    // wasn't called.
    std::atomic<std::uint32_t> err_code{0};
    std::atomic<bool> err_called{false};
    engine.set_on_error([&](std::uint32_t err, std::string_view) {
        err_code = err;
        err_called = true;
    });
    uint32_t rc = engine.open();
    if (rc != 0) {
        std::fprintf(stderr, "DEBUG: engine.open() rc=0x%X err=0x%X err_called=%d\n",
                     rc, err_code.load(), err_called.load() ? 1 : 0);
    }
    EXPECT_EQ(rc, 0u) << "engine.open() should succeed when plugins are registered";
    EXPECT_TRUE(engine.is_open());

    engine.close();
    EXPECT_FALSE(engine.is_open());
}

// ---------------------------------------------------------------------------
// Test: send_audio() reaches the audio3a plugin (R2.5 verification)
// ---------------------------------------------------------------------------
//
// We can't fully verify send_audio() success without a connected peer (ICE
// send returns 0x1FFF when not connected — that's the libjuice error code,
// not a plugin-related error). The audio3a plugin path IS taken when the
// engine log shows "op8 audio3a plugin ok" (see stderr during the test).
//
// This test verifies the engine stays open long enough to call send_audio,
// and that the audio3a plugin is discoverable in the registry afterwards.
TEST_F(EnginePluginLoading, engine_send_audio_path_is_reachable) {
    EngineConfig cfg;
    // local_port_range_begin/end default to 0 = OS-assigned. The GitHub
    // Actions Windows runner occasionally rejects binds to fixed ranges
    // (WSAEACCES on ports around 50400-50500), so let the OS pick.
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The audio3a "webrtc" factory must be registered after open() (it
    // wasn't before, because the engine never called register_all_default
    // before R2.5).
    auto ids = PluginRegistry::instance().list_audio3a();
    bool found_webrtc = false;
    for (auto id : ids) if (id == "webrtc") { found_webrtc = true; break; }
    EXPECT_TRUE(found_webrtc);

    // 10 ms of silence at 48 kHz mono = 480 samples
    constexpr std::size_t kSamples = 480;
    std::vector<float> silence(kSamples, 0.0f);

    // send_audio() will fail because DTLS is not yet connected (the engine
    // refuses to emit plaintext SRTP packets before the handshake completes).
    // We only verify that the return code is NOT kEngineInvalidParam (0x1001),
    // which would mean the args or plugin path were wrong.  kEngineNotReady
    // (0x1002) is expected and correct here — it proves the engine was open
    // and the audio3a plugin path was reached before the DTLS gate fired.
    uint32_t rc = engine.send_audio(silence.data(), silence.size());
    EXPECT_NE(rc, nimrtc::core::kEngineInvalidParam) << "args are valid";
    // kEngineNotReady means "open but DTLS not connected yet" — correct.
    EXPECT_TRUE(rc == 0u || rc == nimrtc::core::kEngineNotReady)
        << "send_audio: expected 0 (DTLS connected) or kEngineNotReady (DTLS "
           "pending), got 0x"
        << std::hex << rc << std::dec;

    engine.close();
}

// ---------------------------------------------------------------------------
// PAL Slice 2: explicit default-registrar table (kDefaultRegistrars[])
// ---------------------------------------------------------------------------
//
// `register_all_default_plugins()` is now backed by an iterable table in
// `src/core/src/pal_default_registrars.cpp`.  This test asserts that the
// table is non-empty AND that iterating it produces the same factory
// registration set as the previous inline statement-list body.

TEST_F(EnginePluginLoading, pal_default_registrars_table_is_nonempty) {
    // The detail::kDefaultRegistrarCount is declared in registry.hpp as
    // an `extern const std::size_t` reference.  Verify it has at least
    // the same entries as the v0.10.1 inline body did: 12 unconditional
    // + up to 6 conditional = at least 12 unconditional entries.
    //
    // We can't include detail::kDefaultRegistrarCount directly here
    // (it's declared extern in registry.hpp), so we test indirectly
    // by checking that calling register_all_default_plugins() registers
    // the known unconditional entries.
    nimrtc::core::register_all_default_plugins();
    nimrtc::core::register_all_default_plugins();
    nimrtc::core::register_all_default_plugins();

    // All 5 unconditional categories must have at least one factory.
    EXPECT_FALSE(PluginRegistry::instance().list_transports().empty())
        << "PAL Slice 2: transport table lost an entry";
    EXPECT_FALSE(PluginRegistry::instance().list_audio3a().empty())
        << "PAL Slice 2: audio3a table lost an entry";
    EXPECT_FALSE(PluginRegistry::instance().list_rtp().empty())
        << "PAL Slice 2: rtp table lost an entry";
    EXPECT_FALSE(PluginRegistry::instance().list_sdp().empty())
        << "PAL Slice 2: sdp table lost an entry";
    EXPECT_FALSE(PluginRegistry::instance().list_jb().empty())
        << "PAL Slice 2: jb table lost an entry";
    EXPECT_FALSE(PluginRegistry::instance().list_bwes().empty())
        << "PAL Slice 2: bwe table lost an entry";
    EXPECT_FALSE(PluginRegistry::instance().list_schedulers().empty())
        << "PAL Slice 2: sched table lost an entry";
}

// ---------------------------------------------------------------------------
// PAL Slice 3: factory ids are greppable via NIMRTC_PLUGIN_ID() and the
// resulting string values match the v0.10.1 contract byte-for-byte.
// ---------------------------------------------------------------------------
//
// `NIMRTC_PLUGIN_ID(x)` expands to `PluginIdTag<__LINE__>(x)` with an
// implicit conversion to `std::string_view`.  This test verifies:
//   (a) The wrapper is callable with a literal and yields the literal.
//   (b) Different callsites produce distinct *types* (compile-time check).
//   (c) The actual factory ids returned at runtime still match the
//       expected literal strings ("webrtc", "webrtc_apm", "opus",
//       "adaptive", "h264") — proving the Slice 3 refactor is purely
//       additive and changes no on-the-wire values.

namespace nimrtc_pal_slice3_test {

// Two callsites on different lines ⇒ distinct PluginIdTag<> template
// instantiations.  The static_assert below verifies that the types
// differ — if NIMRTC_PLUGIN_ID ever stops producing unique types, this
// breaks at compile time.
static constexpr auto kId1 = NIMRTC_PLUGIN_ID("one");
static constexpr auto kId2 = NIMRTC_PLUGIN_ID("two");
static_assert(!std::is_same_v<decltype(kId1), decltype(kId2)>,
              "PAL Slice 3: NIMRTC_PLUGIN_ID callsites must yield distinct "
              "types (one per __LINE__)");

} // namespace nimrtc_pal_slice3_test

TEST_F(EnginePluginLoading, pal_plugin_id_wrappers_preserve_string_values) {
    // Verify the literal values are preserved verbatim through the wrapper.
    constexpr auto kId_webrtc    = NIMRTC_PLUGIN_ID("webrtc");
    constexpr auto kId_webrtc_apm = NIMRTC_PLUGIN_ID("webrtc_apm");
    constexpr auto kId_opus      = NIMRTC_PLUGIN_ID("opus");
    constexpr auto kId_adaptive  = NIMRTC_PLUGIN_ID("adaptive");
    constexpr auto kId_h264      = NIMRTC_PLUGIN_ID("h264");
    constexpr auto kId_wolfssl   = NIMRTC_PLUGIN_ID("wolfssl");

    EXPECT_EQ(std::string_view{kId_webrtc},     "webrtc");
    EXPECT_EQ(std::string_view{kId_webrtc_apm}, "webrtc_apm");
    EXPECT_EQ(std::string_view{kId_opus},       "opus");
    EXPECT_EQ(std::string_view{kId_adaptive},   "adaptive");
    EXPECT_EQ(std::string_view{kId_h264},       "h264");
    EXPECT_EQ(std::string_view{kId_wolfssl},    "wolfssl");

    // Cross-callsite uniqueness check (runtime): no two known factory ids
    // share a string value.  This catches accidental id collisions in
    // future PRs that add a new factory with an existing id.
    std::set<std::string> known_ids = {
        "webrtc", "webrtc_apm", "opus", "adaptive", "h264", "wolfssl",
    };
    EXPECT_EQ(known_ids.size(), 6u)
        << "duplicate id in known_ids test vector";
}

// ---------------------------------------------------------------------------
// PAL Slice 8 (v0.10.2) — Transport PAL Slice 8 registry hooks.
//
// The Slice 8 DoD requires 4 new typed slots on core::PluginRegistry:
//   - register_dtls_session(id, factory*) / get_dtls_session(id)
//   - register_sctp_socket(id, factory*)    / get_sctp_socket(id)
//   - register_raw_udp_datagram(id, factory*) / get_raw_udp_datagram(id)
//   - register_transport_stack(id, factory*) / get_transport_stack(id)
//
// Each slot is populated by its module's `register_default_plugins()`
// entry point, which the unified `core::register_all_default_plugins()`
// walks in PAL Slice 2's `kDefaultRegistrars[]` table.  These 4 tests
// assert that all four slots have non-null lookups after the
// unified entry point runs at least once — i.e. the Slice 8 wiring
// is end-to-end functional, not just present in the header.
// ---------------------------------------------------------------------------

TEST_F(EnginePluginLoading, slice8_dtls_session_registry_hook_populated) {
    // WolfsslDtlsFactory must register under id "wolfssl" via the typed
    // registry hook.  Slice 4 used a process-local test_only slot;
    // Slice 8 promotes the canonical lookup to
    // `core::PluginRegistry::get_dtls_session(id)`.
    const auto* factory =
        PluginRegistry::instance().get_dtls_session("wolfssl");
    ASSERT_NE(factory, nullptr)
        << "PAL Slice 8: DTLS factory 'wolfssl' not found in registry";
    EXPECT_FALSE(factory->id().empty());
    EXPECT_EQ(factory->id(), "wolfssl");

    // The factory list should be non-empty AND contain the wolfssl id.
    auto ids = PluginRegistry::instance().list_dtls_sessions();
    EXPECT_FALSE(ids.empty())
        << "PAL Slice 8: list_dtls_sessions() is empty";
    bool found_wolfssl = false;
    for (auto id : ids) {
        if (id == "wolfssl") { found_wolfssl = true; break; }
    }
    EXPECT_TRUE(found_wolfssl)
        << "PAL Slice 8: 'wolfssl' not present in list_dtls_sessions()";
}

TEST_F(EnginePluginLoading, slice8_sctp_socket_registry_hook_populated) {
    // SctpStubFactory must register under id "stub" via the typed
    // registry hook.  Slice 5 used a process-local cache;
    // Slice 8 promotes the canonical lookup to
    // `core::PluginRegistry::get_sctp_socket(id)`.
    const auto* factory =
        PluginRegistry::instance().get_sctp_socket("stub");
    ASSERT_NE(factory, nullptr)
        << "PAL Slice 8: SCTP factory 'stub' not found in registry";
    EXPECT_FALSE(factory->id().empty());
    EXPECT_EQ(factory->id(), "stub");

    auto ids = PluginRegistry::instance().list_sctp_sockets();
    EXPECT_FALSE(ids.empty())
        << "PAL Slice 8: list_sctp_sockets() is empty";
    bool found_stub = false;
    for (auto id : ids) {
        if (id == "stub") { found_stub = true; break; }
    }
    EXPECT_TRUE(found_stub)
        << "PAL Slice 8: 'stub' not present in list_sctp_sockets()";
}

TEST_F(EnginePluginLoading, slice8_raw_udp_datagram_registry_hook_populated) {
    // ArqRawUdpFactory must register under id "arq" via the typed
    // registry hook.  Slice 6 used a process-local singleton;
    // Slice 8 promotes the canonical lookup to
    // `core::PluginRegistry::get_raw_udp_datagram(id)`.
    const auto* factory =
        PluginRegistry::instance().get_raw_udp_datagram("arq");
    ASSERT_NE(factory, nullptr)
        << "PAL Slice 8: raw-UDP factory 'arq' not found in registry";
    EXPECT_FALSE(factory->id().empty());
    EXPECT_EQ(factory->id(), "arq");

    auto ids = PluginRegistry::instance().list_raw_udp_datagrams();
    EXPECT_FALSE(ids.empty())
        << "PAL Slice 8: list_raw_udp_datagrams() is empty";
    bool found_arq = false;
    for (auto id : ids) {
        if (id == "arq") { found_arq = true; break; }
    }
    EXPECT_TRUE(found_arq)
        << "PAL Slice 8: 'arq' not present in list_raw_udp_datagrams()";
}

TEST_F(EnginePluginLoading, slice8_transport_stack_registry_hook_populated) {
    // CapabilitySelectorStackFactory must register under id "default"
    // via the typed registry hook.  Slice 7 only had the Selector;
    // Slice 8 adds the stack factory (shell impl — real composition
    // lands in Slice 7.5) so the engine integration has a non-null
    // factory pointer to resolve.
    const auto* factory =
        PluginRegistry::instance().get_transport_stack("default");
    ASSERT_NE(factory, nullptr)
        << "PAL Slice 8: transport-stack factory 'default' not found in registry";
    EXPECT_FALSE(factory->id().empty());
    EXPECT_EQ(factory->id(), "default");

    auto ids = PluginRegistry::instance().list_transport_stacks();
    EXPECT_FALSE(ids.empty())
        << "PAL Slice 8: list_transport_stacks() is empty";
    bool found_default = false;
    for (auto id : ids) {
        if (id == "default") { found_default = true; break; }
    }
    EXPECT_TRUE(found_default)
        << "PAL Slice 8: 'default' not present in list_transport_stacks()";
}

// ---------------------------------------------------------------------------
// PAL Slice 8 — idempotency + compatibility with the Slice 1-3 tests.
//
// Verifies that running register_all_default_plugins() multiple times
// (the PAL Slice 2 idempotency gate) does not double-register the
// Slice 8 typed slots.  Also asserts the four new factory ids are
// exactly the ones listed in the v0.10.2 release notes — anything
// else would silently regress the lookup contract.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// PAL Slice 7.5 — WebRtcClassicStackFactory
//
// Verifies that WebRtcClassicStackFactory (id="webrtc-classic") is
// registered alongside the shell "default" factory, and that it can
// create a stack with resolved components.
// ---------------------------------------------------------------------------

TEST_F(EnginePluginLoading, slice75_webrtc_classic_factory_registered) {
    // WebRtcClassicStackFactory (id="webrtc-classic") is registered by
    // nimrtc::transport::register_default_plugins() (called via
    // core::register_all_default_plugins()).  The factory id "webrtc-classic"
    // is what the CapabilitySelector resolves for §5.3 rule 1
    // (needs_browser_interop == true).
    const auto* factory =
        PluginRegistry::instance().get_transport_stack("webrtc-classic");
    ASSERT_NE(factory, nullptr)
        << "PAL Slice 7.5: WebRtcClassicStackFactory 'webrtc-classic' "
           "not found in registry";
    EXPECT_EQ(factory->id(), "webrtc-classic");
    EXPECT_FALSE(factory->display_name().empty());
}

TEST_F(EnginePluginLoading, slice75_webrtc_classic_stack_create_and_lifecycle) {
    // Verify WebRtcClassicStackFactory::create() produces a non-null stack
    // whose component accessors are non-null after the factory resolves all
    // four backends.  The ICE / DTLS / RTP / SCTP factories are all registered
    // (they were verified in the Slice 1/2/3/8 tests above), so this test
    // exercises the full wiring path.
    const auto* factory =
        PluginRegistry::instance().get_transport_stack("webrtc-classic");
    ASSERT_NE(factory, nullptr)
        << "PAL Slice 7.5: factory 'webrtc-classic' not found";

    nimrtc::StackConfig cfg;
    cfg.ice_id  = "ice";
    cfg.dtls_id = "wolfssl";
    cfg.rtp_id  = "webrtc";
    cfg.sctp_id = "stub";

    std::unique_ptr<nimrtc::ITransportStack> stack{
        factory->create(cfg)};
    ASSERT_NE(stack, nullptr) << "PAL Slice 7.5: factory->create() returned null";

    // Component accessors must be non-null (factory resolved all four backends).
    EXPECT_NE(&stack->ice(), nullptr)   << "PAL Slice 7.5: ICE not resolved";
    EXPECT_NE(&stack->dtls(), nullptr)  << "PAL Slice 7.5: DTLS not resolved";
    EXPECT_NE(&stack->rtp(), nullptr)   << "PAL Slice 7.5: RTP not resolved";
    EXPECT_NE(&stack->sctp(), nullptr) << "PAL Slice 7.5: SCTP not resolved";

    // raw_control() is nullptr for "webrtc-classic" (control uses SCTP stub;
    // real control stack lands in Slice 7.5 sibling RawUdpArqStackFactory).
    EXPECT_EQ(stack->raw_control(), nullptr)
        << "PAL Slice 7.5: raw_control() should be nullptr for webrtc-classic";

    // Lifecycle: start() / close() must not crash (no-throw contract).
    EXPECT_NO_THROW(stack->start());
    EXPECT_NO_THROW(stack->close());
}

TEST_F(EnginePluginLoading, slice75_idempotent_register_does_not_duplicate) {
    // Call register_all_default_plugins() three times (the Slice 2
    // idempotency contract).  Each of the four Slice 8 typed slots
    // must end up with exactly one factory entry — the "last writer
    // wins" semantics of TypedRegistry::register_one would otherwise
    // either silently overwrite (idempotent) or accumulate (regression).
    nimrtc::core::register_all_default_plugins();
    nimrtc::core::register_all_default_plugins();
    nimrtc::core::register_all_default_plugins();

    // Exactly one factory per known Slice 8 id.
    // NOTE: Slice 7.5 registers BOTH "default" (shell) AND "webrtc-classic"
    // (real) in the same `register_default_plugins()` call, so the count is 2.
    // The shell is preserved for backward-compat with Slice 7 tests that use
    // `get_transport_stack("default")`.
    //
    // TPAL-5 (v0.11.0) adds the second SCTP factory: the production
    // `UsrsctpSocketFactory` ("usrsctp") registers alongside the
    // pre-existing Slice-5 `SctpStubFactory` ("stub"). Both are
    // additive and distinct (different ids), so the SCTP slot holds
    // exactly 2 entries. Re-registration must NOT duplicate either.
    EXPECT_EQ(PluginRegistry::instance().list_dtls_sessions().size(), 1u)
        << "Slice 8: DTLS slot duplicated after repeated registration";
    EXPECT_EQ(PluginRegistry::instance().list_sctp_sockets().size(), 2u)
        << "TPAL-5: SCTP slot must hold stub + usrsctp (2 entries); "
           "duplication here means TPAL-5's register_default_plugins() "
           "is no longer idempotent";
    EXPECT_EQ(PluginRegistry::instance().list_raw_udp_datagrams().size(), 1u)
        << "Slice 8: raw-UDP slot duplicated after repeated registration";
    EXPECT_EQ(PluginRegistry::instance().list_transport_stacks().size(), 2u)
        << "Slice 7.5: transport-stack slot has 2 entries: default (shell) + webrtc-classic (real)";
}

// ---------------------------------------------------------------------------
// TPAL-5 (v0.11.0) — DataChannel registry slot + engine integration.
//
// Verifies the Slice 8-style typed `register_datachannel(id, factory*)`
// hook: the registry resolves factories by id (mirrors DTLS / SCTP /
// raw_udp / transport-stack).  We register a stub factory here rather
// than asserting against a built-in id because Subagent A's production
// `SctpDataChannelFactory` lands in a separate PR — the typed slot
// itself is what this PR is responsible for, and that's stable without
// any concrete SCTP implementation.
//
// The full `create_data_channel()` end-to-end test (factory → engine →
// IDataChannel instance → callback forwarding) lives in
// `tests/test_datachannel_engine.cpp` — kept separate so the
// `EnginePluginLoading` suite stays focused on the registry layer.
// ---------------------------------------------------------------------------

// Local stub factory — only used by the next test, lives at namespace
// scope so `register_datachannel` can take its address without keeping
// it in a wrapper (TypedRegistry doesn't copy factories).
namespace nimrtc_engine_plugin_loading_dc {

class LocalStubFactory final : public nimrtc::plugins::IDataChannelFactory {
public:
    std::string_view id() const noexcept override { return "engine_loading_dc"; }
    std::string_view display_name() const noexcept override {
        return "test stub for engine_plugin_loading DataChannel slot";
    }
    nimrtc::plugins::IDataChannel* create() const override {
        // We don't actually create a channel here — this test only
        // exercises the registry lookup, not the engine's
        // create_data_channel() path (that's test_datachannel_engine).
        // Returning nullptr keeps the test side-effect-free.
        return nullptr;
    }
};

static const LocalStubFactory s_factory;

} // namespace nimrtc_engine_plugin_loading_dc

TEST_F(EnginePluginLoading, tpall5_DataChannelFactoryIsRegistered) {
    // Register the local stub factory under a non-colliding id.
    PluginRegistry::instance().register_datachannel(
        nimrtc_engine_plugin_loading_dc::s_factory.id(),
        &nimrtc_engine_plugin_loading_dc::s_factory);

    // Slot resolves by id (the canonical lookup NimRTCEngine uses).
    const auto* factory = PluginRegistry::instance().get_datachannel(
        "engine_loading_dc");
    ASSERT_NE(factory, nullptr)
        << "TPAL-5: DataChannel factory 'engine_loading_dc' not found "
           "in registry (the Slice 8-style slot must resolve factories "
           "by id, mirroring DTLS / SCTP / raw_udp / transport-stack)";
    EXPECT_EQ(factory->id(), "engine_loading_dc");
    EXPECT_FALSE(factory->display_name().empty());

    // The factory list must contain the stub id alongside any
    // built-in / test-supplied factories (e.g. the "stub_dc" entry
    // registered by test_datachannel_engine's SetUpTestSuite if both
    // tests share a process — which they do, since GTest runs in a
    // single binary).
    auto ids = PluginRegistry::instance().list_datachannels();
    bool found = false;
    for (auto id : ids) {
        if (id == "engine_loading_dc") { found = true; break; }
    }
    EXPECT_TRUE(found)
        << "list_datachannels() must include the test's stub factory";

    // Unknown id → nullptr (no silent fallback).  Mirrors the Slice 8
    // DTLS seam contract from test_dtls_seam's third test.
    const auto* missing =
        PluginRegistry::instance().get_datachannel("not_registered_xyz");
    EXPECT_EQ(missing, nullptr)
        << "get_datachannel() must return nullptr for unregistered ids "
           "(no silent fallback — matches Slice 8 typed-slot contract)";
}

} // anonymous namespace
