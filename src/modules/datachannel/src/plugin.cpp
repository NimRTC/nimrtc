/**
 * @file src/modules/datachannel/src/plugin.cpp
 * @brief datachannel::register_default_plugins() — Meyers-singleton
 *        entry point that publishes SctpDataChannelFactory under
 *        `id="sctp"` in `core::PluginRegistry`.
 *
 * Mirrors the same idiom as
 *   - `nimrtc::sctp::register_default_plugins()` (src/sctp/src/sctp_plugin.cpp)
 *   - `nimrtc::ice::register_default_plugins()`  (src/modules/ice/src/ice.cpp)
 *   - `nimrtc::audio3a::register_default_plugins()`
 *     (src/modules/audio3a/src/audio3a_plugin.cpp)
 *
 * ## Idempotency
 *
 * Static-local latch (Meyers singleton). First call registers the
 * factory; subsequent calls are no-ops. Safe to call from any TU.
 *
 * ## MSVC static-link workaround
 *
 * `register_default_plugins()` is declared **non-inline** in
 * `sctp_data_channel.hpp` so the symbol is guaranteed to land in
 * `nimrtc_datachannel.lib`. Without this, MSVC's linker may strip
 * the registrar from the static archive (no ODR-use → no symbol
 * pull). See the ice.cpp / audio3a_plugin.cpp header comments for
 * the full rationale.
 */

#include <nimrtc/datachannel/sctp_data_channel.hpp>

#include <string_view>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>

namespace nimrtc::datachannel {

namespace detail {

void do_register_default_plugins() noexcept {
    // Static ⇒ address stable for process lifetime; runs once at first call.
    static const struct Registrar {
        Registrar() {
            static SctpDataChannelFactory s_factory{};
            // Publish under the canonical id "sctp" — engine / Profile
            // loader resolve it via
            //   core::PluginRegistry::instance().get_datachannel("sctp")
            nimrtc::core::PluginRegistry::instance().register_datachannel(
                std::string_view{s_factory.id()}, &s_factory);
        }
    } s_registrar;
    (void)s_registrar;
}

} // namespace detail

// Non-inline (declared in sctp_data_channel.hpp) so the symbol is
// guaranteed to be emitted inside nimrtc_datachannel.lib for static
// consumers that link via PluginRegistry.
void register_default_plugins() noexcept {
    static const int once = []() {
        core::log::Logger::instance().info(
            "nimrtc::datachannel: registering default plugins "
            "(id=\"sctp\" [usrsctp-backed DataChannel via ISctpSocket seam])");
        detail::do_register_default_plugins();
        return 1;
    }();
    (void)once;
}

} // namespace nimrtc::datachannel
