// src/modules/rd_input/src/coalescer.cpp
// ----------------------------------------------------------------------------
// The input-side coalescer and rate limiter (remote-desktop plan G4).
// ----------------------------------------------------------------------------

#include "nimrtc/rd_input/coalescer.hpp"

namespace nimrtc::rd_input {

InputCoalescer::InputCoalescer(const CoalescerConfig& config)
    : config_{config} {}

// ---------------------------------------------------------------------------
// push
// ---------------------------------------------------------------------------

bool InputCoalescer::push(const InputEvent& event, std::uint64_t now_us) {
    switch (event.type) {
        case InputEventType::kMouseMove:
            ++stats_.pointer_events_in;

            // The absolute position follows the relative motion, so the
            // coalescer tracks it: a button event carries the position the
            // operator clicked at, and the pending motion has to be applied
            // to it first.
            last_x_ += event.dx;
            last_y_ += event.dy;

            pending_dx_ += event.dx;
            pending_dy_ += event.dy;
            pending_valid_ = true;
            // The coalesced motion carries the timestamp of the *last*
            // sample it covers, so the receiver can tell how fresh it is.
            pending_motion_ts_us_ = event.timestamp_us != 0 ? event.timestamp_us : now_us;
            return true;

        case InputEventType::kMouseWheel:
            // A wheel notch is a discrete act: coalescing two notches into
            // one loses a scroll step.  It is rate-limited with the
            // pointer, but it is never merged with another event.
            ++stats_.essential_events_in;
            essential_.push_back(event);
            return true;

        case InputEventType::kMouseButton: {
            ++stats_.essential_events_in;
            // The click happened at the pointer's current position, so the
            // pending motion is applied to the event's own coordinates and
            // the pending delta is discarded — the click carries the
            // position, and a motion event after it would move the pointer
            // again.
            InputEvent button_event = event;
            if (pending_valid_) {
                if (button_event.x == 0 && button_event.y == 0) {
                    button_event.x = last_x_;
                    button_event.y = last_y_;
                }
                pending_dx_ = 0;
                pending_dy_ = 0;
                pending_valid_ = false;
            }
            essential_.push_back(std::move(button_event));
            return true;
        }

        case InputEventType::kKey:
            ++stats_.essential_events_in;
            ++stats_.key_events_in;
            essential_.push_back(event);
            return true;

        case InputEventType::kText:
            ++stats_.essential_events_in;
            essential_.push_back(event);
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// flush_pointer
// ---------------------------------------------------------------------------

void InputCoalescer::flush_pointer(std::uint64_t now_us,
                                    std::vector<InputEvent>& out) {
    if (!pending_valid_) return;

    // ---- the display rate ----------------------------------------------
    // A pointer event goes out at most once per display frame.  The first
    // one after a quiet period goes out immediately — there is nothing to
    // wait for, and waiting would add a frame of latency to the very first
    // movement.
    const std::uint64_t interval = config_.frame_interval_us();
    bool due = true;
    if (interval > 0 && last_pointer_emit_us_ != 0
        && now_us < last_pointer_emit_us_ + interval) {
        due = false;
    }
    if (!due) return;

    // ---- the input path's own rate limit --------------------------------
    // A separate budget from the media path's: a stuck pointer device must
    // not be able to flood the link.  The window is one second.
    if (config_.max_pointer_events_per_second > 0) {
        if (now_us < rate_window_start_us_) {
            // The clock went backwards (a host that reset its monotonic
            // clock, or a rebase).  Start a fresh window.
            rate_window_start_us_ = now_us;
            pointer_events_this_window_ = 0;
        } else if (now_us - rate_window_start_us_ >= 1000000ull) {
            rate_window_start_us_ = now_us;
            pointer_events_this_window_ = 0;
        }
        if (pointer_events_this_window_ >= config_.max_pointer_events_per_second) {
            // Out of budget: the pending motion is dropped, not queued.  A
            // queued backlog would turn the rate limit into a latency
            // generator instead of a bound.
            ++stats_.pointer_events_dropped;
            pending_dx_ = 0;
            pending_dy_ = 0;
            pending_valid_ = false;
            last_pointer_emit_us_ = now_us;
            return;
        }
        ++pointer_events_this_window_;
    }

    InputEvent move;
    move.type = InputEventType::kMouseMove;
    move.dx = pending_dx_;
    move.dy = pending_dy_;
    move.x = last_x_;
    move.y = last_y_;
    move.timestamp_us = pending_motion_ts_us_ != 0 ? pending_motion_ts_us_ : now_us;
    out.push_back(std::move(move));

    ++stats_.pointer_events_out;
    pending_dx_ = 0;
    pending_dy_ = 0;
    pending_valid_ = false;
    pending_motion_ts_us_ = 0;
    last_pointer_emit_us_ = now_us;
}

// ---------------------------------------------------------------------------
// drain
// ---------------------------------------------------------------------------

std::vector<InputEvent> InputCoalescer::drain(std::uint64_t now_us) {
    std::vector<InputEvent> out;

    // The essential events come out first, in arrival order.  A key press
    // that is queued behind a pointer event would arrive late; a key press
    // that is queued behind *another key press* would arrive out of order.
    while (!essential_.empty()) {
        out.push_back(std::move(essential_.front()));
        essential_.pop_front();
        ++stats_.essential_events_out;
    }

    // The pointer motion comes out last.
    flush_pointer(now_us, out);

    return out;
}

// ---------------------------------------------------------------------------
// reset
// ---------------------------------------------------------------------------

void InputCoalescer::reset() noexcept {
    essential_.clear();
    pending_dx_ = 0;
    pending_dy_ = 0;
    pending_valid_ = false;
    pending_motion_ts_us_ = 0;
    last_x_ = 0;
    last_y_ = 0;
    last_pointer_emit_us_ = 0;
    rate_window_start_us_ = 0;
    pointer_events_this_window_ = 0;
}

} // namespace nimrtc::rd_input
