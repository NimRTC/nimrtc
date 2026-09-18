/**
 * @file src/raw_udp/src/raw_udp_plugin.cpp
 * @brief Public registration entry point for the raw-udp bypass module.
 *
 * Slice 6 (transport-selection §6.3) — promoted in Slice 8 (v0.10.2)
 * to publish `ArqRawUdpFactory` through the typed
 * `nimrtc::core::PluginRegistry::register_raw_udp_datagram(id, factory*)`
 * hook (see `docs/plan/transport-selection.md` §6.5 follow-up gap).
 *
 * The registration logic is intentionally minimal — the engine / Selector
 * / ITransportStack composition will look up the factory by id "arq"
 * through the registry rather than going through a process-local cache.
 *
 * MSVC static-link workaround: not `inline` so the symbol lands in
 * nimrtc_raw_udp.lib.
 */
#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/raw_udp/raw_udp_factory.hpp>
#include <nimrtc/raw_udp/raw_udp_plugin.hpp>

namespace nimrtc::raw_udp {

namespace detail {

void do_register_default_plugins() noexcept {
    // Slice 8 (v0.10.2): publish the factory through the typed
    // registry hook so the engine / Selector / future Profile loader
    // can look it up by id "arq" via
    // `core::PluginRegistry::get_raw_udp_datagram("arq")`. The hook is
    // idempotent under repeated calls — a second registration overwrites
    // the first in place (TypedRegistry::register_one semantics).
    //
    // The factory is now a public IRawUdpFactory (Slice 8 promotion —
    // see nimrtc/raw_udp/raw_udp_factory_iface.hpp), so the upcast to
    // the typed registry slot is implicit.
    static const ArqRawUdpFactory s_factory{};
    nimrtc::core::PluginRegistry::instance().register_raw_udp_datagram(
        std::string_view{s_factory.id()}, &s_factory);
    NIMRTC_LOG_INFO("raw_udp: default plugin registered (id=\""
                    << s_factory.id() << "\", via Slice 8 typed registry hook)");
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
