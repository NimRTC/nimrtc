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
 * ## Slice 4 scope (this file)
 *
 * Registers the default WolfsslDtlsFactory under the DTLS seam id
 * "wolfssl".  Slice 7 / Slice 8 will add a corresponding slot in
 * `core::PluginRegistry::register_dtls(...)`; for Slice 4 the
 * registration is seam-local (the factory is reachable via
 * `test_only::get_wolfssl_factory()` from tests that link
 * `nimrtc_dtls_seam`).
 *
 * That keeps Slice 4 a true seam-only diff: no
 * core::PluginRegistry surface changes, no engine.cpp changes.
 *
 * @note P1 — entry point added as part of PAL Slice 4 (v0.11.0).
 */

#include <nimrtc/dtls/dtls_plugin.hpp>
#include <nimrtc/dtls/dtls_session_factory.hpp>

#include <nimrtc/core/log.hpp>

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
// singleton) and publish it under the seam id "wolfssl".  Slice 7
// will add the registry call here:
//
//     nimrtc::core::PluginRegistry::instance().register_dtls(
//         std::string_view{s_factory.id()}, &s_factory);
//
// For Slice 4 the seam is module-local — the factory is reachable via
// `test_only::get_wolfssl_factory()` from any consumer that links
// `nimrtc_dtls_seam`.  Slice 7 will promote it to a registered
// backend when the transport-stack / Profile schema lands.

void do_register_default_plugins() noexcept {
    // `static` inside a function ⇒ address-stable for program lifetime;
    // constructed exactly once on the first invocation (C++11
    // guarantees thread-safety for static-local initialisation).
    static const struct Registrar {
        Registrar() {
            // Publish the singleton factory pointer through the
            // test-only slot so Slice 4 tests can resolve the
            // factory without going through core::PluginRegistry.
            // Slice 7 will replace this with a `register_dtls(...)`
            // call into the typed DTLS slot.
            const IDtlsSessionFactory* f = wolfssl_factory_singleton();
            test_only::set_wolfssl_factory_for_testing(f);
            nimrtc::core::log::Logger::instance().info(
                std::string("nimrtc::dtls: default plugin registered (id=") +
                std::string(f->id()) + ")");
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
