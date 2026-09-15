/**
 * @file src/raw_udp/src/raw_udp_plugin.cpp
 * @brief Public registration entry point for the raw-udp bypass module.
 *
 * Slice 6 (transport-selection §6.3). The registration logic is
 * intentionally minimal — Slice 7 will add a `register_raw_udp(id, factory)`
 * entry point on the PluginRegistry, and at that point this function
 * will register the ArqRawUdpFactory there. For Slice 6 we just build
 * a process-static factory instance and log the registration so consumers
 * can see the slice is alive.
 *
 * MSVC static-link workaround: not `inline` so the symbol lands in
 * nimrtc_raw_udp.lib.
 */
#include <nimrtc/core/log.hpp>
#include <nimrtc/raw_udp/raw_udp_factory.hpp>
#include <nimrtc/raw_udp/raw_udp_plugin.hpp>

namespace nimrtc::raw_udp {

namespace detail {

void do_register_default_plugins() noexcept {
    // Process-static instance. We don't push it into core::PluginRegistry
    // yet because no `get_raw_udp` slot exists — that lands with Slice 7.
    // For now this just makes sure the factory is constructed at least
    // once per process so static-linkers strip the .obj from the .lib.
    static const ArqRawUdpFactory s_factory{};
    NIMRTC_LOG_INFO("raw_udp: default plugin registered (id=\""
                    << s_factory.id() << "\")");
}

} // namespace detail

void register_default_plugins() noexcept {
    // Idempotent Meyer's-singleton latch (matches the ice.cpp / datachannel.cpp
    // pattern, which dodges MSVC's static-local-stripping).
    static const int once = []() {
        detail::do_register_default_plugins();
        return 1;
    }();
    (void)once;
}

} // namespace nimrtc::raw_udp
