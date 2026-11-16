/**
 * @file nimrtc/rd_input/safety_filter.hpp
 * @brief The configurable input safety filter (remote-desktop plan G7).
 *
 * ## What it protects
 *
 * The operator of a remote desktop can send *any* key.  A policy that
 * says "the viewer may drive the machine, but not into a lock screen, not
 * into a task switcher, and not out of the app window" is a **safety**
 * policy, and it belongs on the *sending* side: a denied event must never
 * reach the remote machine at all, rather than being rejected after it
 * arrives.
 *
 * The filter therefore runs on the **capture** side, before the event is
 * encoded and put on the wire.
 *
 * ## The two lists
 *
 * Per key:
 *
 *   - `denied_keys` — these key codes never go out.  A denied key is
 *     *dropped*, not queued, not delayed.
 *   - `allowed_keys` — when this list is **not** empty, it is an
 *     allow-list: every key outside it is denied.  When it is empty, every
 *     key that is not explicitly denied passes.
 *
 * The deny list always wins over the allow list: a key in both lists is
 * denied.  A policy that says "allow A, B, C; deny C" means C is denied.
 *
 * Per axis:
 *
 *   - `dx` / `dy` — the *relative* pointer motion, in pixels per event.
 *   - `x` / `y` — the *absolute* pointer position, in pixels.
 *
 * An axis limit is a **clamp**, not a rejection: a pointer event whose
 * motion exceeds the limit is emitted with the axis clamped to the limit's
 * bound.  Clamping keeps the pointer usable while bounding what a viewer
 * can do, and dropping the event instead would leave the remote cursor
 * stuck.  A key event is different in kind — a key either goes or does not
 * — so a denied key is dropped.
 *
 * ## The denial is logged
 *
 * Every denial is reported through the core logger at the `Warn` level,
 * with the offending key code or axis and the reason.  A security-relevant
 * decision that leaves no trace is a decision nobody can audit, and a
 * viewer whose input "silently does nothing" is indistinguishable from a
 * broken session.
 *
 * A host that wants the denial in *its* telemetry as well can install a
 * `DenyObserver`: the filter then calls it in addition to logging.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <functional>
#include <set>
#include <string>

#include "nimrtc/rd_input/input_event.hpp"

namespace nimrtc::rd_input {

// ---------------------------------------------------------------------------
// Axis limits
// ---------------------------------------------------------------------------

/**
 * @brief An inclusive range for one axis.
 *
 * A default-constructed range is *unbounded*: `enabled` is false and the
 * bounds are ignored.
 */
struct NIMRTC_API_STABLE AxisRange {
    /** Whether the range applies at all. */
    bool enabled = false;

    /** The lowest permitted value (inclusive). */
    std::int32_t min_value = 0;

    /** The highest permitted value (inclusive). */
    std::int32_t max_value = 0;

    /** Clamp `value` into the range.  A disabled range returns `value`
     *  unchanged. */
    [[nodiscard]] std::int32_t clamp(std::int32_t value) const noexcept {
        if (!enabled) return value;
        if (value < min_value) return min_value;
        if (value > max_value) return max_value;
        return value;
    }

    /** True when `value` is inside the range (or the range is disabled). */
    [[nodiscard]] bool contains(std::int32_t value) const noexcept {
        if (!enabled) return true;
        return value >= min_value && value <= max_value;
    }
};

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/** The safety filter's configuration. */
struct NIMRTC_API_STABLE SafetyFilterConfig {
    /** The key codes that are never sent. */
    std::set<std::uint32_t> denied_keys;

    /** When non-empty, the *only* key codes that are sent. */
    std::set<std::uint32_t> allowed_keys;

    /** The limit on the relative pointer motion per event. */
    AxisRange dx_range;

    /** The limit on the relative pointer motion per event (the y axis). */
    AxisRange dy_range;

    /** The limit on the absolute pointer position. */
    AxisRange x_range;

    /** The limit on the absolute pointer position (the y axis). */
    AxisRange y_range;

    /** The mouse buttons that are never sent.  An empty set allows every
     *  button. */
    std::set<MouseButton> denied_buttons;

    /** Called for every denial, in addition to the core logger.  The
     *  observer runs on the calling thread. */
    std::function<void(const InputEvent& event, const std::string& reason)> on_deny;
};

// ---------------------------------------------------------------------------
// The verdict
// ---------------------------------------------------------------------------

/** What the filter decided about an event. */
enum class NIMRTC_API_STABLE FilterVerdict : std::uint8_t {
    kAllow = 0,  ///< the event passes (possibly with a clamped axis)
    kDeny  = 1,  ///< the event is dropped
};

/** The axes the filter can clamp. */
enum class NIMRTC_API_STABLE Axis : std::uint8_t {
    kNone = 0,
    kDx   = 1,
    kDy   = 2,
    kX    = 3,
    kY    = 4,
};

/** The result of filtering one event. */
struct NIMRTC_API_STABLE FilterResult {
    /** Whether the event passes. */
    FilterVerdict verdict = FilterVerdict::kAllow;

    /** The reason for a denial (empty when allowed). */
    std::string reason;

    /** True when the event was allowed but an axis was clamped. */
    bool clamped = false;

    /** The axis that was clamped (only meaningful when `clamped`). */
    Axis clamped_axis = Axis::kNone;

    /** The clamped value (only meaningful when `clamped`). */
    std::int32_t clamped_value = 0;

    [[nodiscard]] bool allowed() const noexcept { return verdict == FilterVerdict::kAllow; }
};

// ---------------------------------------------------------------------------
// The filter
// ---------------------------------------------------------------------------

/**
 * @brief The input safety filter.
 *
 * Usage:
 *
 * ```
 *   nimrtc::rd_input::SafetyFilter filter(config);
 *   auto result = filter.check(event);
 *   if (result.allowed()) {
 *       send(filter.adjust(event));    // the clamped event
 *   }
 * ```
 *
 * `check()` does not modify the event; `adjust()` returns the event with
 * its axes clamped.  Splitting the two means a caller that only wants a
 * yes/no answer does not pay for the copy — and a caller that forgets to
 * call `adjust()` sends the *unclamped* event, which is a hole.  To make
 * that hole impossible, `apply()` does both and is the intended entry
 * point; `check()` / `adjust()` are the building blocks it is made of.
 */
class NIMRTC_API_STABLE SafetyFilter {
public:
    explicit SafetyFilter(const SafetyFilterConfig& config = SafetyFilterConfig{});

    /** The configuration this filter was built with. */
    [[nodiscard]] const SafetyFilterConfig& config() const noexcept { return config_; }

    /**
     * @brief Decide whether `event` may be sent.
     *
     * The decision is logged at the `Warn` level when it is a denial.
     */
    [[nodiscard]] FilterResult check(const InputEvent& event) const;

    /**
     * @brief Return `event` with its axes clamped to the configured ranges.
     *
     * The event is returned unchanged when no range applies.
     */
    [[nodiscard]] InputEvent adjust(const InputEvent& event) const;

    /**
     * @brief Filter and clamp in one step.
     *
     * @param event    (in/out) the event; clamped in place when it is allowed.
     * @param result   (out) the verdict.
     * @return true when the event may be sent.
     */
    bool apply(InputEvent& event, FilterResult& result) const;

private:
    /** Log a denial (and notify the observer). */
    void report_denial(const InputEvent& event, const std::string& reason) const;

    SafetyFilterConfig config_;
};

} // namespace nimrtc::rd_input
