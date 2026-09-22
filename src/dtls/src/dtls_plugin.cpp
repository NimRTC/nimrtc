/**
 * @file src/dtls/src/dtls_plugin.cpp
 * @brief register_default_plugins() implementation for the DTLS seam (PAL Slice 4).
 *
 * Mirrors src/modules/ice/src/ice.cpp's pattern:
 *
 *   namespace detail { void do_register_default_plugins() noexcept { ... } }
 *
 *   void register_default_plugins() noexcept {
 *       static const int once = []() {
 *           detail::do_register_default_plugins();
 *           return 1;
 *       }();
 *       (void)once;
 *   }
 *
 * The `static const int once = []() { ... }()` pattern is the
 * Meyer-singleton latch — idempotent under repeated calls and
 * thread-safe per C++11 static-local semantics.  The wrapper
 * `register_default_plugins()` is intentionally NOT `inline`: see
 * the header comment in `dtls_plugin.hpp` for the MSVC static-lib
 * linker rationale.
 *
 * ## Slice 8 (v0.10.2) — registry hook
 *
 * The DTLS factory is published via
 * `nimrtc::core::PluginRegistry::register_dtls_session(id, factory*)`
 * (Transport PAL Slice 8 — see `docs/plan/transport-selection.md` §6.5).
 * The legacy `test_only::get_wolfssl_factory()` accessor is preserved
 * as a thin wrapper around `core::PluginRegistry::get_dtls_session(id)`
 * so existing Slice 4 tests continue to compile and pass without
 * modification.
 *
 * @note P1 — entry point added as part of PAL Slice 4 (v0.11.0),
 *       promoted to the typed registry hook in Slice 8 (v0.10.2).
 */

#include <nimrtc/dtls/dtls_plugin.hpp>
#include <nimrtc/dtls/dtls_session_factory.hpp>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>

// Forward declaration of the test-only accessor defined in
// dtls_wolfssl_factory.cpp.  We don't include the .cpp directly —
// keeping the accessor internal makes the seam module's public
// surface (`dtls_session_iface.hpp`, `dtls_session_factory.hpp`,
// `dtls_plugin.hpp`) clean.
namespace nimrtc::dtls {
const IDtlsSessionFactory* wolfssl_factory_singleton() noexcept;
namespace test_only {
const IDtlsSessionFactory* get_wolfssl_factory() noexcept;
void set_wolfssl_factory_for_testing(const IDtlsSessionFactory* f) noexcept;
} // namespace test_only
} // namespace nimrtc::dtls

// ADR-013: conditionally include GMSSL factory forward declaration.
#if defined(NIMRTC_HAS_DTLS_GMSSL)
namespace nimrtc::dtls {
const IDtlsSessionFactory* gmssl_factory_singleton() noexcept;
namespace test_only {
const IDtlsSessionFactory* get_gmssl_factory() noexcept;
void set_gmssl_factory_for_testing(const IDtlsSessionFactory* f) noexcept;
} // namespace test_only
} // namespace nimrtc::dtls
#endif // NIMRTC_HAS_DTLS_GMSSL

namespace nimrtc::dtls {

namespace detail {

// ---------------------------------------------------------------------------
// do_register_default_plugins
// ---------------------------------------------------------------------------
//
// Defined in this .cpp (not header) so the strong-definition symbol
// lands in `nimrtc_dtls_seam.lib` for static-link consumers (same
// workaround as ice.cpp / rtp_plugin.cpp).
//
// On first call: construct a single `WolfsslDtlsFactory` (Meyer
// singleton), publish it under the seam id "wolfssl" through BOTH:
//   (1) the typed registry hook
//       `core::PluginRegistry::register_dtls_session("wolfssl", &f)`
//       — the canonical lookup path used by the engine / Selector /
//       Stack factory composition;
//   (2) the legacy `test_only::set_wolfssl_factory_for_testing(&f)`
//       hook — preserved verbatim for Slice 4 tests that resolve the
//       factory pointer without going through the registry.

void do_register_default_plugins() noexcept {
    // `static` inside a function ⇒ address-stable for program lifetime;
    // constructed exactly once on the first invocation (C++11
    // guarantees thread-safety for static-local initialisation).
    static const struct Registrar {
        Registrar() {
            const IDtlsSessionFactory* f = wolfssl_factory_singleton();

            // (1) Canonical: publish via the Slice 8 typed registry hook.
            //     This is what the engine / Selector / future Profile
            //     loader will look up by id "wolfssl" when they need a
            //     DTLS backend. The hook is idempotent under repeated
            //     calls (TypedRegistry::register_one overwrites in place).
            nimrtc::core::PluginRegistry::instance().register_dtls_session(
                std::string_view{f->id()}, f);

            // (2) Legacy: keep the Slice 4 test-only accessor wired so
            //     tests that resolve the factory pointer without going
            //     through the registry continue to work.
            test_only::set_wolfssl_factory_for_testing(f);

            nimrtc::core::log::Logger::instance().info(
                std::string("nimrtc::dtls: default plugin registered (id=") +
                std::string(f->id()) +
                ", via Slice 8 typed registry hook + legacy test_only slot)");

// ADR-013: register GMSSL factory alongside wolfSSL when the backend
// was enabled at build time (NIMRTC_HAS_DTLS_GMSSL=1).
#if defined(NIMRTC_HAS_DTLS_GMSSL)
            const IDtlsSessionFactory* g = gmssl_factory_singleton();
            test_only::set_gmssl_factory_for_testing(g);
            nimrtc::core::log::Logger::instance().info(
                std::string("nimrtc::dtls: GMSSL plugin registered (id=") +
                std::string(g->id()) + ")");
#endif // NIMRTC_HAS_DTLS_GMSSL
        }
    } s_registrar;
    (void)s_registrar;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Public registration entry point
// ---------------------------------------------------------------------------
//
// Non-inline definition forces a strong symbol in the static lib so
// consumers that call `nimrtc::dtls::register_default_plugins()` from
// their translation unit always link the implementation (matches the
// nimrtc_ice.lib / nimrtc_rtp.lib patterns).
//
// Meyer's-singleton latch — first call runs detail::do_register_default_plugins()
// exactly once; subsequent calls early-return on the `once` flag.
//
// `noexcept` per the header declaration: registration only touches
// Meyers-singleton statics and the Logger; both are noexcept-safe.

void register_default_plugins() noexcept {
    static const int once = []() {
        detail::do_register_default_plugins();
        return 1;
    }();
    (void)once;
}

} // namespace nimrtc::dtls
