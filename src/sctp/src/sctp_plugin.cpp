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
 * Slice 5 only registers the stub factory (id="stub"). v0.11.0 will
 * additionally register the production `UsrsctpSocketFactory` (id="usrsctp")
 * without disturbing the "stub" entry — see
 * `docs/plan/transport-selection.md` §6.2 / §7 / §8 #7.
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
#include <nimrtc/sctp/usrsctp_factory.hpp>

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
        static const SctpStubFactory s_stub_factory{};
        // TPAL-5 (v0.11.0) — production usrsctp backend.
        // Sibling to the stub; both factories coexist so v0.10.x
        // callers that picked `id="stub"` continue to work.
        // See `docs/plan/transport-selection.md` §6.2 / §7.
        static const UsrsctpSocketFactory s_usrsctp_factory{};

        // Canonical path (Slice 8): publish via the typed registry hook.
        // The engine / Selector / future Profile loader will look up the
        // factory by id "stub" through this slot.
        nimrtc::core::PluginRegistry::instance().register_sctp_socket(
            std::string_view{s_stub_factory.id()}, &s_stub_factory);

        // TPAL-5: register the production backend under id "usrsctp".
        // Both factories coexist; explicit-id callers get whichever
        // they ask for, and the typed slot is the canonical source
        // of truth for both.
        nimrtc::core::PluginRegistry::instance().register_sctp_socket(
            std::string_view{s_usrsctp_factory.id()}, &s_usrsctp_factory);

        // Legacy: keep the process-local cache in sync for any
        // pre-Slice-8 test code that still pokes the slot directly.
        // `get_stub_factory()` prefers the typed registry slot, so the
        // cache is a back-compat fallback rather than a second source
        // of truth.
        g_stub_factory = &s_stub_factory;

        core::log::Logger::instance().info(
            "nimrtc::sctp: registered default plugins "
            "(id=\"" + std::string{s_stub_factory.id()} +
            "\" [Slice 5 stub] + id=\"" + std::string{s_usrsctp_factory.id()} +
            "\" [TPAL-5 usrsctp production backend]; "
            "Slice 8 typed registry hook + legacy cache)");
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
