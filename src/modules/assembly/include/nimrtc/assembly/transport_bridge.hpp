/**
 * @file nimrtc/assembly/transport_bridge.hpp
 * @brief The assembly → transport bridge: `assembly::TransportSection`
 *        → `nimrtc::ProfileTransportInput` → `nimrtc::TransportRequirements`.
 *
 * Architecture remediation plan §4.2 (D2 — dead-module disposition):
 * the `assembly` module used to be *referenced* by the transport layer
 * (the `ProfileTransportInput` doc comment advertised a
 * `transport::profile_input_from_assembly(...)` helper that did not
 * exist) but never *consumed* by it.  This header is the missing
 * adapter — and it lives in the **assembly** module on purpose.
 *
 * ## Why the adapter lives here (and not in `transport/`)
 *
 * `docs/plan/transport-strategies.md` §8 (BDFL review 2026-10-13)
 * fixed the layer order:
 *
 *   - `transport` is a **PAL Slice 2** peer of `core`; it depends on no
 *     module above that slice.
 *   - `assembly` is a **higher-level** layer; it depends on `transport`.
 *
 * Putting the adapter in `transport/` would invert that order (and drag
 * the assembly module's nlohmann/json dependency into the transport
 * header).  Putting it in `assembly/` keeps the dependency arrow
 * pointing the right way: `assembly → transport`.
 *
 * The transport-side `ProfileTransportInput` therefore stays a
 * dependency-free *mirror* of `assembly::TransportSection` (see
 * `nimrtc/transport/transport_strategy.hpp`), and this header is the
 * single place that knows how to copy one into the other.  Because the
 * copy is written down and unit-tested, the two shapes cannot silently
 * drift apart.
 *
 * ## The lifetime contract
 *
 * `ProfileTransportInput` holds `std::string_view`s.  The views produced
 * here point **into** the `TransportSection` / `Profile` that was passed
 * in, so the source object must outlive the returned input:
 *
 * @code
 *   Profile profile = ...;                 // owns the strings
 *   auto input = assembly::to_transport_input(profile);  // views into `profile`
 *   auto req   = transport::requirements_from_profile_input(input);
 * @endcode
 *
 * Copying the `Profile` (or keeping it alive for the whole session) is
 * the caller's job.  The two convenience helpers below
 * (`to_transport_requirements`) do the copy-and-resolve in one step so
 * the common case cannot dangle.
 *
 * ## What is (and is not) mapped
 *
 * Mapped:
 *   - `transport.strategy`                     → `strategy`
 *   - `transport.media.max_p99_ms`             → `media_max_p99_ms`
 *   - `transport.control.enabled`              → `control_enabled`
 *   - `transport.control.max_p99_ms`           → `control_max_p99_ms`
 *   - `transport.control.freq_hz`              → `control_freq_hz`
 *   - `transport.control.reliability`          → `control_reliability`
 *   - `transport.control.ttl_ms`               → `control_ttl_ms`
 *
 * Deliberately **not** mapped:
 *   - `transport.media.stack` / `transport.control.stack` — the Selector
 *     derives the stack ids from the strategy name (§5.3 rule table),
 *     not from the JSON.  They are diagnostic / override hooks for the
 *     rule engine, not inputs to the strategy mapping.
 */
#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>

#include <string_view>

#include <nimrtc/assembly/assembly.hpp>
#include <nimrtc/transport/transport_selector.hpp>
#include <nimrtc/transport/transport_strategy.hpp>

namespace nimrtc::assembly {

// ---------------------------------------------------------------------------
// to_transport_input — TransportSection → ProfileTransportInput
// ---------------------------------------------------------------------------
//
// Copies the seven transport-relevant fields out of a `TransportSection`
// into the transport-side mirror.  The returned views point into
// `section` — see the lifetime contract in the file header.
//
// A default-constructed `TransportSection` (the v1.0 backward-compat
// shape: no `transport` block in the JSON) maps to an input with an
// empty strategy, which `transport::strategy_by_name()` resolves to the
// engine default ("browser-call").
//
NIMRTC_API_STABLE inline ProfileTransportInput
to_transport_input(const TransportSection& section) noexcept {
    ProfileTransportInput in;
    in.strategy            = std::string_view{section.strategy};
    in.media_max_p99_ms    = section.media.max_p99_ms;
    in.control_enabled     = section.control.enabled;
    in.control_max_p99_ms  = section.control.max_p99_ms;
    in.control_freq_hz     = section.control.freq_hz;
    in.control_reliability = std::string_view{section.control.reliability};
    in.control_ttl_ms      = section.control.ttl_ms;
    return in;
}

// ---------------------------------------------------------------------------
// to_transport_input — Profile → ProfileTransportInput
// ---------------------------------------------------------------------------
//
// Convenience overload for the common case: the host already holds a
// `Profile` and wants to feed its `transport` section to the transport
// layer.  The returned views point into `profile`.
//
// @code
//   Profile profile = Builder{}.load_profile("agent-gateway").build();
//   auto input = assembly::to_transport_input(profile);
//   cfg.transport_strategy_name = std::string{input.strategy};
// @endcode
//
NIMRTC_API_STABLE inline ProfileTransportInput
to_transport_input(const Profile& profile) noexcept {
    return to_transport_input(profile.transport);
}

// ---------------------------------------------------------------------------
// to_transport_requirements — Profile → TransportRequirements
// ---------------------------------------------------------------------------
//
// One-shot translation: the Profile's transport section is mapped to the
// transport-side input and immediately resolved into the Selector's
// input.  The returned value is **owned** (a copy of the resolved
// requirements), so it does not borrow from the Profile — the Profile
// may be destroyed immediately after the call.
//
// Throws `std::invalid_argument` when the Profile names a strategy that
// is not one of the five canonical ids; the message comes from
// `transport::strategy_by_name()` and lists the valid ids.
//
// @code
//   Profile profile = Builder{}.load_profile("teleop").build();
//   auto req = assembly::to_transport_requirements(profile);
//   // req.needs_high_freq_control == true  (teleop strategy)
// @endcode
//
NIMRTC_API_STABLE inline TransportRequirements
to_transport_requirements(const Profile& profile) {
    const ProfileTransportInput in = to_transport_input(profile);
    return requirements_from_profile_input(in);
}

// ---------------------------------------------------------------------------
// transport_strategy_name — the id the engine expects
// ---------------------------------------------------------------------------
//
// The strategy id of a Profile, ready to be copied into
// `EngineConfig::transport_strategy_name`.  An empty result means "the
// engine default" (the engine resolves "" to "browser-call").
//
// This is a trivial accessor, but it exists so the *whole* Profile →
// engine hand-off has exactly one documented entry point (see
// `docs/plan/architecture-remediation-plan.md` §4.2).
//
NIMRTC_API_STABLE inline std::string_view
transport_strategy_name(const Profile& profile) noexcept {
    return std::string_view{profile.transport.strategy};
}

}  // namespace nimrtc::assembly
