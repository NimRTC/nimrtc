/**
 * @file nimrtc/transport/transport_plugin.hpp
 * @brief Public registration entry points for the transport Selector
 *        (Slice 7) and the default ITransportStackFactory (Slice 8).
 *
 * Slice 7 (transport-selection.md §6.4): `register_default_selector()`
 * constructs a process-static `CapabilitySelector` so the symbol has a
 * defined destructor location.
 *
 * Slice 8 (v0.10.2): `register_default_plugins()` publishes the
 * `CapabilitySelectorStackFactory` (id="default") through the typed
 * `core::PluginRegistry::register_transport_stack(...)` hook so the
 * Slice 8 engine integration has a non-null factory pointer to
 * resolve. Slice 7.5 will replace this with concrete
 * `WebRtcClassicStackFactory` + `RawUdpArqStackFactory` impls.
 *
 * Both entry points follow the Meyer's-singleton latch pattern (same
 * as `nimrtc::ice::register_default_plugins()` in ice.cpp) so the
 * symbols survive MSVC static-link stripping.
 *
 * ## Namespace placement
 *
 * These entry points live in the inner `nimrtc::transport` namespace
 * (matching the forward declaration in `core::registry.hpp`) rather
 * than the outer `nimrtc` namespace. The Slice 7 code accidentally
 * placed them in `nimrtc`; Slice 8 (v0.10.2) corrects the namespace
 * mismatch so the `kDefaultRegistrars[]` table entry
 * `&nimrtc::transport::register_default_plugins` resolves at link
 * time. The selector entry point is kept in `nimrtc` for
 * backward-compat with pre-Slice-8 consumers.
 */
#pragma once

namespace nimrtc {

/** Register the default CapabilitySelector (id "default").
 *
 *  Idempotent — multiple calls are safe (Meyer's-singleton latch in
 *  transport_plugin.cpp). */
void register_default_selector() noexcept;

} // namespace nimrtc

namespace nimrtc::transport {

/** Register the default ITransportStackFactory (id "default") with
 *  `nimrtc::core::PluginRegistry::register_transport_stack(...)`.
 *
 *  Slice 8 (v0.10.2) entry point. Idempotent — multiple calls are safe
 *  (Meyer's-singleton latch). After this call,
 *  `core::PluginRegistry::instance().get_transport_stack("default")`
 *  returns a non-null pointer to the shell `CapabilitySelectorStackFactory`
 *  (see `default_transport_stack_factory.cpp` for the contract).
 *
 *  Slice 7.5 will replace this shell factory with concrete
 *  `WebRtcClassicStackFactory` (id="webrtc-classic") +
 *  `RawUdpArqStackFactory` (id="raw-udp-arq") impls; the "default" id
 *  remains for backward-compat with Slice 7 tests. */
void register_default_plugins() noexcept;

} // namespace nimrtc::transport
