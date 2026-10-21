/**
 * @file nimrtc/transport/transport_strategy.hpp
 * @brief Named transport strategies — the bridge from Profile v1.1
 *        `transport.strategy` to a concrete `TransportRequirements`.
 *
 * Companion to `docs/plan/transport-strategies.md` (PR1 of v0.12.0).
 * This header is the **runtime counterpart** of the assembly-layer
 * `TransportSection` (in `src/modules/assembly/include/.../assembly.hpp`):
 *
 *   - `assembly::TransportSection` owns the JSON-serialisable shape
 *     (one of the five named strategies + media/control sub-blocks).
 *   - `TransportRequirements` (in `transport_selector.hpp`) is the
 *     Selector's input — a flat set of capability flags.
 *   - This header bridges the two: a `strategy_by_name()` lookup maps
 *     a Profile-declared strategy id to a `TransportRequirements` value
 *     that the Selector can consume.
 *
 * Round-trip contract:
 *   - The five strategy names here MUST match the `enum` in
 *     `profiles/schema/profile-v1.1.json`. The test
 *     `test_transport_strategy.StrategyNamesMatchProfileSchemaEnum`
 *     hard-fails if the two diverge.
 *   - The five `kStrategy*` constants here are the **default**
 *     TransportRequirements for each strategy. Profiles can override
 *     the per-instance values (e.g. `max_p99_ms`) on top of the
 *     default; `strategy_by_name()` returns the base, callers
 *     compose per-instance overrides.
 *
 * Why not a `std::unordered_map<std::string_view, TransportRequirements>`?
 *   - The constants are `inline const` so they live in `.rodata`, not
 *     on the heap.
 *   - The lookup is a small `if-else` chain; a hashmap would be
 *     premature optimisation for 5 entries.
 */
#pragma once


// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nimrtc/transport/transport_selector.hpp>

