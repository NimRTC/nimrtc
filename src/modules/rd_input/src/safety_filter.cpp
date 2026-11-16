// src/modules/rd_input/src/safety_filter.cpp
// ----------------------------------------------------------------------------
// The configurable input safety filter (remote-desktop plan G7).
// ----------------------------------------------------------------------------

#include "nimrtc/rd_input/safety_filter.hpp"

#include <nimrtc/core/log.hpp>

namespace nimrtc::rd_input {

SafetyFilter::SafetyFilter(const SafetyFilterConfig& config)
    : config_{config} {}

// ---------------------------------------------------------------------------
// check
// ---------------------------------------------------------------------------

FilterResult SafetyFilter::check(const InputEvent& event) const {
    FilterResult result;

    // ---- the key policy ---------------------------------------------------
    if (event.type == InputEventType::kKey) {
        // The deny list always wins: a key in both lists is denied.
        if (config_.denied_keys.count(event.key_code) != 0) {
            result.verdict = FilterVerdict::kDeny;
            result.reason = "key " + std::to_string(event.key_code)
                          + " is on the deny list";
            report_denial(event, result.reason);
            return result;
        }
        if (!config_.allowed_keys.empty()
            && config_.allowed_keys.count(event.key_code) == 0) {
            result.verdict = FilterVerdict::kDeny;
            result.reason = "key " + std::to_string(event.key_code)
                          + " is not on the allow list";
            report_denial(event, result.reason);
            return result;
        }
    }

    // ---- the button policy ------------------------------------------------
    if (event.type == InputEventType::kMouseButton
        && config_.denied_buttons.count(event.button) != 0) {
        result.verdict = FilterVerdict::kDeny;
        result.reason = "mouse button "
                      + std::to_string(static_cast<int>(event.button))
                      + " is on the deny list";
        report_denial(event, result.reason);
        return result;
    }

    // ---- the axis policy --------------------------------------------------
    // An axis limit is a clamp, not a rejection: dropping the event would
    // leave the remote cursor stuck where it was, which is worse than a
    // cursor that moves less than the operator asked for.
    if (event.type == InputEventType::kMouseMove
        || event.type == InputEventType::kMouseWheel) {
        if (!config_.dx_range.contains(event.dx)) {
            result.clamped = true;
            result.clamped_axis = Axis::kDx;
            result.clamped_value = config_.dx_range.clamp(event.dx);
        } else if (!config_.dy_range.contains(event.dy)) {
            result.clamped = true;
            result.clamped_axis = Axis::kDy;
            result.clamped_value = config_.dy_range.clamp(event.dy);
        }
    } else if (event.type == InputEventType::kMouseButton) {
        if (!config_.x_range.contains(event.x)) {
            result.clamped = true;
            result.clamped_axis = Axis::kX;
            result.clamped_value = config_.x_range.clamp(event.x);
        } else if (!config_.y_range.contains(event.y)) {
            result.clamped = true;
            result.clamped_axis = Axis::kY;
            result.clamped_value = config_.y_range.clamp(event.y);
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// adjust
// ---------------------------------------------------------------------------

InputEvent SafetyFilter::adjust(const InputEvent& event) const {
    InputEvent out = event;

    switch (event.type) {
        case InputEventType::kMouseMove:
        case InputEventType::kMouseWheel:
            out.dx = config_.dx_range.clamp(event.dx);
            out.dy = config_.dy_range.clamp(event.dy);
            break;
        case InputEventType::kMouseButton:
            out.x = config_.x_range.clamp(event.x);
            out.y = config_.y_range.clamp(event.y);
            break;
        case InputEventType::kKey:
        case InputEventType::kText:
            break;
    }
    return out;
}

// ---------------------------------------------------------------------------
// apply
// ---------------------------------------------------------------------------

bool SafetyFilter::apply(InputEvent& event, FilterResult& result) const {
    result = check(event);
    if (!result.allowed()) {
        return false;
    }
    event = adjust(event);
    return true;
}

// ---------------------------------------------------------------------------
// report_denial
// ---------------------------------------------------------------------------

void SafetyFilter::report_denial(const InputEvent& event,
                                  const std::string& reason) const {
    // The denial is logged: a security-relevant decision that leaves no
    // trace is a decision nobody can audit, and a viewer whose input
    // "silently does nothing" is indistinguishable from a broken session.
    NIMRTC_LOG_WARN("rd_input: denied an input event ("
                    << input_event_type_name(event.type) << "): " << reason);

    if (config_.on_deny) {
        config_.on_deny(event, reason);
    }
}

} // namespace nimrtc::rd_input
