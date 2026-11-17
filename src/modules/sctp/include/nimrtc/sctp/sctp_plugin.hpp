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
 * The default implementation is the **stub** factory (id="stub",
 * `SctpStubFactory`). Sends fail with `plugins::kErrNotReady` — the seam
 * compiles and registers, but there is no real SCTP transport. The
 * production data plane is WebTransport over QUIC, which does not go
 * through this seam.
 *
 * ## Idempotency
 *
 * `register_default_plugins()` uses a static-local latch (Meyers singleton)
 * so it is safe to call multiple times. The first call registers
 * `SctpStubFactory` under id "stub"; subsequent calls are no-ops.
 *
 * @note Not `inline` — see the static-link workaround comment in
 *       `nimrtc::ice::register_default_plugins()` (same pattern; the
 *       static-local latch must land in a real .obj inside the
 *       `nimrtc_sctp` static lib).
 */

#ifndef NIMRTC_SCTP_SCTP_PLUGIN_HPP
#define NIMRTC_SCTP_SCTP_PLUGIN_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.

#include <nimrtc/core/api_version.hpp>
#include <nimrtc/sctp/sctp_socket_factory.hpp>

namespace nimrtc::sctp {

class NIMRTC_API_STABLE SctpStubFactory;   // forward-declared from src/sctp_stub_factory.hpp

/**
 * @brief Register the built-in "stub" SCTP factory with
 *        `nimrtc::core::PluginRegistry`.
 *
 * Since v1.2.0 (plan E7) this is the **only** SCTP backend that exists:
 * the usrsctp backend was deleted together with its vendored tree.
 *
 * Idempotent (Meyers-singleton latch in the .cpp).
 *
 * @note Not `inline` — see the static-link workaround in
 *       `nimrtc::ice::register_default_plugins()`.
 */
NIMRTC_API_STABLE void register_default_plugins() noexcept;

// ---------------------------------------------------------------------------
// Test-only accessor (not part of the public API; production code MUST NOT
// use this). Lives in a clearly-tagged nested namespace so the public
// surface stays minimal (only `register_default_plugins()` is exported).
// ---------------------------------------------------------------------------
namespace test_only {

/** Returns the registered stub factory pointer, or nullptr if
 *  `register_default_plugins()` has not been called yet.
 *  Test-only — production code MUST NOT call this. */
NIMRTC_API_STABLE const SctpStubFactory* get_stub_factory() noexcept;

} // namespace test_only

} // namespace nimrtc::sctp

#endif // NIMRTC_SCTP_SCTP_PLUGIN_HPP
