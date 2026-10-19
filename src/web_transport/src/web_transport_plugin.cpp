/**
 * @file src/web_transport/src/web_transport_plugin.cpp
 * @brief register_default_plugins() implementation (v0.14.0-beta.1, ADR-014).
 *
 * Mirrors src/dtls/src/dtls_plugin.cpp and src/sctp/src/sctp_plugin.cpp:
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
 * The `static const int once = []() { ... }()` Meyer-singleton latch
 * ensures registration runs exactly once on the first invocation and
 * is thread-safe per C++11.
 *
 * ## What is registered
 *
 * Two factory slots are published in this module:
 *
 * 1. `WebTransportMsQuicSessionFactory` (id = "msquic") — via
 *    `core::PluginRegistry::register_web_transport("msquic", factory*)`.
 *    Produces `IWebTransportSession` instances backed by msquic.
 *
 * 2. `WebTransportDataChannelFactory` (id = "web_transport") — via
 *    `core::PluginRegistry::register_datachannel("web_transport", factory*)`.
 *    Produces `plugins::IDataChannel` instances backed by an
 *    `IWebTransportSession`.
 *
 * ## What is NOT registered here
 *
 * - Server-side listener infrastructure (v1.x scope).
 * - The quiche fallback factory (ADR-014 §Alternatives — only registered
 *   if msquic's ADR-014 Criterion 5 maintenance check fails at build time).
 *
 * @note P0 — added as part of v0.14.0-beta.1 (ADR-014).
 */

#include <nimrtc/web_transport/web_transport_plugin.hpp>
#include <nimrtc/web_transport/web_transport_session_factory.hpp>
#include <nimrtc/web_transport/web_transport_data_channel.hpp>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>

// Forward declaration of the factory singletons defined in
// web_transport_msquic_session.cpp.  We don't include the .cpp directly
// to keep the public header surface clean.
namespace nimrtc::web_transport {
const IWebTransportSessionFactory* msquic_factory_singleton() noexcept;
namespace test_only {
const IWebTransportSessionFactory* get_msquic_factory() noexcept;
void set_msquic_factory_for_testing(
    const IWebTransportSessionFactory* f) noexcept;
} // namespace test_only
} // namespace nimrtc::web_transport

namespace nimrtc::web_transport {

namespace detail {

void do_register_default_plugins() noexcept {
    // Check that msquic is available before registering.
    // If the msquic submodule is not populated (e.g. build before
    // `git submodule update --init`), the factory would fail at
    // session creation time.  We log a warning but still register
    // so the engine can produce a clear "factory not available" error
    // at session-creation time rather than at registry-lookup time.
    // ADR-014 Criterion 5 (maintenance check) is enforced separately
    // by interop/v0_14_msquic_spike/ at build time.
    static const struct Registrar {
        Registrar() {
            // ---- 1. WebTransport session factory (msquic backend) ----
            const IWebTransportSessionFactory* f = msquic_factory_singleton();
            nimrtc::core::PluginRegistry::instance().register_web_transport(
                std::string_view{f->id()}, f);
            test_only::set_msquic_factory_for_testing(f);

            // ---- 2. DataChannel factory (WebTransport backend) ----
            // Registered as id "web_transport" so the engine can resolve
            // EngineConfig::datachannel_backend = "web_transport".
            static const WebTransportDataChannelFactory dc_factory{};
            nimrtc::core::PluginRegistry::instance().register_datachannel(
                std::string_view{dc_factory.id()}, &dc_factory);

            nimrtc::core::log::Logger::instance().info(
                std::string("nimrtc::web_transport: default plugins registered (")
                + "session id=" + std::string(f->id())
                + ", datachannel id=" + std::string(dc_factory.id())
                + ", msquic backend, via core::PluginRegistry)");
        }
    } s_registrar;
    (void)s_registrar;
}

} // namespace detail

void register_default_plugins() noexcept {
    static const int once = []() {
        detail::do_register_default_plugins();
        return 1;
    }();
    (void)once;
}

} // namespace nimrtc::web_transport
