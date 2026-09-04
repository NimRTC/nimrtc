/**
 * @file nimrtc/plugins/registry.hpp
 * @brief PluginRegistry backward-compatibility shim.
 *
 * Per ARCHITECTURE.md Layout Invariant 6, PluginRegistry now lives in
 * nimrtc::core:: (see <nimrtc/core/registry.hpp>).
 * This header re-exports it into the plugins:: namespace so existing code
 * that includes "nimrtc/plugins/registry.hpp" continues to compile without change.
 *
 * New code should include the canonical location directly:
 *   #include "nimrtc/core/registry.hpp"
 *
 * @deprecated Prefer including <nimrtc/core/registry.hpp> directly.
 */

#ifndef NIMRTC_PLUGINS_REGISTRY_HPP
#define NIMRTC_PLUGINS_REGISTRY_HPP

// Canonical location — PluginRegistry lives in nimrtc::core:: per Layout Invariant 6
#include "nimrtc/core/registry.hpp"

#endif // NIMRTC_PLUGINS_REGISTRY_HPP
