/**
 * @file nimrtc/raw_udp/raw_udp_plugin.hpp
 * @brief Public registration entry point for the raw-udp bypass module.
 *
 * Slice 6 (transport-selection §6.3). Calling register_default_plugins()
 * registers an ArqRawUdpFactory under id "arq" so that the Slice 7
 * Selector (and any future PluginRegistry::get_raw_udp() lookup) can
 * resolve it.
 *
 * Idempotent — multiple calls are safe; the second and subsequent calls
 * are no-ops (Meyer's-singleton latch in raw_udp_plugin.cpp).
 */
#pragma once

#include <nimrtc/core/api_version.hpp>   // NIMRTC_API_STABLE

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
namespace nimrtc::raw_udp {

/** Register the built-in "arq" raw-UDP datagram factory. Call once at
 *  program startup before any consumer resolves the "arq" factory.
 *
 *  MSVC static-link workaround: not `inline` so the symbol is guaranteed
 *  in nimrtc_raw_udp.lib (matches nimrtc::ice::register_default_plugins
 *  convention). */
NIMRTC_API_STABLE void register_default_plugins() noexcept;

} // namespace nimrtc::raw_udp
