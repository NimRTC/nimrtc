/**
 * @file nimrtc/bwe/bwe.hpp
 * @brief Bandwidth Estimator — concrete AIMD implementation (P1).
 *
 * Public API: this header re-exports `nimrtc::plugins::BweConfig /
 * BweFeedback / BweEstimate / IBwe / IBweFactory` so callers that link
 * only `nimrtc::bwe` see the same types everyone else sees. The concrete
 * default implementation is `nimrtc::bwe::Bwe`, which inherits from
 * `nimrtc::plugins::IBwe`.
 *
 * ## Algorithm
 *
 * AIMD (Additive Increase Multiplicative Decrease). P1 baseline:
 *   - No loss  -> increase bitrate by `increase_bps` per second.
 *   - Loss     -> multiply bitrate by `decrease_factor`.
 *   - REMB higher than local -> trust receiver (sanity-clamped).
 *   - EMA smoothing to avoid oscillation.
 *
 * P3: replaced by Goog-CC compatible mainline as the default (see §6 /
 * §13 P3 milestone). BBR stays internal experiment.
 *
 * Design notes:
 *   - BWE is a pure consumer of feedback signals. It does NOT send packets.
 *   - All estimates are bits per second (bps).
 *   - The class is reentrant (no shared state) — concurrent calls from a
 *     single engine thread are safe.
 */

#pragma once

#include <cstdint>
#include <memory>

#include <nimrtc/plugins/bwe.hpp>   // pulls BweConfig / BweFeedback / BweEstimate / IBwe

// Re-export plugin types under nimrtc::bwe:: for backwards-compatibility
// (callers can keep writing `bwe::Config`, `bwe::Feedback`, etc.).
namespace nimrtc::bwe {
    using Config   = plugins::BweConfig;
    using Feedback = plugins::BweFeedback;
    using Estimate = plugins::BweEstimate;
    using IBwe     = plugins::IBwe;
}

// =============================================================================
// nimrtc::bwe — concrete default implementation
// =============================================================================
namespace nimrtc::bwe {

// ---------------------------------------------------------------------------
// Bwe — AIMD default (P1)
// ---------------------------------------------------------------------------

/** AIMD bandwidth estimator — implements `plugins::IBwe`.
 *
 *  Used by the default `"aimd"` plugin id. Goog-CC compatible mainline
 *  (P3) will replace this as the default; the existing tests in
 *  `modules/bwe/tests/test_bwe.cpp` must continue to pass for the new
 *  default.
 */
class Bwe final : public plugins::IBwe {
public:
    /** Construct with a BweConfig (defaults match the plugin header). */
    explicit Bwe(plugins::BweConfig config = {});
    ~Bwe() override;

    Bwe(const Bwe&)            = delete;
    Bwe& operator=(const Bwe&) = delete;
    Bwe(Bwe&&)                 noexcept;
    Bwe& operator=(Bwe&&)      noexcept;

    // plugins::IPlugin ---------------------------------------------------
    const char* name() const noexcept override;
    plugins::Status open() noexcept override;
    void close() noexcept override;

    // plugins::IBwe -----------------------------------------------------
    void on_feedback(plugins::BweFeedback feedback) noexcept override;
    plugins::BweEstimate estimate() const noexcept override;
    void update_config(plugins::BweConfig config) noexcept override;
    plugins::BweConfig config() const noexcept override;
    void reset() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nimrtc::bwe
