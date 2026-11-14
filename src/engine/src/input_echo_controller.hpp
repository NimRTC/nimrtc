/**
 * @file src/engine/src/input_echo_controller.hpp
 * @brief PRIVATE engine collaborator: the input-echo decision
 *        (remote-desktop plan E6).
 *
 * ## What this is
 *
 * Plan E6 asks for the operator's own cursor movement to be echoed
 * back to the viewer — "or not at all (configurable)".  The echo is a
 * *product* decision, not a codec feature, so it is switchable, and
 * when it is on it is **bounded** in two ways:
 *
 *   - **maximum echo age** — an event older than the budget is
 *     *stale*: the viewer has already moved on, and echoing it would
 *     drag the viewer's cursor back to where it was.  A stale event is
 *     dropped, not delayed.
 *   - **rate limit** — the echo path is capped at N events per second.
 *     An unbounded echo is an unbounded message rate, which on a
 *     congested link is exactly the wrong thing to do.
 *
 * ## Why the decision lives here and not in the module
 *
 * The event vocabulary belongs to the `rd_input` module, but the engine
 * may **not** include a concrete module header (Layout Invariant 4 /
 * ADR-025): `rd_input` is not on the exemption list.  So the engine's
 * seam is expressed with **plain engine types** — a cursor position
 * and a timestamp — and the *decision* (echo / stale / rate-limited)
 * is made here, in a small deterministic collaborator.
 *
 * The G-track wiring (plan G3) attaches the transport later: the
 * engine hands the accepted echo to its callback, and the host (or the
 * G-track code) puts it on the control channel.
 *
 * ## Determinism
 *
 * The controller never reads a clock: the caller passes "now" and the
 * event's own timestamp.  So it is directly unit-testable.
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * @see docs/plan/remote-desktop-plan.md §Track E (E6 — input echo).
 */
#pragma once

#include <cstdint>

namespace nimrtc::engine::detail {

/** The input-echo configuration. */
struct InputEchoConfig {
    /** Whether the echo is enabled at all.  Off by default. */
    bool enabled = false;

    /** The maximum age an echoed event may have, in microseconds.
     *
     *  0 = no age limit.  An event whose age exceeds the budget is
     *  *stale* and is dropped. */
    std::int64_t max_age_us = 250'000;

    /** The maximum number of echoes per second.  0 = no limit. */
    std::uint32_t max_rate_hz = 30;
};

/** Why an echo candidate was not echoed. */
enum class InputEchoReject : std::uint8_t {
    /** The candidate was echoed. */
    kNone = 0,
    /** The echo is switched off. */
    kDisabled = 1,
    /** The candidate was older than the maximum echo age. */
    kStale = 2,
    /** The rate limit was reached. */
    kRateLimited = 3,
};

/** The input-echo counters. */
struct InputEchoCounters {
    /** The events that were echoed. */
    std::uint64_t echoed = 0;
    /** The candidates that were dropped because they were stale. */
    std::uint64_t suppressed_stale = 0;
    /** The candidates that were dropped because of the rate limit. */
    std::uint64_t suppressed_rate = 0;
};

/**
 * @brief The echo decision: on / off, the maximum age and the rate limit.
 *
 * The controller is a pure function of its inputs plus its own
 * counters — no clock, no allocation, no thread — which makes it
 * directly unit-testable.
 */
class InputEchoController {
public:
    InputEchoController() = default;

    explicit InputEchoController(const InputEchoConfig& cfg) noexcept {
        configure(cfg);
    }

    // ---- Configuration ---------------------------------------------------

    void configure(const InputEchoConfig& cfg) noexcept {
        cfg_ = cfg;
        if (cfg_.max_age_us < 0) cfg_.max_age_us = 0;
    }

    [[nodiscard]] const InputEchoConfig& config() const noexcept {
        return cfg_;
    }

    void set_enabled(bool enabled) noexcept { cfg_.enabled = enabled; }
    [[nodiscard]] bool enabled() const noexcept { return cfg_.enabled; }

    void set_max_age_us(std::int64_t max_age_us) noexcept {
        cfg_.max_age_us = max_age_us < 0 ? 0 : max_age_us;
    }
    [[nodiscard]] std::int64_t max_age_us() const noexcept {
        return cfg_.max_age_us;
    }

    void set_max_rate_hz(std::uint32_t max_rate_hz) noexcept {
        cfg_.max_rate_hz = max_rate_hz;
    }
    [[nodiscard]] std::uint32_t max_rate_hz() const noexcept {
        return cfg_.max_rate_hz;
    }

    // ---- The decision ----------------------------------------------------

    /**
     * @brief Decide whether one cursor event may be echoed.
     *
     * @param now_us      The current instant, on the engine's monotonic
     *                    clock (microseconds).
     * @param event_ts_us The event's own timestamp, on the same clock.
     *                    0 means "unknown" and is treated as fresh.
     * @return `kNone` when the event may be echoed; the reason
     *         otherwise.
     */
    [[nodiscard]] InputEchoReject decide(std::int64_t now_us,
                                         std::int64_t event_ts_us) noexcept {
        if (!cfg_.enabled) return InputEchoReject::kDisabled;

        // ---- The age budget ------------------------------------------------
        if (cfg_.max_age_us > 0 && event_ts_us > 0) {
            const std::int64_t age = now_us - event_ts_us;
            if (age > cfg_.max_age_us) {
                ++counters_.suppressed_stale;
                return InputEchoReject::kStale;
            }
        }

        // ---- The rate limit ------------------------------------------------
        //
        // A fixed one-second window.  The window opens on the first
        // candidate after a quiet period, so a burst at the start of a
        // window is bounded by the limit and the window rolls over on
        // its own.
        if (cfg_.max_rate_hz > 0) {
            if (rate_window_start_us_ == 0 || now_us - rate_window_start_us_ >= 1'000'000) {
                rate_window_start_us_ = now_us;
                rate_window_count_ = 0;
            }
            if (rate_window_count_ >= cfg_.max_rate_hz) {
                ++counters_.suppressed_rate;
                return InputEchoReject::kRateLimited;
            }
            ++rate_window_count_;
        }

        ++counters_.echoed;
        return InputEchoReject::kNone;
    }

    // ---- Reporting --------------------------------------------------------

    [[nodiscard]] const InputEchoCounters& counters() const noexcept {
        return counters_;
    }

    void reset_counters() noexcept { counters_ = InputEchoCounters{}; }

private:
    InputEchoConfig  cfg_{};
    InputEchoCounters counters_{};

    /// The number of echoes emitted in the current one-second window.
    std::uint32_t rate_window_count_ = 0;

    /// The instant at which the current rate window opened.
    std::int64_t rate_window_start_us_ = 0;
};

}  // namespace nimrtc::engine::detail
