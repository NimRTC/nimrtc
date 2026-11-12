/**
 * @file src/modules/srtp/src/srtp_plugin.cpp
 * @brief register_default_plugins() implementation for the SRTP
 *        crypto-backend seam (v1.2.0, remediation plan B7 / Q3).
 *
 * Mirrors `src/dtls/src/dtls_plugin.cpp`'s pattern:
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
 * `register_default_plugins()` is intentionally NOT `inline`: see the
 * header comment in `srtp_plugin.hpp` for the MSVC static-lib
 * linker rationale.
 *
 * ## Registry hook
 *
 * The factory is published via
 * `nimrtc::core::PluginRegistry::register_srtp_backend(id, factory*)`.
 * The engine resolves that slot through `EngineConfig::srtp_backend_name`
 * (default "libsrtp").  An unknown id is a **hard failure** at
 * `open()` time — there is no silent fallback to libsrtp.
 */

#include <nimrtc/srtp/srtp_plugin.hpp>

#include <string_view>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/srtp/libsrtp_backend.hpp>

namespace nimrtc::srtp {

namespace detail {

// ---------------------------------------------------------------------------
// do_register_default_plugins
// ---------------------------------------------------------------------------
//
// Defined in this .cpp (not header) so the strong-definition symbol
// lands in `nimrtc_srtp.lib` for static-link consumers (same
// workaround as ice.cpp / rtp_plugin.cpp / dtls_plugin.cpp).
//
// On first call: construct a single `LibsrtpBackendFactory` (Meyer
// singleton) and publish it under the seam id "libsrtp" through the
// typed registry hook.

void do_register_default_plugins() noexcept {
    static const struct Registrar {
        Registrar() {
            static const LibsrtpBackendFactory factory{};
            const plugins::ISrtpBackendFactory* f = &factory;

            nimrtc::core::PluginRegistry::instance().register_srtp_backend(
                std::string_view{f->id()}, f);

            nimrtc::core::log::Logger::instance().info(
                std::string("nimrtc::srtp: default SRTP backend registered (id=") +
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
// consumers that call `nimrtc::srtp::register_default_plugins()` from
// their translation unit always link the implementation (matches the
// nimrtc_ice.lib / nimrtc_rtp.lib / nimrtc_dtls_seam.lib patterns).
//
// Meyer's-singleton latch — first call runs
// detail::do_register_default_plugins() exactly once; subsequent calls
// early-return on the `once` flag.
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

} // namespace nimrtc::srtp
