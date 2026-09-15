/**
 * @file nimrtc/sctp/sctp_plugin.hpp
 * @brief Public registration entry point for the SCTP module (Slice 5).
 *
 * Per `docs/plan/transport-selection.md` §5.2 / §6.2, the SCTP seam
 * (`ISctpSocket` / `ISctpSocketFactory`) is registered with the global
 * `nimrtc::core::PluginRegistry` via a single entry point:
 *
 * ```cpp
 * nimrtc::sctp::register_default_plugins();
 * ```
 *
 * ## Slice 5 status
 *
 * The default implementation in v0.10.x is the **stub** factory (id="stub",
 * `SctpStubFactory`). Sends fail with `plugins::kErrNotReady` — the seam
 * compiles and registers, but the engine has no real SCTP transport until
 * v0.11.0 lands `UsrsctpSocket` and flips the registered id to "usrsctp"
 * per `docs/plan/transport-selection.md` §7.
 *
 * ## Idempotency
 *
 * `register_default_plugins()` uses a static-local latch (Meyers singleton)
 * so it is safe to call multiple times. The first call registers
 * `SctpStubFactory` under id "stub"; subsequent calls are no-ops.
 *
 * ## v0.11.0 migration note
 *
 * When `UsrsctpSocketFactory` (id="usrsctp") lands, the plugin entry
 * function should:
 *   1. Register the production factory under id "usrsctp" (replacing
 *      "stub" in the active set, or adding "usrsctp" alongside it for
 *      downgrade scenarios — see transport-selection §8 #7).
 *   2. Leave the stub factory registered (or remove it explicitly) so
 *      tests can still resolve id="stub" if they want to verify
 *      behaviour against a non-ready backend.
 *
 * @note Not `inline` — see the static-link workaround comment in
 *       `nimrtc::ice::register_default_plugins()` (same pattern; the
 *       static-local latch must land in a real .obj inside the
 *       `nimrtc_sctp` static lib).
 */

#ifndef NIMRTC_SCTP_SCTP_PLUGIN_HPP
#define NIMRTC_SCTP_SCTP_PLUGIN_HPP

#include <nimrtc/sctp/sctp_socket_factory.hpp>

namespace nimrtc::sctp {

class SctpStubFactory;   // forward-declared from src/sctp_stub_factory.hpp

/**
 * @brief Register the built-in "stub" SCTP factory with
 *        `nimrtc::core::PluginRegistry`.
 *
 * Default impl pending v0.11.0 usrsctp integration per
 * `docs/plan/transport-selection.md` §7. Once v0.11.0 lands
 * `UsrsctpSocketFactory`, this function will additionally register id
 * "usrsctp" (the production default) — see the v0.11.0 migration note
 * in the file header.
 *
 * Idempotent (Meyers-singleton latch in the .cpp).
 *
 * @note Not `inline` — see the static-link workaround in
 *       `nimrtc::ice::register_default_plugins()`.
 */
void register_default_plugins() noexcept;

// ---------------------------------------------------------------------------
// Test-only accessor (not part of the public API; production code MUST NOT
// use this). Lives in a clearly-tagged nested namespace so the public
// surface stays minimal (only `register_default_plugins()` is exported).
// ---------------------------------------------------------------------------
namespace test_only {

/** Returns the registered stub factory pointer, or nullptr if
 *  `register_default_plugins()` has not been called yet.
 *  Test-only — production code MUST NOT call this. */
const SctpStubFactory* get_stub_factory() noexcept;

} // namespace test_only

} // namespace nimrtc::sctp

#endif // NIMRTC_SCTP_SCTP_PLUGIN_HPP
