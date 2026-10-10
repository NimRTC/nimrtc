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
inline const TransportRequirements& strategy_by_name(std::string_view name) {
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

}  // namespace nimrtc
