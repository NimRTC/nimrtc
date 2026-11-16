// src/modules/video_sink/src/displayed_frame.cpp
// ----------------------------------------------------------------------------
// The "what the operator saw" record (remote-desktop plan E9).
// ----------------------------------------------------------------------------

#include "nimrtc/video_sink/displayed_frame.hpp"

#include <algorithm>

namespace nimrtc::video_sink {

DisplayedFrameTracker::DisplayedFrameTracker(std::size_t capacity)
    : capacity_{capacity == 0 ? 1u : capacity} {
    entries_.reserve(capacity_);
}

void DisplayedFrameTracker::record(DisplayedFrameInfo info) noexcept {
    ++recorded_;
    entries_.push_back(info);
    // The window is a *recent* one: when it is full the oldest entry falls
    // out.  A remote-desktop session can run for hours, so an unbounded
    // log of every displayed frame would be a slow memory leak.
    if (entries_.size() > capacity_) {
        entries_.erase(entries_.begin());
        ++evicted_;
    }
}

std::optional<DisplayedFrameInfo> DisplayedFrameTracker::last() const noexcept {
    if (entries_.empty()) {
        return std::nullopt;
    }
    return entries_.back();
}

std::optional<DisplayedFrameInfo> DisplayedFrameTracker::find(
    std::uint64_t ref_frame_id) const noexcept {
    // The newest match wins: the same id can legitimately be recorded
    // twice (a re-sent frame, a loopback), and the operator was looking
    // at the most recent one.
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (it->ref_frame_id == ref_frame_id) {
            return *it;
        }
    }
    return std::nullopt;
}

std::optional<std::int64_t> DisplayedFrameTracker::capture_ts_for(
    std::uint64_t ref_frame_id) const noexcept {
    const auto found = find(ref_frame_id);
    if (!found.has_value()) {
        return std::nullopt;
    }
    return found->capture_ts_us;
}

std::size_t DisplayedFrameTracker::size() const noexcept {
    return entries_.size();
}

std::size_t DisplayedFrameTracker::capacity() const noexcept {
    return capacity_;
}

std::uint64_t DisplayedFrameTracker::recorded_total() const noexcept {
    return recorded_;
}

std::uint64_t DisplayedFrameTracker::evicted_total() const noexcept {
    return evicted_;
}

void DisplayedFrameTracker::clear() noexcept {
    entries_.clear();
    recorded_ = 0;
    evicted_ = 0;
}

} // namespace nimrtc::video_sink
