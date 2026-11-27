/**
 * @file src/engine/src/engine_capture_reconnect.hpp
 * @brief The capture-device reconnect backoff state machine (v1-device-policy D4).
 *
 * ## What this file is
 *
 * When a capture device is lost (its error counter grows, or a
 * device-list change reports it removed), the engine must not reopen it
 * once and give up, and must not retry it every tick.  It reopens with
 * an exponential backoff: 100 ms, doubling, capped at 5 s, at most 20
 * attempts, then it gives up (`kStopped`).
 *
 * It is a separate header, with no dependence on the engine, on the
 * plugins or on the core library, for the same reason
 * `engine_capture_cadence.hpp` is: the retry rule is subtle enough that
 * it is pinned by a **deterministic** unit test rather than by driving
 * a device through real 5-second waits
 * (`src/engine/tests/test_engine_device_policy.cpp`).  The rule and the
 * test must be the same rule, so both include this file.
 *
 * ## The state machine
 *
 * ```
 * kIdle --(device lost)--> kBackoff --(reopen ok)--> kIdle
 *                              |  \--(reopen fails, attempts <= max)--> wait longer
 *                              \---(reopen fails, attempts > max)--> kStopped
 * ```
 *
 * The state lives in the engine's `video_state` (a private engine
 * member, not a global), so both the "device lost" path and the "device
 * healthy" path observe the same machine.
 *
 * ## Time
 *
 * All entry points take `now_us` explicitly so the test can drive the
 * machine with synthetic timestamps instead of sleeping.
 */

#pragma once

#include <algorithm>
#include <cstdint>

namespace nimrtc::engine::detail {

/** The reconnect backoff parameters (engine-internal, tunable). */
struct CaptureReconnectParams {
    /** The first retry's delay after the device is lost. */
    std::int64_t initial_backoff_us = 100'000;  // 100 ms
    /** The ceiling the doubling is capped at. */
    std::int64_t max_backoff_us = 5'000'000;    // 5 s
    /** Retries above this bound give up (kStopped).  The 100ms->5s
     *  doubling sequence reaches the ceiling at attempt ~6 and the
     *  ceiling then repeats, so 20 attempts take roughly a minute. */
    std::uint32_t max_attempts = 20;
};

/** The reconnect state machine (D4). */
class CaptureReconnect {
public:
    explicit CaptureReconnect(CaptureReconnectParams params = {}) noexcept
        : params_(params) {}

    enum class State : std::uint8_t {
        kIdle = 0,     ///< the device is healthy
        kBackoff = 1,  ///< the device is lost, waiting for the next retry
        kStopped = 2,  ///< gave up (max attempts exceeded)
    };

    State state() const noexcept { return state_; }
    std::uint32_t attempts() const noexcept { return attempts_; }
    std::int64_t current_backoff_us() const noexcept { return backoff_us_; }
    /** Whether a retry is due now. */
    bool due(std::int64_t now_us) const noexcept {
        return state_ == State::kBackoff && now_us >= next_retry_us_;
    }

    /** Enter `kBackoff` from `kIdle` (a device loss).  Already in
     *  `kBackoff` the current window is kept — a device that stays lost
     *  must not reset its own timer.
     *
     *  The **first** reopen is probed immediately (due on the next tick):
     *  a device that recovers between the loss notification and the next
     *  tick must be back on the pump at once.  Only when that probe
     *  *fails* does the exponential backoff start (100 ms, doubling,
     *  capped at 5 s). */
    void note_lost(std::int64_t now_us) noexcept {
        if (state_ == State::kIdle) {
            state_ = State::kBackoff;
            backoff_us_ = 0;  // first retry is immediate
            next_retry_us_ = now_us;
        }
    }

    /** A reopen succeeded: back to `kIdle`, counters cleared. */
    void note_succeeded(std::int64_t /*now_us*/) noexcept {
        state_ = State::kIdle;
        attempts_ = 0;
        backoff_us_ = 0;
        next_retry_us_ = 0;
    }

    /** A reopen failed: double the backoff (capped), count the attempt;
     *  at the bound, give up.  The bound is inclusive — exactly
     *  `max_attempts` failed reopens happen, no more (spec D4: "重试次数
     *  ≤ 上限"). */
    void note_failed(std::int64_t now_us) noexcept {
        ++attempts_;
        if (attempts_ >= params_.max_attempts) {
            state_ = State::kStopped;
            return;
        }
        if (backoff_us_ == 0) {
            backoff_us_ = params_.initial_backoff_us;
        } else {
            backoff_us_ = std::min(backoff_us_ * 2, params_.max_backoff_us);
        }
        next_retry_us_ = now_us + backoff_us_;
    }

    /** Reset to `kIdle` without retrying (the engine is closing). */
    void reset() noexcept {
        state_ = State::kIdle;
        attempts_ = 0;
        backoff_us_ = 0;
        next_retry_us_ = 0;
    }

private:
    CaptureReconnectParams params_;
    State state_ = State::kIdle;
    std::uint32_t attempts_ = 0;
    std::int64_t backoff_us_ = 0;
    std::int64_t next_retry_us_ = 0;
};

/** The device-change poll interval (engine-internal, tunable).  Enumeration
 *  is not cheap on every backend, so the diff runs on this throttle rather
 *  than on every tick. */
inline constexpr std::int64_t kCaptureDevicePollIntervalUs = 500'000;  // 500 ms

}  // namespace nimrtc::engine::detail
