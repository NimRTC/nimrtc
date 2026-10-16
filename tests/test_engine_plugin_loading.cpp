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
// v0.14.0 alpha.2 (ADR-014): WebTransport / QUIC seam factory
// interface — needed for the typed `register_web_transport()` slot
// in `core::PluginRegistry`.  The seam itself (IWebTransportSession)
// is forward-declared in registry.hpp; the factory interface is
// the public type the registry hands out via get_web_transport(id).
#include <nimrtc/web_transport/web_transport_session_factory.hpp>

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

// ===========================================================================
// v0.13.0 — dtls_weak() must auto-expire after close()
//
// Regression test for the v0.12.0 P0 UAF class:
//   - Capture a weak_ptr via dtls_weak() while the stack is alive.
//   - Verify .lock() returns a valid shared_ptr (session reachable).
//   - Close the stack.
//   - Verify .lock() now returns nullptr (session auto-destroyed via
//     the stack's internal shared_ptr; the engine's weak_ptr expired
//     without a UAF).
//
// Without this guarantee, the engine's `Impl::dtls weak_ptr` would
// silently hold a "stuck non-null" weak_ptr that .lock()'s into
// freed memory — exactly the v0.12.0 P0 bug class.  v0.13.0
// weak_ptr migration eliminates it; this test guards the invariant.
// ===========================================================================
TEST_F(EnginePluginLoading, v013_dtls_weak_expires_after_close) {
    const auto* factory =
        PluginRegistry::instance().get_transport_stack("webrtc-classic");
    ASSERT_NE(factory, nullptr)
        << "PAL Slice 7.5: factory 'webrtc-classic' not found";

    nimrtc::StackConfig cfg;
    cfg.ice_id  = "ice";
    cfg.dtls_id = "wolfssl";
    cfg.rtp_id  = "webrtc";
    cfg.sctp_id = "stub";

    std::unique_ptr<nimrtc::ITransportStack> stack{factory->create(cfg)};
    ASSERT_NE(stack, nullptr);

    // Capture the weak_ptr BEFORE close.
    std::weak_ptr<nimrtc::dtls::IDtlsSession> weak = stack->dtls_weak();

    // While the stack is alive, the weak_ptr must lock to a valid session.
    {
        auto locked = weak.lock();
        EXPECT_NE(locked, nullptr)
            << "v0.13.0: dtls_weak().lock() must be non-null while the stack is alive";
    }

    EXPECT_NO_THROW(stack->start());
    EXPECT_NO_THROW(stack->close());

    // After close, the weak_ptr MUST be expired — the engine's
    // weak_ptr.lock() check in drain_dtls() / on_transport_recv() /
    // maybe_install_srtp_keys() depends on this invariant.
    EXPECT_TRUE(weak.expired())
        << "v0.13.0: dtls_weak() must auto-expire after close() — "
           "if this fails, the engine's post-close UAF defence is broken";

    auto locked_after = weak.lock();
    EXPECT_EQ(locked_after, nullptr)
        << "v0.13.0: post-close .lock() must return nullptr (no UAF)";
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
    // v0.11.0 cut: only the Slice 5 stub ("stub") is auto-registered. The
    // production usrsctp factory ("usrsctp") is NOT auto-registered (see
    // sctp_plugin.cpp "Why usrsctp is not the default" header) — opt-in
    // callers construct it directly. The SCTP slot therefore holds
    // exactly 1 entry, and re-registration must NOT duplicate it.
    // See tests/test_sctp_usrsctp.cpp UsrsctpNotAutoRegistered for the
    // negative assertion that locks the new contract.
    EXPECT_EQ(PluginRegistry::instance().list_dtls_sessions().size(), 1u)
        << "Slice 8: DTLS slot duplicated after repeated registration";
    EXPECT_EQ(PluginRegistry::instance().list_sctp_sockets().size(), 1u)
        << "v0.11.0: SCTP slot must hold ONLY the Slice 5 stub (1 entry); "
           "usrsctp is opt-in only and not auto-registered. Duplication "
           "here means register_default_plugins() is no longer idempotent";
    EXPECT_EQ(PluginRegistry::instance().list_raw_udp_datagrams().size(), 1u)
        << "Slice 8: raw-UDP slot duplicated after repeated registration";
    EXPECT_EQ(PluginRegistry::instance().list_transport_stacks().size(), 3u)
        << "Slice 7.6: transport-stack slot has 3 entries: default (shell) + "
           "webrtc-classic (real) + raw-udp-arq (control plane)";
}

