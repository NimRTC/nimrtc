/**
 * @file nimrtc/srtp/srtp_plugin.hpp
 * @brief Public registration entry point for the SRTP crypto-backend seam.
 *
 * Mirrors the DTLS seam's pattern (`src/dtls/include/nimrtc/dtls/
 * dtls_plugin.hpp`):
 *
 *   namespace nimrtc::srtp {
 *     void register_default_plugins() noexcept;
 *   }
 *
 * Calling `nimrtc::srtp::register_default_plugins()` once at program
 * startup registers the built-in `LibsrtpBackendFactory` (id = "libsrtp")
 * under the SRTP-backend seam.  Subsequent calls are no-ops (Meyer's-
 * singleton latch, the same pattern as ice / rtp / dtls / audio3a).
 *
 * ## Why this entry point exists now (v1.2.0)
 *
 * The `ISrtpSessionBackend` seam was introduced in v1.1 as a
 * *declaration only*: the interface and the libsrtp adapter existed,
 * but `SrtpContext` called libsrtp directly and the registry had no
 * slot for the family — so nothing could ever select a different
 * backend.  This entry point publishes the built-in backend into the
 * registry's SRTP slot (`PluginRegistry::register_srtp_backend`), and
 * the engine resolves that slot through `EngineConfig::srtp_backend_name`.
 *
 * @note v1.2.0 (architecture remediation plan B7 / Q3).
 */
#ifndef NIMRTC_SRTP_PLUGIN_HPP
#define NIMRTC_SRTP_PLUGIN_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>

namespace nimrtc::srtp {

namespace detail {
/**
 * @brief Implementation of register_default_plugins.
 *
 * Defined in `src/modules/srtp/src/srtp_plugin.cpp`.  Forces the .obj
 * (with its static Registrar) into the consumer's link when
 * register_default_plugins() is called.  Mirrors the DTLS pattern.
 */
void do_register_default_plugins() noexcept;
} // namespace detail

/**
 * @brief Register the built-in SRTP backend factory (id = "libsrtp").
 *
 * Idempotent: the first call constructs and publishes a single
 * `LibsrtpBackendFactory` instance; subsequent calls hit the static
 * latch and return immediately.  MUST be called once at program startup
 * (or via `core::register_all_default_plugins()`) before the engine
 * resolves `EngineConfig::srtp_backend_name`.
 *
 * @note Not `inline` — see the file header for the static-link
 *       rationale that matches `nimrtc::dtls::register_default_plugins()`.
 */
void register_default_plugins() noexcept;

} // namespace nimrtc::srtp

#endif // NIMRTC_SRTP_PLUGIN_HPP