namespace nimrtc {

// ---------------------------------------------------------------------------
// Default TransportRequirements per strategy
// ---------------------------------------------------------------------------
//
// `inline const` (not `inline constexpr`) because `TransportRequirements`
// contains `std::optional<std::string>` (`std::string` is not a literal
// type in C++17/20, so the struct isn't either). The values are still
// compile-time-initialised in practice — the linker puts them in `.rodata`.

inline const TransportRequirements kStrategyBrowserCall{
    .needs_browser_interop   = true,
    .needs_high_freq_control = false,
    .max_p99_ms              = 200,
};

inline const TransportRequirements kStrategyTeleopUltraLowLatency{
    .needs_browser_interop   = false,
    .needs_high_freq_control = true,
    .max_p99_ms              = 50,
};

inline const TransportRequirements kStrategyAgentGateway{
    .needs_browser_interop   = true,
    .needs_high_freq_control = true,
    .max_p99_ms              = 100,
};

inline const TransportRequirements kStrategySfuRelay{
    .needs_browser_interop   = true,
    .needs_high_freq_control = false,
    .max_p99_ms              = 200,
};

inline const TransportRequirements kStrategyCrossNetworkPeer{
    .needs_browser_interop   = true,
    .needs_high_freq_control = false,
    .max_p99_ms              = 150,
};

// ---------------------------------------------------------------------------
// strategy_by_name — name → TransportRequirements lookup
// ---------------------------------------------------------------------------
//
// Returns the default `TransportRequirements` for the named strategy.
// `name` is one of the five ids from `docs/plan/transport-strategies.md` §2:
//
//   - "browser-call"
//   - "teleop-ultra-low-latency"
//   - "agent-gateway"
//   - "sfu-relay"
//   - "cross-network-peer"
//
// An empty `name` returns the engine compile-time default
// (currently `kStrategyBrowserCall`). An unknown name throws
// `std::invalid_argument` with a message listing the five valid ids —
// the loader is the typical caller, and an unknown name is a
// configuration error worth loud-failing on.
//
// The return type is `const TransportRequirements&` (not by value) so
// callers can take a reference and override individual fields without
// copying the whole struct:
//
//   auto req = strategy_by_name("agent-gateway");
//   req.max_p99_ms = 80;   // tighter than the 100 ms default
//   selector.select(req);
//
NIMRTC_API_STABLE inline const TransportRequirements& strategy_by_name(std::string_view name) {
    if (name.empty()) {
        return kStrategyBrowserCall;  // engine default
    }
    if (name == "browser-call") {
        return kStrategyBrowserCall;
    }
    if (name == "teleop-ultra-low-latency") {
        return kStrategyTeleopUltraLowLatency;
    }
    if (name == "agent-gateway") {
        return kStrategyAgentGateway;
    }
    if (name == "sfu-relay") {
        return kStrategySfuRelay;
    }
    if (name == "cross-network-peer") {
        return kStrategyCrossNetworkPeer;
    }
    throw std::invalid_argument(
        "transport::strategy_by_name: unknown strategy name '" +
        std::string{name} +
        "'. Valid: browser-call, teleop-ultra-low-latency, "
        "agent-gateway, sfu-relay, cross-network-peer");
}

// Number of named strategies; useful for tests and for ranged-for over
// the strategy catalog (e.g. CLI tools that list available strategies).
inline constexpr std::size_t kStrategyCount = 5;

// Ordered list of strategy names (matches the if-else in strategy_by_name
// and the JSON Schema enum in profiles/schema/profile-v1.1.json).
inline constexpr std::string_view kStrategyNames[kStrategyCount] = {
    "browser-call",
    "teleop-ultra-low-latency",
    "agent-gateway",
    "sfu-relay",
    "cross-network-peer",
};

// ---------------------------------------------------------------------------
// ProfileTransportInput — transport-side mirror of assembly::TransportSection
// ---------------------------------------------------------------------------
//
// Why a mirror instead of just including <nimrtc/assembly/assembly.hpp>?
//   - `transport/` is a peer of `assembly/` — neither depends on the
//     other. Pulling `assembly::TransportSection` into the transport
//     header would create a one-way dep and break the
//     "engine is the composition root" invariant (see transport-selection.md
//     §2).
//   - The fields the bridge needs are 5 strings + 4 optionals. A
//     hand-rolled mirror is simpler than wrestling with cross-module
//     header includes (and the assembly module is heavier — it pulls in
//     nlohmann/json transitively via its implementation, which we don't
//     want in `transport_strategy.hpp`).
//   - The mirror stays **structurally identical** to
//     `assembly::TransportSection` so application code can write:
//         auto req = transport::requirements_from_profile_input(
//             transport::profile_input_from_assembly(profile.transport));
//     — i.e. a one-line adapter near the call site (or a private helper
//     in the engine) maps between the two shapes.
//
// Field mapping (Profile v1.1 JSON key → ProfileTransportInput field):
//   - `transport.strategy`            → `strategy`
//   - `transport.media.max_p99_ms`    → `media_max_p99_ms`
//   - `transport.control.enabled`     → `control_enabled`
//   - `transport.control.max_p99_ms`  → `control_max_p99_ms`
//   - `transport.control.freq_hz`     → `control_freq_hz`
//   - `transport.control.reliability` → `control_reliability`
//   - `transport.control.ttl_ms`      → `control_ttl_ms`
// (The two `stack` fields on the assembly side are intentionally NOT
// mirrored here — the Selector decides the stack id from the strategy
// name, not from the JSON. `transport.media.stack` and
// `transport.control.stack` are diagnostic / override hooks for the
// §5.3 rule engine, not inputs to the strategy mapping.)
// ---------------------------------------------------------------------------
struct NIMRTC_API_STABLE ProfileTransportInput {
    /// Strategy id (one of the 5 named use cases). Empty = engine default
    /// (currently `browser-call`).
    std::string_view strategy = {};

    /// Optional p99 budget override for the media plane.
    std::optional<int> media_max_p99_ms = std::nullopt;

