/**
 * @file nimrtc/transport/transport_plugin.hpp
 * @brief Public registration entry point for the transport Selector.
 *
 * Slice 7 of the transport-selection plan (transport-selection.md §6.4).
 * `register_default_selector()` pushes a CapabilitySelector instance
 * into the (future) PluginRegistry slot so that the Slice-8 engine
 * integration can `get_selector("default")` without a hard dependency
 * on the Selector class.
 *
 * Slice 7 ships the registration entry point + the Selector itself but
 * NOT the registry slot — adding the slot is Slice-8 work that depends
 * on the Slice-4/5/6 default impls being mergeable. For now the
 * registration function is a process-static singleton that the engine
 * can reach via a known-symbol call (matching the ice/datachannel/raw_udp
 * pattern of "register_default_plugins()" without an enum / registry).
 */
#pragma once

namespace nimrtc {

/** Register the default CapabilitySelector (id "default").
 *
 *  Idempotent — multiple calls are safe (Meyer's-singleton latch in
 *  transport_plugin.cpp). */
void register_default_selector() noexcept;

} // namespace nimrtc
