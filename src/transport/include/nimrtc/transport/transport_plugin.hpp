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
 * Slice 7.6 (v0.12.0): `register_default_plugins()` additionally
 * publishes `RawUdpArqStackFactory` (id="raw-udp-arq") through the
 * same typed registry hook. This is the control-plane factory: it
 * exposes a real `IRawUdpDatagram` for high-frequency control traffic.
 * The CapabilitySelector's §5.3 rules 1b / 2 / 4 (high-freq-control
 * variants) map to this factory for the control_stack slot.
 *
 * Slice 7.6 PR3 (v0.12.0): `make_transport_instance()` — the
 * canonical engine-facing entry point that constructs a
 * `TransportSession` from the Selector's resolved (media_factory_id,
 * control_factory_id) pair by looking up the factories in the registry
 * and calling `create(cfg)`. This replaces the engine's pre-PR3
 * manual construction path (direct `reg.get_ice_transport()` /
 * `reg.get_dtls_session()` calls).
 *
 * Both registration entry points follow the Meyer's-singleton latch
 * pattern (same as `nimrtc::ice::register_default_plugins()` in ice.cpp)
 * so the symbols survive MSVC static-link stripping.
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

#include <nimrtc/core/api_version.hpp>
#include <string_view>

#include <nimrtc/transport/transport_stack.hpp>

namespace nimrtc {

/** Register the default CapabilitySelector (id "default").
 *
 *  Idempotent — multiple calls are safe (Meyer's-singleton latch in
 *  transport_plugin.cpp). */
NIMRTC_API_STABLE
void register_default_selector() noexcept;

} // namespace nimrtc

namespace nimrtc::transport {

/** Register the default ITransportStackFactory entries with
 *  `nimrtc::core::PluginRegistry::register_transport_stack(...)`.
 *
 *  Slice 8 (v0.10.2) + Slice 7.5 + Slice 7.6 entry point. Idempotent —
 *  multiple calls are safe (Meyer's-singleton latch). After this call:
 *    - `get_transport_stack("default")`         → shell CapabilitySelectorStackFactory
 *    - `get_transport_stack("webrtc-classic")`  → real WebRtcClassicStackFactory
 *    - `get_transport_stack("raw-udp-arq")`     → real RawUdpArqStackFactory
 *
 *  The CapabilitySelector's §5.3 rule 1 resolves "webrtc-classic" for
 *  browser-interop profiles, so production code gets the real stack.
 *  The "default" id is preserved for backward-compat with Slice 7 tests. */
NIMRTC_API_STABLE
void register_default_plugins() noexcept;

/** Construct a `TransportSession` from the Selector's resolved factory
 *  ids (Slice 7.6 PR3 — v0.12.0).
 *
 *  This is the **canonical engine-facing entry point** for the
 *  v0.12.0 transport-strategy layer. It replaces the pre-PR3 manual
 *  path that constructed ICE / DTLS / RTP / SCTP via separate
 *  `core::PluginRegistry::get_*()` calls.
 *
 *  Behaviour:
 *    1. Looks up the media factory via
 *       `core::PluginRegistry::get_transport_stack(media_factory_id)`.
 *    2. If found, calls `factory->create(cfg)` and stores the result
 *       in `session.media_stack`.  `session.media_stack_id` mirrors
 *       `media_factory_id` so diagnostics can read what was chosen
 *       even if the factory returned null.
 *    3. If `control_factory_id` is non-empty, repeats (1)+(2) for the
 *       control factory and populates `session.control_stack` /
 *       `session.control_stack_id`.
 *    4. Missing factories are logged and `session.media_stack` /
 *       `session.control_stack` are left as null (loud-fail at the
 *       next ICE/DTLS call rather than crashing the engine).
 *
 *  The `cfg.dtls_id`, `cfg.ice_id`, etc. fields on the `StackConfig`
 *  are read by the factory's `create()` to resolve the underlying
 *  ICE / DTLS / RTP / SCTP / raw_UDP plugins from their respective
 *  typed registry slots.  The engine populates `cfg` from its own
 *  `EngineConfig::dtls_name` etc. (see `NimRTCEngine::init_modules_once`).
 *
 *  Not thread-safe. Call once per engine during `init_modules_once()`.
 *
 *  @param media_factory_id   The Selector's resolved media stack id
 *                            (e.g. "webrtc-classic" for all 4
 *                            browser-interop strategies, "raw-udp-arq"
 *                            for `teleop-ultra-low-latency` per §5.3
 *                            rule 2). Must be non-empty.
 *  @param control_factory_id The Selector's resolved control stack id
 *                            (e.g. "raw-udp-arq" for
 *                            `agent-gateway` / `teleop-ultra-low-latency`,
 *                            empty for the other 3 strategies). May be
 *                            empty.
 *  @param cfg                Per-stack backend selection. The factory's
 *                            `create()` reads this to resolve ICE / DTLS
 *                            / RTP / SCTP / raw_UDP from the registry.
 *  @return A `TransportSession` with `media_stack` and (optionally)
 *          `control_stack` populated.  Either stack may be null if
 *          the corresponding factory id is not registered — callers
 *          MUST check before dereferencing component accessors.
 */
NIMRTC_API_STABLE
nimrtc::TransportSession make_transport_instance(
    std::string_view media_factory_id,
    std::string_view control_factory_id,
    const nimrtc::StackConfig& cfg) noexcept;

} // namespace nimrtc::transport
