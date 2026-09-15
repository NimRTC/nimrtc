/**
 * @file nimrtc/dtls/dtls_plugin.hpp
 * @brief Public registration entry point for the DTLS seam (PAL Slice 4).
 *
 * Mirrors `src/modules/ice/include/nimrtc/ice/ice.hpp`'s public surface:
 *
 *   namespace nimrtc::dtls {
 *     namespace detail { void do_register_default_plugins() noexcept; }
 *     void register_default_plugins() noexcept;
 *   }
 *
 * Calling `nimrtc::dtls::register_default_plugins()` once at program
 * startup registers the built-in `WolfsslDtlsFactory` (id = "wolfssl")
 * under the DTLS seam.  Subsequent calls are no-ops (Meyer's-singleton
 * latch, same pattern as ice/rtp/audio3a).
 *
 * ## Why not in plugins/
 *
 * The DTLS seam is intentionally module-local (it lives next to the
 * concrete dtls module).  Promoting the registration to
 * `core::register_all_default_plugins()` is a Slice 7 / Slice 8
 * concern — for Slice 4 we only stand up the seam and its default
 * factory, and document the integration point.
 *
 * ## Static-link workaround
 *
 * The function is intentionally NOT marked `inline`: when the
 * `register_default_plugins()` symbol is referenced from a consumer
 * that links the `nimrtc_dtls` static library via MSVC, an `inline`
 * declaration causes the compiler to emit the static-local latch as a
 * weak external symbol that the .lib archive may not carry.  Making
 * the function non-inline forces a strong definition in
 * `nimrtc_dtls.lib` so the consumer's link always pulls it in.
 *
 * @note P1 — entry point added as part of PAL Slice 4 (v0.11.0).
 */

#ifndef NIMRTC_DTLS_PLUGIN_HPP
#define NIMRTC_DTLS_PLUGIN_HPP

#include <nimrtc/dtls/dtls_session_factory.hpp>   // IDtlsSessionFactory

namespace nimrtc::dtls {

namespace detail {
/**
 * @brief Implementation of register_default_plugins.
 *
 * Defined in `src/dtls/src/dtls_plugin.cpp`.  Forces the .obj (with
 * its static Registrar) into the consumer's link when
 * register_default_plugins() is called.  Mirrors the ICE pattern.
 */
void do_register_default_plugins() noexcept;
} // namespace detail

/**
 * @brief Register the built-in DTLS factory (id = "wolfssl").
 *
 * Idempotent: the first call constructs and publishes a single
 * `WolfsslDtlsFactory` instance; subsequent calls hit the static
 * latch and return immediately.  MUST be called once at program
 * startup before any transport-stack construction (Slice 7) or
 * engine integration (Slice 8) looks up the "wolfssl" factory.
 *
 * @note Not `inline` — see the file header for the static-link
 *       rationale that matches `nimrtc::ice::register_default_plugins()`
 *       and `nimrtc::rtp::register_default_plugins()`.
 */
void register_default_plugins() noexcept;

// ---------------------------------------------------------------------------
// Test-only accessor (not part of the public API; production code MUST NOT
// use this).  Mirrors `nimrtc::sctp::test_only::get_stub_factory()` —
// keeps the public seam surface (only `register_default_plugins()`) clean
// while still letting tests resolve the factory pointer without going
// through a registry.
//
// Slice 7 will route the canonical lookup through
// `core::PluginRegistry::register_dtls(...)` / `get_dtls(id)`; for
// Slice 4 the test path is the seam-local accessor.
// ---------------------------------------------------------------------------
namespace test_only {

/** Returns the registered factory pointer, or nullptr if
 *  `register_default_plugins()` has not been called yet.
 *  Test-only — production code MUST NOT call this. */
const IDtlsSessionFactory* get_wolfssl_factory() noexcept;

} // namespace test_only

} // namespace nimrtc::dtls

#endif // NIMRTC_DTLS_PLUGIN_HPP