    /// Whether a side-stack for the control plane is enabled. When false,
    /// the Selector uses the media stack's DC plane (SCTP) for control.
    bool control_enabled = false;

    /// Optional p99 budget override for the control plane.
    std::optional<int> control_max_p99_ms = std::nullopt;

    /// Optional expected control-message rate (Hz). Used by ARQ impls to
    /// pre-size the send window.
    std::optional<int> control_freq_hz = std::nullopt;

    /// Optional control reliability model. Accepted values:
    /// "reliable_ordered" (default), "partial_reliable", "unreliable".
    std::string_view control_reliability = {};

    /// Optional time-to-live for partial_reliable messages (ms).
    std::optional<int> control_ttl_ms = std::nullopt;
};

// ---------------------------------------------------------------------------
// requirements_from_profile_input — bridge from Profile v1.1 to TransportReq
// ---------------------------------------------------------------------------
//
// Resolves the strategy name via `strategy_by_name()` (so an unknown
// name throws `std::invalid_argument` with the canonical 5-id list), then
// composes a `TransportRequirements` value that the Selector can consume.
//
// The composition rule (Slice 7.6 PR3 — `docs/plan/transport-strategies.md`):
//
//   1. Start with the strategy's default `TransportRequirements`.
//
//   2. `media_max_p99_ms` (if set) overrides the default `max_p99_ms`.
//      The override is the application saying "I want this strategy
//      with a tighter / looser p99 budget than the strategy default".
//      Strategies where the default already has a value (all 5) accept
//      the override. The Selector records the override but does not
//      enforce it (v0.13+ may add a validator; see transport-strategies
//      §6 #4).
//
//   3. The control-plane fields do NOT translate into booleans on
//      `TransportRequirements` directly — they are recorded on the
//      `TransportSession` after `select()` returns. This split keeps
//      `TransportRequirements` a flat set of capability flags (the
//      Selector's contract) while still allowing Profile JSONs to
//      declare the full p99 / freq / reliability triple. The engine
//      uses `requirements_from_profile_input` for the Selector's input
//      and re-reads the per-instance fields from the original
//      `ProfileTransportInput` when wiring the resulting session.
//
// Return-by-value (not by reference): the function constructs a fresh
// `TransportRequirements` from the per-instance override, so the result
// is independent of the canonical `kStrategy*` constants. Caller can
// safely take ownership and override further fields.
//
// Throws `std::invalid_argument` (with the canonical 5-id list) if
// `input.strategy` is non-empty and not one of the five known ids. An
// empty `input.strategy` is silently treated as the engine default
// ("browser-call") — the same convention as `strategy_by_name("")`.
//
NIMRTC_API_STABLE inline TransportRequirements
requirements_from_profile_input(const ProfileTransportInput& input) {
    // Step 1: resolve strategy name. Empty -> browser-call default.
    // strategy_by_name throws std::invalid_argument on unknown names.
    const auto& base = strategy_by_name(input.strategy);

    // Step 2: compose the result. We start with a copy of the base
    // requirements and apply per-instance overrides.
    TransportRequirements out = base;

    if (input.media_max_p99_ms.has_value()) {
        out.max_p99_ms = *input.media_max_p99_ms;
    }
    // The control-plane booleans (control_enabled, control_reliability,
    // control_freq_hz, control_ttl_ms) are NOT part of TransportRequirements
    // today — they are recorded on TransportSession post-select(). The
    // engine reads them from the original `input` when it wires the
    // session. Future Slice-7.x work may grow TransportRequirements to
    // include `needs_partial_reliable_control` etc.; for v0.12.0, we keep
    // the bridge intentionally narrow.
    (void)input.control_enabled;
    (void)input.control_max_p99_ms;
    (void)input.control_freq_hz;
    (void)input.control_reliability;
    (void)input.control_ttl_ms;

    return out;
}

}  // namespace nimrtc
