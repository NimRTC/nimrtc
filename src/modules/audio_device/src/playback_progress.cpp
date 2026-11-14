// audio_device/src/playback_progress.cpp
// The playback-progress bookkeeping — remote-desktop plan F3.

#include <nimrtc/audio_device/playback_progress.hpp>

#include <chrono>
#include <algorithm>

namespace nimrtc::audio_device {

namespace {

std::int64_t now_us() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

PlaybackProgressTracker::PlaybackProgressTracker(
    std::size_t backlog_limit_frames) noexcept
    : backlog_limit_frames_(backlog_limit_frames) {}

void PlaybackProgressTracker::on_samples_queued(std::size_t frames) noexcept {
    if (frames == 0) {
        return;
    }
    queued_.fetch_add(static_cast<std::uint64_t>(frames),
                       std::memory_order_relaxed);

    if (backlog_limit_frames_ != 0) {
        const std::size_t pending = frames_pending();
        if (pending > backlog_limit_frames_) {
            backlog_limit_exceeded_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void PlaybackProgressTracker::on_samples_played(std::size_t frames) noexcept {
    if (frames == 0) {
        return;
    }

    const std::uint64_t queued = queued_.load(std::memory_order_relaxed);
    const std::uint64_t already = played_.load(std::memory_order_relaxed);

    // The played counter may never exceed the queued counter: an
    // over-report is a device bug, and letting the AEC reference run
    // ahead of the audio that was actually handed over would silently
    // misalign the echo canceller.  Clamp and count the event.
    std::uint64_t allowed = frames;
    if (already >= queued) {
        // Nothing is outstanding: nothing may be played.
        over_reports_clamped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::uint64_t outstanding = queued - already;
    if (allowed > outstanding) {
        allowed = outstanding;
        over_reports_clamped_.fetch_add(1, std::memory_order_relaxed);
    }

    played_.fetch_add(allowed, std::memory_order_relaxed);
    last_playback_us_.store(now_us(), std::memory_order_relaxed);
}

std::uint64_t PlaybackProgressTracker::frames_played() const noexcept {
    return played_.load(std::memory_order_relaxed);
}

std::uint64_t PlaybackProgressTracker::frames_queued() const noexcept {
    return queued_.load(std::memory_order_relaxed);
}

std::size_t PlaybackProgressTracker::frames_pending() const noexcept {
    const std::uint64_t queued = queued_.load(std::memory_order_relaxed);
    const std::uint64_t played = played_.load(std::memory_order_relaxed);
    if (played >= queued) {
        return 0;
    }
    return static_cast<std::size_t>(queued - played);
}

std::int64_t PlaybackProgressTracker::last_playback_us() const noexcept {
    return last_playback_us_.load(std::memory_order_relaxed);
}

bool PlaybackProgressTracker::backlog_exceeded() const noexcept {
    if (backlog_limit_frames_ == 0) {
        return false;
    }
    return frames_pending() > backlog_limit_frames_;
}

PlaybackProgressStats PlaybackProgressTracker::stats() const noexcept {
    PlaybackProgressStats s;
    s.frames_queued = queued_.load(std::memory_order_relaxed);
    s.frames_played = played_.load(std::memory_order_relaxed);
    s.frames_pending = frames_pending();
    s.last_playback_us = last_playback_us_.load(std::memory_order_relaxed);
    s.backlog_limit_exceeded = backlog_limit_exceeded_.load(std::memory_order_relaxed);
    s.over_reports_clamped = over_reports_clamped_.load(std::memory_order_relaxed);
    return s;
}

void PlaybackProgressTracker::reset() noexcept {
    queued_.store(0, std::memory_order_relaxed);
    played_.store(0, std::memory_order_relaxed);
    last_playback_us_.store(0, std::memory_order_relaxed);
    backlog_limit_exceeded_.store(0, std::memory_order_relaxed);
    over_reports_clamped_.store(0, std::memory_order_relaxed);
}

} // namespace nimrtc::audio_device
