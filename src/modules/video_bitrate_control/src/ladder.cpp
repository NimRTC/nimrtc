/**
 * @file src/ladder.cpp
 * @brief The ladder implementation.
 *
 * Remote-desktop plan D1 / D2 / D3; video plan B2 / B3 / B4 / B9.
 */

#include <nimrtc/video_bitrate_control/ladder.hpp>

#include <algorithm>
#include <cmath>

namespace nimrtc::video_bitrate_control {

namespace {

/** Scale a dimension, keeping it even (the 4:2:0 chroma needs it). */
std::uint32_t scale_even(std::uint32_t value, std::uint32_t num,
                         std::uint32_t den) noexcept {
    const std::uint64_t scaled =
        (static_cast<std::uint64_t>(value) * num + den / 2) / den;
    std::uint32_t out = static_cast<std::uint32_t>(scaled);
    if (out < 2) out = 2;
    if ((out & 1u) != 0u) --out;
    return out;
}

/** A fraction of @p value, as a bitrate. */
std::uint32_t percent(std::uint32_t value, std::uint32_t pct) noexcept {
    return static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(value) * pct / 100);
}

/** The frame-rate rungs, from the source rate downwards. */
struct FpsRungs {
    std::uint32_t full = 30;
    std::uint32_t mid  = 24;
    std::uint32_t low  = 15;
};

FpsRungs fps_rungs(std::uint32_t fps) noexcept {
    FpsRungs r;
    r.full = fps;
    r.mid  = fps >= 24 ? 24 : fps;
    r.low  = fps >= 15 ? 15 : fps;
    if (r.mid > r.full) r.mid = r.full;
    if (r.low > r.mid) r.low = r.mid;
    if (r.low == 0) r.low = 1;
    return r;
}

} // namespace

Ladder Ladder::make_default(std::uint32_t width,
                            std::uint32_t height,
                            std::uint32_t fps,
                            std::uint32_t bitrate,
                            VideoLadderMode mode) {
    Ladder l;

    const std::uint32_t w3 = scale_even(width, 3, 4);
    const std::uint32_t h3 = scale_even(height, 3, 4);
    const std::uint32_t w2 = scale_even(width, 1, 2);
    const std::uint32_t h2 = scale_even(height, 1, 2);
    const std::uint32_t w4 = scale_even(width, 1, 4);
    const std::uint32_t h4 = scale_even(height, 1, 4);

    const FpsRungs fr = fps_rungs(fps);

    std::vector<VideoLadderLevel> levels;

    if (mode == VideoLadderMode::kQuality) {
        // ---- Quality-first ------------------------------------------------
        //
        // The picture *size* is what the operator notices, so it is the
        // last thing to go.  The first rungs hold the geometry and
        // only the bitrate moves; the geometry rungs come afterwards
        // and are strictly cheaper than the full-size ones, so the
        // ladder stays monotone in bitrate.
        levels.push_back({width, height, fr.full, bitrate, false});
        levels.push_back({width, height, fr.full, percent(bitrate, 70), false});
        levels.push_back({width, height, fr.full, percent(bitrate, 50), false});
        levels.push_back({width, height, fr.full, percent(bitrate, 35), false});
        levels.push_back({w3,    h3,    fr.full, percent(bitrate, 30), true});
        levels.push_back({w3,    h3,    fr.mid,  percent(bitrate, 22), true});
        levels.push_back({w2,    h2,    fr.mid,  percent(bitrate, 15), true});
        levels.push_back({w2,    h2,    fr.low,  percent(bitrate, 10), true});
        levels.push_back({w4,    h4,    fr.low,  percent(bitrate, 5),  true});
    } else {
        // ---- Smoothness-first ----------------------------------------------
        //
        // The operator wants a picture that keeps *moving*.  The
        // resolution goes first, the frame rate is held as long as
        // possible, and the bitrate follows the geometry.
        levels.push_back({width, height, fr.full, bitrate, false});
        levels.push_back({w3,    h3,    fr.full, percent(bitrate, 56), true});
        levels.push_back({w2,    h2,    fr.full, percent(bitrate, 25), true});
        levels.push_back({w4,    h4,    fr.full, percent(bitrate, 14), true});
        levels.push_back({w4,    h4,    fr.mid,  percent(bitrate, 10), true});
        levels.push_back({w4,    h4,    fr.low,  percent(bitrate, 7),  true});
    }

    l.set_levels(std::move(levels));
    l.set_bitrate_bounds(150'000, 8'000'000);
    return l;
}

void Ladder::set_levels(std::vector<VideoLadderLevel> levels) {
    levels_ = std::move(levels);
}

VideoLadderLevel Ladder::at(std::size_t index) const noexcept {
    if (levels_.empty()) return {};
    if (index >= levels_.size()) index = levels_.size() - 1;
    return levels_[index];
}

std::size_t Ladder::index_for_height(std::uint32_t height) const noexcept {
    if (levels_.empty()) return 0;
    // The *last* rung that is still tall enough is the cheapest rung
    // that satisfies the floor; the rung below it is the first one
    // that violates it.
    std::size_t answer = levels_.size() - 1;
    for (std::size_t i = 0; i < levels_.size(); ++i) {
        if (levels_[i].height >= height) {
            answer = i;
        } else {
            break;
        }
    }
    return answer;
}

std::uint32_t Ladder::clamped_bitrate(std::size_t index) const noexcept {
    return clamp_bitrate(at(index).bitrate_bps);
}

void Ladder::set_bitrate_bounds(std::uint32_t min_bps,
                                 std::uint32_t max_bps) noexcept {
    if (min_bps == 0) min_bps = 1;
    if (max_bps < min_bps) max_bps = min_bps;
    min_bps_ = min_bps;
    max_bps_ = max_bps;
}

std::uint32_t Ladder::clamp_bitrate(std::uint32_t bps) const noexcept {
    if (bps < min_bps_) return min_bps_;
    if (bps > max_bps_) return max_bps_;
    return bps;
}

std::uint32_t Ladder::bitrate_for(std::size_t index) const noexcept {
    return clamp_bitrate(at(index).bitrate_bps);
}

std::uint64_t Ladder::area_for(std::size_t index) const noexcept {
    const VideoLadderLevel lvl = at(index);
    return static_cast<std::uint64_t>(lvl.width) *
           static_cast<std::uint64_t>(lvl.height);
}

} // namespace nimrtc::video_bitrate_control
