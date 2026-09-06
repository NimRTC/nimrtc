/**
 * @file src/modules/datachannel/src/datachannel.cpp
 * @brief DataChannel module P1 stub.
 *
 * P1: register_default_plugins() is a no-op.
 * P2: call nimrtc::datachannel::register_default_plugins() from the
 *     usrsctp adapter .cpp file.
 */

#include <nimrtc/core/registry.hpp>

namespace nimrtc::datachannel {

/** P1: nothing to register.
 *  P2: register SCTP factory with PluginRegistry. */
void register_default_plugins() noexcept {
    // P1: no concrete implementation — nothing to register.
    // P2: call PluginRegistry::instance().register_datachannel(...)
}

} // namespace nimrtc::datachannel
