/**
 * @file nimrtc/transport/strategy_validator.hpp
 * @brief v1.2 M2 — StrategyValidator (opt-in p99 budget enforcement).
 *
 * The CapabilitySelector records `TransportRequirements::max_p99_ms` but
 * never enforces it (transport-strategies.md section 6 item 4). This header
 * adds the enforcement point as a pure, header-only helper so the engine
 * (or a future `open()` path) can opt in without changing the Selector:
 *
 *   - Default (`enforce == false`): over-budget only emits a WARN log and
 *     returns 0 (record-only, existing behaviour preserved).
 *   - Opt-in (`enforce == true`): over-budget emits a WARN log and returns
 *     `core::kEngineConfig` (0x1010) so the caller can fail `open()`.
 *
 * Additive-only: new header, new `NIMRTC_API_UNSTABLE` symbols, no change
 * to any existing header or factory. Depends only on core (log +
 * engine_errors), honouring L1-L4 (transport never includes engine heads).
 */
#pragma once

#include <nimrtc/core/api_version.hpp>
#include <nimrtc/core/engine_errors.hpp>
#include <nimrtc/core/log.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace nimrtc::transport {

/** v1.2 M2 validator configuration (additive, unstable). */
struct NIMRTC_API_UNSTABLE StrategyValidatorConfig {
    /** False (default): record-only. True: over-budget fails open(). */
    bool enforce = false;
};

/**
 * Validate a measured p99 against the strategy budget.
 *
 * @param max_p99_ms      Strategy budget (`TransportRequirements::max_p99_ms`).
 *                        Empty = no cap, always returns 0.
 * @param measured_p99_ms Observed end-to-end p99 in ms.
 * @param cfg             Opt-in enforcement flag (default record-only).
 * @param stack_id        Diagnostic id logged on breach (e.g. "webrtc-classic").
 * @return 0 when within budget (or record-only mode); otherwise
 *         `core::kEngineConfig` when `cfg.enforce` is true.
 */
NIMRTC_API_UNSTABLE inline std::uint32_t validate_p99_budget(
    std::optional<int> max_p99_ms, int measured_p99_ms,
    StrategyValidatorConfig cfg = {},
    std::string_view stack_id = {}) noexcept {
    if (!max_p99_ms.has_value()) return 0;
    if (measured_p99_ms <= *max_p99_ms) return 0;
    std::string msg = std::string("transport: p99 budget exceeded on stack '") +
                      std::string(stack_id) + "': measured " +
                      std::to_string(measured_p99_ms) + " ms > budget " +
                      std::to_string(*max_p99_ms) + " ms" +
                      (cfg.enforce ? " (enforced)" : " (record-only)");
    nimrtc::core::log::Logger::instance().warn(msg);
    return cfg.enforce ? nimrtc::core::kEngineConfig : 0;
}

}  // namespace nimrtc::transport