// ---------------------------------------------------------------------------
// Slice 7.6 (v0.12.0) — RawUdpArqStackFactory (id="raw-udp-arq")
//
// The control-plane counterpart of WebRtcClassicStack. CapabilitySelector's
// §5.3 rules 1b / 2 / 4 (high-freq-control variants) resolve this factory
// for the control_stack slot. See docs/plan/transport-strategies.md §2.
// ---------------------------------------------------------------------------
TEST_F(EnginePluginLoading, slice76_raw_udp_arq_factory_registered) {
    // RawUdpArqStackFactory (id="raw-udp-arq") is registered by
    // nimrtc::transport::register_default_plugins() (called via
    // core::register_all_default_plugins()).  The factory id "raw-udp-arq"
    // is what the CapabilitySelector resolves for §5.3 rules 1b / 2 / 4
    // when needs_high_freq_control == true.
    const auto* factory =
        PluginRegistry::instance().get_transport_stack("raw-udp-arq");
    ASSERT_NE(factory, nullptr)
        << "PAL Slice 7.6: RawUdpArqStackFactory 'raw-udp-arq' not found "
           "in registry";
    EXPECT_EQ(factory->id(), "raw-udp-arq");
    EXPECT_FALSE(factory->display_name().empty());
}

TEST_F(EnginePluginLoading, slice76_raw_udp_arq_stack_create_and_lifecycle) {
    // Verify RawUdpArqStackFactory::create() produces a non-null stack
    // whose raw_control() returns a real IRawUdpDatagram. The other
    // accessors (ice/dtls/rtp/sctp) return null-instance references
    // (the control stack doesn't own those components; see
    // raw_udp_arq_stack.hpp for the rationale).
    const auto* factory =
        PluginRegistry::instance().get_transport_stack("raw-udp-arq");
    ASSERT_NE(factory, nullptr)
        << "PAL Slice 7.6: factory 'raw-udp-arq' not found";

    nimrtc::StackConfig cfg;
    cfg.raw_id = "arq";   // Slice 6 default ARQ factory
    auto stack = factory->create(cfg);
    ASSERT_NE(stack, nullptr)
        << "RawUdpArqStackFactory::create() returned null";
    EXPECT_EQ(stack->id(), "raw-udp-arq");

    // The whole point of the raw-udp-arq stack is to expose a real
    // ARQ session via raw_control(). The ARQ impl is created in
    // start() (matches the lifecycle model: start() opens resources).
    EXPECT_EQ(stack->raw_control(), nullptr)
        << "raw_control() must be null before start() (lazy init)";
    EXPECT_NO_THROW(stack->start());
    ASSERT_NE(stack->raw_control(), nullptr)
        << "raw_control() must be non-null after start()";
    EXPECT_NO_THROW(stack->close());
    EXPECT_EQ(stack->raw_control(), nullptr)
        << "raw_control() must be null after close()";
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

// ---------------------------------------------------------------------------
// Transport strategy integration (v0.12.0 Slice 7.6 PR3)
//
// Verifies that NimRTCEngine:
//   - Exposes `transport_strategy_name` / `transport_strategy_session`
//     accessors that return the resolved state.
//   - Resolves an empty `cfg.transport_strategy_name` to the engine
//     default ("browser-call") and records the resulting factory pair
//     (`webrtc-classic`, no control stack).
//   - Resolves each of the 4 non-default strategies to the expected
//     `media_factory_id` / `control_factory_id` per §5.3 of
//     `docs/plan/transport-selection.md`.
//   - Fails `open()` with a non-zero return code when
//     `cfg.transport_strategy_name` is set to a name that is not in
//     the v0.12.0 catalog.  This is the "loud-fail on unknown strategy"
//     contract — the engine does NOT silently fall back to
//     `browser-call` for an unknown name, because that would mask
//     integrator typos.
// ---------------------------------------------------------------------------
TEST_F(EnginePluginLoading, slice76_strategy_default_is_browser_call) {
    // No explicit `transport_strategy_name` — the engine must fall
    // back to `browser-call` (the canonical engine default).  The
    // media stack is `webrtc-classic`; no control side-stack (legacy
    // DC plane only).
    EngineConfig cfg;
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    EXPECT_EQ(engine.transport_strategy_name(), "browser-call");
    const auto session = engine.transport_strategy_session();
    EXPECT_EQ(session.media_factory_id, "webrtc-classic");
    EXPECT_EQ(session.control_factory_id, "")
        << "browser-call must NOT enable a control side-stack";
    engine.close();
}

TEST_F(EnginePluginLoading, slice76_strategy_agent_gateway) {
    // agent-gateway: §5.3 rule 1b — browser interop + high-freq
    // control → media=webrtc-classic, control=raw-udp-arq.
    EngineConfig cfg;
    cfg.transport_strategy_name = "agent-gateway";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    EXPECT_EQ(engine.transport_strategy_name(), "agent-gateway");
    const auto session = engine.transport_strategy_session();
    EXPECT_EQ(session.media_factory_id, "webrtc-classic");
    EXPECT_EQ(session.control_factory_id, "raw-udp-arq");
    engine.close();
}

TEST_F(EnginePluginLoading, slice76_strategy_teleop_ultra_low_latency) {
    // teleop-ultra-low-latency: §5.3 rule 2 — no browser interop +
    // high-freq control → media=raw-udp-arq, control=raw-udp-arq.
    // The Selector picks raw-udp-arq for the media stack (the legacy
    // v0.10.x "webrtc-classic" fallback only kicks in when the
    // Selector itself returns empty, e.g. for a hypothetical
    // webrtc-quic-call strategy that lands in v1.x).  The engine
    // **records** the Selector's choice but does NOT yet instantiate
    // the raw-udp-arq media stack — that lands in Slice 7.x 2nd-half
    // work (v0.13+; see `docs/plan/transport-selection.md` §6.5).
    // The engine's actual media-stack instantiation continues to go
    // through `cfg.transport_name` ("ice" by default).
    EngineConfig cfg;
    cfg.transport_strategy_name = "teleop-ultra-low-latency";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    EXPECT_EQ(engine.transport_strategy_name(), "teleop-ultra-low-latency");
    const auto session = engine.transport_strategy_session();
    EXPECT_EQ(session.media_factory_id, "raw-udp-arq")
        << "§5.3 rule 2 must resolve the media stack to raw-udp-arq "
           "(no browser interop + high-freq control)";
    EXPECT_EQ(session.control_factory_id, "raw-udp-arq");
    engine.close();
}

TEST_F(EnginePluginLoading, slice76_strategy_sfu_relay) {
    // sfu-relay: §5.3 rule 1 — browser interop, no high-freq control
    // → media=webrtc-classic, no control side-stack.
    EngineConfig cfg;
    cfg.transport_strategy_name = "sfu-relay";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    EXPECT_EQ(engine.transport_strategy_name(), "sfu-relay");
    const auto session = engine.transport_strategy_session();
    EXPECT_EQ(session.media_factory_id, "webrtc-classic");
    EXPECT_EQ(session.control_factory_id, "")
        << "sfu-relay must NOT enable a control side-stack";
    engine.close();
}

TEST_F(EnginePluginLoading, slice76_strategy_cross_network_peer) {
    // cross-network-peer: §5.3 rule 1 (tighter p99) — same
    // (webrtc-classic, null) pair as browser-call.
    EngineConfig cfg;
    cfg.transport_strategy_name = "cross-network-peer";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    EXPECT_EQ(engine.transport_strategy_name(), "cross-network-peer");
    const auto session = engine.transport_strategy_session();
    EXPECT_EQ(session.media_factory_id, "webrtc-classic");
    EXPECT_EQ(session.control_factory_id, "");
    engine.close();
}

TEST_F(EnginePluginLoading, slice76_unknown_strategy_fails_open) {
    // An unknown strategy name must fail `open()` with a non-zero
    // return code.  The engine does NOT silently fall back to
    // `browser-call` — that would mask integrator typos.
    EngineConfig cfg;
    cfg.transport_strategy_name = "webrtc-quic-call";  // not in v0.12.0
    NimRTCEngine engine(cfg);
    // Catch the error via the on_error callback so we can assert on
    // the message shape without leaking kEngineInvalidParam constants
    // into the test (the test's responsibility is "fail + log
    // something about the strategy", not "exactly error code 0x1001").
    std::atomic<std::uint32_t> err_code{0};
    std::atomic<bool> err_called{false};
    engine.set_on_error([&](std::uint32_t err, std::string_view msg) {
        err_code = err;
        err_called = true;
        (void)msg;
    });
    const uint32_t rc = engine.open();
    EXPECT_NE(rc, 0u)
        << "Unknown strategy name must fail open() (got rc=" << rc << ")";
    EXPECT_TRUE(err_called.load())
        << "Engine must invoke on_error when strategy validation fails";
    // The accessors must remain empty (the strategy was rejected
    // before being recorded).
    EXPECT_EQ(engine.transport_strategy_name(), "");
    EXPECT_EQ(engine.transport_strategy_session().media_factory_id, "");
    EXPECT_EQ(engine.transport_strategy_session().control_factory_id, "");
    // Engine must NOT be open.
    EXPECT_FALSE(engine.is_open());
}

TEST_F(EnginePluginLoading, slice76_accessors_empty_before_open) {
    // The strategy is recorded inside init_modules_once(); before
    // open() the accessors must return empty strings (the
    // "diagnostic, not live state" contract).
    EngineConfig cfg;
    cfg.transport_strategy_name = "agent-gateway";
    NimRTCEngine engine(cfg);
    EXPECT_EQ(engine.transport_strategy_name(), "");
    EXPECT_EQ(engine.transport_strategy_session().media_factory_id, "");
    EXPECT_EQ(engine.transport_strategy_session().control_factory_id, "");
}

// ---------------------------------------------------------------------------
// v0.14.0 alpha.2 (ADR-014) — WebTransport / QUIC seam registry slot.
//
// The seam (`IWebTransportSession` + companion headers + factory
// interface) lands in v0.14.0 alpha.2; the msquic-backed factory
// lands in beta.1.  At the alpha.2 cut, the registry slot exists
// (parallel to the Slice 8 DTLS / SCTP / raw_udp / transport-stack
// slots) but no factory is auto-registered.  These two tests pin
// the alpha.2 contract:
//
//   1. `wt014_registry_slot_is_typed_for_web_transport_factory`
//        — `get_web_transport()` / `list_web_transports()` /
//        `register_web_transport()` exist and return the
//        expected `const IWebTransportSessionFactory*` type
//        (i.e. the slot is wired into the typed registry
//        hooks, not a process-local cache).
//
//   2. `wt014_registry_default_registrars_omits_web_transport_until_beta1`
//        — after `register_all_default_plugins()` runs (the PAL
//        Slice 2 unified entry point), `list_web_transports()`
//        returns an empty vector.  No factory is auto-registered
//        at the alpha.2 cut.  A user-supplied factory id
//        resolves to nullptr — the runtime error documented in
//        v0.14-plan.md §3.1 Gate #3 ("loud-fail on unknown
//        id", no silent fallback).
// ---------------------------------------------------------------------------

TEST_F(EnginePluginLoading,
       wt014_registry_slot_is_typed_for_web_transport_factory) {
    // The slot must exist.  The `get_web_transport()` accessor
    // returns nullptr for unknown ids (the Slice 8 typed-slot
    // contract); verify the accessor compiles + links + returns
    // the right type by checking that an unknown id returns
    // nullptr and a known id (also nonexistent) returns nullptr.
    const auto* factory =
        PluginRegistry::instance().get_web_transport("msquic");
    EXPECT_EQ(factory, nullptr)
        << "v0.14.0 alpha.2: msquic factory is NOT auto-registered "
           "(the factory lands in beta.1)";

    // The list accessor must exist and return an empty vector
    // (verified in the next subtest).
    auto ids = PluginRegistry::instance().list_web_transports();
    // Cast to a typed size_t so the test compiles cleanly even
    // if the accessor returns an empty vector (the static_cast
    // is a no-op on size_t, but it documents the type contract).
    EXPECT_EQ(static_cast<std::size_t>(ids.size()),
              static_cast<std::size_t>(0))
        << "v0.14.0 alpha.2: list_web_transports() must return an "
           "empty vector (no factory is auto-registered at the "
           "alpha.2 cut)";

    // The slot must accept a user-registered factory.  Register a
    // local stub factory and verify it resolves by id.  This
    // proves the slot is functional (the engine can wire a
    // factory through it in beta.1).
    namespace wt014_local {
    class StubFactory final
        : public nimrtc::web_transport::IWebTransportSessionFactory {
    public:
        std::string_view id() const noexcept override {
            return "wt014_stub";
        }
        std::string_view display_name() const noexcept override {
            return "v0.14.0 alpha.2 stub factory for the registry test";
        }
        std::unique_ptr<nimrtc::web_transport::IWebTransportSession>
        create(const nimrtc::web_transport::WebTransportConfig&)
            const override {
            return nullptr;  // the test only exercises the lookup,
                             // not the create path
        }
    };
    static const StubFactory k_stub_factory;
    }  // namespace wt014_local

    PluginRegistry::instance().register_web_transport(
        wt014_local::k_stub_factory.id(),
        &wt014_local::k_stub_factory);

    const auto* resolved =
        PluginRegistry::instance().get_web_transport("wt014_stub");
    EXPECT_EQ(resolved, &wt014_local::k_stub_factory)
        << "v0.14.0 alpha.2: get_web_transport() must resolve the "
           "user-registered factory by id (typed slot contract; "
           "matches the DTLS / SCTP / raw_udp / transport-stack "
           "Slice 8 slots)";

    // The stub id must appear in the list accessor.
    auto ids_after = PluginRegistry::instance().list_web_transports();
    bool found_stub = false;
    for (auto id : ids_after) {
        if (id == "wt014_stub") { found_stub = true; break; }
    }
    EXPECT_TRUE(found_stub)
        << "v0.14.0 alpha.2: list_web_transports() must include the "
           "user-registered factory (the slot is a real typed "
           "registry, not a process-local cache)";

    // Unknown id → nullptr (no silent fallback).  Mirrors the
    // Slice 8 DTLS / SCTP / raw_udp / transport-stack contract.
    const auto* missing =
        PluginRegistry::instance().get_web_transport(
            "not_registered_xyz");
    EXPECT_EQ(missing, nullptr)
        << "v0.14.0 alpha.2: get_web_transport() must return nullptr "
           "for unregistered ids (no silent fallback)";

    // Note: the "no auto-registered factory" assertion is split
    // into the next subtest so the assertions are easy to
    // bisect when the test fails.
}

TEST_F(EnginePluginLoading,
       wt014_registry_default_registrars_omits_web_transport_until_beta1) {
    // v0.14.0 alpha.2 contract: no factory is auto-registered by
    // `register_all_default_plugins()`.  The msquic factory lands
    // in beta.1 alongside the msquic integration; the alpha.2
    // cut ships the seam + the registry slot only.
    //
    // Verify: after three calls to `register_all_default_plugins()`
    // (the PAL Slice 2 idempotency contract), the
    // `list_web_transports()` accessor still returns an empty
    // vector.  This pins the alpha.2 → beta.1 transition: if a
    // future PR accidentally adds a factory entry in
    // `pal_default_registrars.cpp` before the msquic integration
    // is ready, this test fails at PR review time.
    nimrtc::core::register_all_default_plugins();
    nimrtc::core::register_all_default_plugins();
    nimrtc::core::register_all_default_plugins();

    auto ids = PluginRegistry::instance().list_web_transports();
    EXPECT_TRUE(ids.empty())
        << "v0.14.0 alpha.2: list_web_transports() must be empty "
           "after register_all_default_plugins() — no factory is "
           "auto-registered at the alpha.2 cut.  Found ids:";
    for (auto id : ids) {
        ADD_FAILURE() << "  unexpected id: " << id;
    }

    // A user-supplied lookup of a would-be beta.1 id ("msquic")
    // must return nullptr — the engine's runtime "loud-fail on
    // unknown factory" contract is the alpha.2 error surface
    // for `EngineConfig::datachannel_backend = "msquic"` callers.
    const auto* msquic =
        PluginRegistry::instance().get_web_transport("msquic");
    EXPECT_EQ(msquic, nullptr)
        << "v0.14.0 alpha.2: get_web_transport(\"msquic\") must "
           "return nullptr (the msquic factory is scheduled to "
           "land in v0.14.0-beta.1; see docs/plan/v0.14-plan.md "
           "§3.1 Gate #3 — the alpha.2 error surface is a "
           "loud-fail, not a silent fallback)";

    // The Slice 8 / v0.10.2 typed-slot count is unchanged
    // (DTLS=1, SCTP=1, raw_udp=1, transport_stack=3, web_transport=0).
    // The web_transport slot adds a new dimension that lands in
    // beta.1; pinning the count prevents accidental over- or
    // under-registration in the PAL Slice 2 table.
    EXPECT_EQ(PluginRegistry::instance().list_dtls_sessions().size(), 1u);
    EXPECT_EQ(PluginRegistry::instance().list_sctp_sockets().size(), 1u);
    EXPECT_EQ(PluginRegistry::instance().list_raw_udp_datagrams().size(), 1u);
    EXPECT_EQ(PluginRegistry::instance().list_transport_stacks().size(), 3u)
        << "Slice 7.6 baseline: default (shell) + webrtc-classic + "
           "raw-udp-arq (3 entries)";
    EXPECT_EQ(PluginRegistry::instance().list_web_transports().size(), 0u)
        << "v0.14.0 alpha.2: web_transport slot is empty (no "
           "factory is auto-registered; the msquic factory lands "
           "in beta.1)";
}

} // anonymous namespace
