/**
 * @file src/sctp/src/sctp_plugin.cpp
 * @brief nimrtc::sctp::register_default_plugins() — Slice 5 entry point.
 *
 * Registers the built-in "stub" factory with the global
 * `nimrtc::core::PluginRegistry` via the typed `register_sctp_socket`
 * hook (Transport PAL Slice 8 — `docs/plan/transport-selection.md`
 * §6.5 follow-up gap). The legacy `test_only::get_stub_factory()`
 * accessor is preserved as a thin wrapper around
 * `core::PluginRegistry::get_sctp_socket("stub")` so existing Slice 5
 * tests continue to compile and pass without modification.
 *
 * ## Idempotency
 *
 * Uses a Meyers-singleton static-local latch (see ice.cpp's
 * `register_default_plugins()` for the same workaround). The latch:
 *   - runs the registration code exactly once per process;
 *   - guarantees the symbol is emitted inside `nimrtc_sctp.lib` (which the
 *     MSVC static-link workaround requires — see ice.cpp comment).
 *
 * ## Slice 5 vs. v0.11.0
 *
 * v0.11.0 ships with the **stub-only** registration. The usrsctp
 * production backend (`UsrsctpSocketFactory`, id="usrsctp") is built
 * and linked into `nimrtc_sctp.lib` (so anyone who wants to wire it
 * manually can), but it is **NOT auto-registered** with the global
 * PluginRegistry — see "Why usrsctp is not the default" below.
 *
 * ## Why usrsctp is not the default in v0.11.0
 *
 * Per `docs/plan/v0.11-plan.md` §1 (the v0.11.0 cut at tag prep
 * time) and the deferred status recorded in §2.2 of the same plan:
 *
 *   - The SCTP-over-DTLS physical wiring (set_dtls_keys() +
 *     DTLS↔SCTP inbound/outbound hookup) was never landed on
 *     `main`; only the in-process `tests/test_datachannel_engine`
 *     path passes 5/5. Chrome-headless e2e stays 0/4.
 *   - usrsctp 0.9.5.0 carries a heavy third-party build cost
 *     (`src/third_party/usrsctp/`: ~25 source files, ~440 compilation
 *     units) and a Windows MSVCRT WSAELOOP limitation on
 *     self-loopback that the in-process test tolerates but a real
 *     DataChannel e2e would not.
 *   - Chrome is moving DataChannel to **WebTransport over QUIC** in
 *     the medium term; the SCTP-over-DTLS path is on Google's
 *     deprecation roadmap. v1.x will revisit the data-plane
 *     problem on top of WebTransport / QUIC instead of
 *     SCTP-over-DTLS. Keeping usrsctp in the default registration
 *     would commit v0.11.0 to a path the upstream is moving away
 *     from.
 *
 * The class still compiles and links; opt-in callers can construct
 * a `UsrsctpSocketFactory` directly and call `factory->create(cfg)`
 * without going through the global registry. The header carries a
 * `@deprecated` comment directing readers to the v1.x plan.
 *
 * ## Note on PluginRegistry::register_sctp_socket
 *
 * Slice 5 landed before the typed registry hook did. The hook is now
 * in place (v0.10.2 Slice 8 — see `core::PluginRegistry::register_sctp_socket`)
 * so this file's body now publishes the factory through that hook
 * directly, with the legacy `g_stub_factory` cache preserved as a
 * back-compat slot.
 */

#include <nimrtc/sctp/sctp_plugin.hpp>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/sctp/sctp_socket_factory.hpp>
#include "sctp_stub_factory.hpp"

namespace nimrtc::sctp {

namespace {

// Process-local cache of the registered factory pointer. Slice 5's
// original pattern (pre-Slice-8) relied on this cache because
// `PluginRegistry::register_sctp_socket()` did not yet exist. Slice 8
// promotes the canonical source of truth to the typed registry slot;
// this cache is preserved as a back-compat layer so any external test
// code that pokes the slot directly via a friend function still works.
//
// `get_stub_factory()` now reads back through the registry first
// (preferring the typed slot) and only falls back to this cache if
// the registry has not been populated yet.
const SctpStubFactory* g_stub_factory = nullptr;

} // namespace

// =============================================================================
// Public registration entry point
// =============================================================================

void register_default_plugins() noexcept {
    // Static-local latch — runs once, idempotent. Same pattern as
    // `nimrtc::ice::register_default_plugins()` (see ice.cpp).
    static const int once = []() {
        // v0.11.0 ships the stub-only default registration. The
        // `UsrsctpSocketFactory` (id="usrsctp") is intentionally NOT
        // registered here — see the file-header "Why usrsctp is not
        // the default in v0.11.0" block for the v1.x WebTransport
        // re-design rationale.
        //
        // The class still compiles and links into `nimrtc_sctp.lib`
        // (see `src/sctp/CMakeLists.txt` — the usrsctp_*.cpp sources
        // are still built); opt-in callers can `new
        // UsrsctpSocketFactory()` and call `factory->create(cfg)`
        // directly. The header carries a `@deprecated` comment
        // pointing at the v1.x plan.
        static const SctpStubFactory s_stub_factory{};

        // Canonical path (Slice 8): publish via the typed registry hook.
        // The engine / Selector / future Profile loader will look up the
        // factory by id "stub" through this slot.
        nimrtc::core::PluginRegistry::instance().register_sctp_socket(
            std::string_view{s_stub_factory.id()}, &s_stub_factory);

        // Legacy: keep the process-local cache in sync for any
        // pre-Slice-8 test code that still pokes the slot directly.
        // `get_stub_factory()` prefers the typed registry slot, so the
        // cache is a back-compat fallback rather than a second source
        // of truth.
        g_stub_factory = &s_stub_factory;

        core::log::Logger::instance().info(
            "nimrtc::sctp: registered default plugins "
            "(id=\"" + std::string{s_stub_factory.id()} +
            "\" [Slice 5 stub]; usrsctp production backend is "
            "@deprecated — see v0.11-plan.md §2.2 / v1.x WebTransport plan)");
        return 1;
    }();
    (void)once;
}

namespace test_only {

// Test-only accessor (defined here, declared in sctp_plugin.hpp). Returns
// the registered stub factory pointer, or nullptr if
// `register_default_plugins()` has not been called yet.
//
// Slice 8: prefers the typed registry slot so callers always observe
// the same factory the engine sees. Falls back to the legacy cache
// only if the registry has not been populated yet (e.g. when a test
// pokes the slot directly).
const SctpStubFactory* get_stub_factory() noexcept {
    if (const auto* f = nimrtc::core::PluginRegistry::instance()
                            .get_sctp_socket("stub")) {
        // The typed registry stores the base interface pointer; the
        // registered factory IS-A SctpStubFactory so the downcast is
        // safe (verified at registration time — see the factory's
        // static type). Cast back to the concrete type for the
        // back-compat accessor.
        return static_cast<const SctpStubFactory*>(f);
    }
    return g_stub_factory;
}

} // namespace test_only

} // namespace nimrtc::sctp
