/**
 * @file nimrtc/transport/transport_plugin.hpp
 * @brief Public registration entry points for the transport Selector
 *        (Slice 7) and the default ITransportStackFactory (Slice 8 + Slice 7.5).
 *
 * Slice 7 (transport-selection.md §6.4): `register_default_selector()`
 * constructs a process-static `CapabilitySelector` so the symbol has a
 * defined destructor location.
 *
 * Slice 8 (v0.10.2): `register_default_plugins()` publishes the
 * `CapabilitySelectorStackFactory` (id="default") through the typed
 * `core::PluginRegistry::register_transport_stack(...)` hook so the
 * Slice 8 engine integration has a non-null factory pointer to
 * resolve. This is a shell factory (component accessors return
 * null-instance singletons; start()/close() are no-ops).
 *
 * Slice 7.5 (v0.10.x): `register_default_plugins()` additionally
 * publishes `WebRtcClassicStackFactory` (id="webrtc-classic") through
 * the same typed registry hook. This is the real factory: it resolves
 * ICE + DTLS + RTP + SCTP (stub) from the PluginRegistry and wires
 * them into a `WebRtcClassicStack`. The CapabilitySelector's §5.3
 * rule 1 ("needs_browser_interop == true") maps to id "webrtc-classic",
 * so production code using the selector gets the real stack.
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

/** Register the default ITransportStackFactory entries with
 *  `nimrtc::core::PluginRegistry::register_transport_stack(...)`.
 *
 *  Slice 8 (v0.10.2) + Slice 7.5 entry point. Idempotent — multiple
 *  calls are safe (Meyer's-singleton latch). After this call:
 *    - `get_transport_stack("default")` → shell CapabilitySelectorStackFactory
 *    - `get_transport_stack("webrtc-classic")` → real WebRtcClassicStackFactory
 *
 *  The CapabilitySelector's §5.3 rule 1 resolves "webrtc-classic" for
 *  browser-interop profiles, so production code gets the real stack.
 *  The "default" id is preserved for backward-compat with Slice 7 tests. */
void register_default_plugins() noexcept;

} // namespace nimrtc::transport
