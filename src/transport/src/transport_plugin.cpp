/**
 * @file src/transport/src/transport_plugin.cpp
 * @brief Default Selector registration entry point.
 *
 * Slice 7 (transport-selection.md §6.4). The Selector registration
 * follows the same Meyer's-singleton latch + process-static-factory
 * pattern as nimrtc::ice::register_default_plugins() so the symbol
 * survives MSVC static-link stripping.
 *
 * NOTE: Slice 7 does NOT yet add a `register_selector(id, factory*)` slot
 * on core::PluginRegistry — that lands with Slice 8 (engine integration),
 * which is the right time to extend the registry. For now the function
 * just constructs a process-static CapabilitySelector instance so its
 * destructor has a defined place to live, and logs the registration.
 */
#include <nimrtc/core/log.hpp>
#include <nimrtc/transport/transport_plugin.hpp>
#include <nimrtc/transport/transport_selector.hpp>

namespace nimrtc {

namespace detail {

void do_register_default_selector() noexcept {
    static const CapabilitySelector s_selector{};
    NIMRTC_LOG_INFO("transport: default selector registered "
                    "(id=\"default\", impl=CapabilitySelector)");
}

} // namespace detail

void register_default_selector() noexcept {
    static const int once = []() {
        detail::do_register_default_selector();
        return 1;
    }();
    (void)once;
}

} // namespace nimrtc
