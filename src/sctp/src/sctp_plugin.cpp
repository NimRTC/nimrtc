/**
 * @file src/sctp/src/sctp_plugin.cpp
 * @brief nimrtc::sctp::register_default_plugins() — Slice 5 entry point.
 *
 * Registers the built-in "stub" factory with the global
 * `nimrtc::core::PluginRegistry` (when the registry hook lands — see
 * "Note on PluginRegistry::register_sctp_socket" below). For now the
 * factory pointer is published to a process-local cache that the tests
 * read through `nimrtc::sctp::test_only::get_stub_factory()`.
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
 * The Slice 5 task brief mentions `register_datachannel` (the existing
 * `IDataChannel` interface) but §5.2 / §6.2 of the transport-selection
 * plan calls for a brand-new `ISctpSocket` / `ISctpSocketFactory` seam
 * living under `nimrtc::sctp::`. This is a deliberate seam-level addition
 * (parallel to IDataChannel, but typed against the lower-level SCTP API)
 * and does NOT replace the IDataChannel interface.
 *
 * PluginRegistry does NOT yet expose `register_sctp_socket()` — adding
 * that hook would touch `src/core/...` and is outside Slice 5 scope. Until
 * the registry hook lands, the registration below publishes the factory
 * only in a process-local static cache that tests can read directly.
 * Once the registry hook is added (a separate Slice 5 / Slice 7 cleanup),
 * this file swaps the body to call `PluginRegistry::register_sctp_socket`
 * and tests read through the registry.
 */

#include <nimrtc/sctp/sctp_plugin.hpp>

#include <nimrtc/core/log.hpp>
#include <nimrtc/sctp/sctp_socket_factory.hpp>
#include "sctp_stub_factory.hpp"

namespace nimrtc::sctp {

namespace {

// Process-local cache of the registered factory pointer (Slice 5 stand-in
// for the missing `PluginRegistry::register_sctp_socket()` hook). Set
// exactly once by the Meyers-singleton latch inside register_default_plugins().
const SctpStubFactory* g_stub_factory = nullptr;

} // namespace

// =============================================================================
// Public registration entry point
// =============================================================================

void register_default_plugins() noexcept {
    // Static-local latch — runs once, idempotent. Same pattern as
    // `nimrtc::ice::register_default_plugins()` (see ice.cpp).
    static const int once = []() {
        static const SctpStubFactory s_factory{};

        // Publish to the process-local cache. Tests use this via
        // `nimrtc::sctp::test_only::get_stub_factory()`. Once the
        // `PluginRegistry::register_sctp_socket` hook lands, this body
        // becomes:
        //     core::PluginRegistry::instance().register_sctp_socket(
        //         s_factory.id(), &s_factory);
        g_stub_factory = &s_factory;

        core::log::Logger::instance().info(
            "nimrtc::sctp: registered default plugin (id=\"" +
            std::string{s_factory.id()} +
            "\"; Slice 5 stub; v0.11.0 usrsctp integration pending)");
        return 1;
    }();
    (void)once;
}

namespace test_only {

// Test-only accessor (defined here, declared in sctp_plugin.hpp). Returns
// the registered stub factory pointer, or nullptr if
// `register_default_plugins()` has not been called yet.
const SctpStubFactory* get_stub_factory() noexcept {
    return g_stub_factory;
}

} // namespace test_only

} // namespace nimrtc::sctp
