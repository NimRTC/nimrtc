/**
 * @file nimrtc/web_transport/web_transport_plugin.hpp
 * @brief register_default_plugins() declaration for the WebTransport seam.
 *
 * v0.14.0-beta.1 (ADR-014): the WebTransport / QUIC seam is now
 * production-ready.  This header exposes the `register_default_plugins()`
 * entry point that publishes the msquic-backed factory under the id
 * "msquic" into the global `core::PluginRegistry`.
 *
 * The header mirrors the pattern used by the DTLS / SCTP / raw_udp
 * seam modules (`nimrtc/dtls/dtls_plugin.hpp`, etc.) so the call
 * site is consistent across all transport-layer plugins:
 *
 * @code
 *   #include <nimrtc/web_transport/web_transport_plugin.hpp>
 *   nimrtc::web_transport::register_default_plugins();
 * @endcode
 *
 * Or, equivalently, via the unified entry point:
 *
 * @code
 *   #include <nimrtc/core/registry.hpp>
 *   nimrtc::core::register_all_default_plugins();
 * @endcode
 *
 * The forward declaration of `register_default_plugins()` in
 * `nimrtc/core/registry.hpp` (the `web_transport` namespace block)
 * resolves the symbol without pulling this header into every TU
 * that includes `registry.hpp`.
 *
 * @note P0 — added as part of v0.14.0-beta.1 (ADR-014).
 */

#ifndef NIMRTC_WEB_TRANSPORT_PLUGIN_HPP
#define NIMRTC_WEB_TRANSPORT_PLUGIN_HPP

#include <nimrtc/core/api_version.hpp>  // NIMRTC_API_UNSTABLE

namespace nimrtc::web_transport {

/**
 * @brief Register the default WebTransport factory (msquic).
 *
 * Idempotent — first call registers the msquic-backed factory under
 * the id "msquic" in `core::PluginRegistry`; subsequent calls are no-ops.
 * Thread-safe per C++11 static-local semantics.
 *
 * Registered factory:
 *   - id:          "msquic"
 *   - display_name: "WebTransport over QUIC (msquic, MIT)"
 *
 * The factory is looked up by `EngineConfig::datachannel_backend`
 * when the engine builds a WebTransport data-channel (beta.1 scope).
 *
 * @see v0.14-plan.md §2.1.1 WT-REGISTRY
 */
NIMRTC_API_UNSTABLE
void register_default_plugins() noexcept;

} // namespace nimrtc::web_transport

#endif // NIMRTC_WEB_TRANSPORT_PLUGIN_HPP
