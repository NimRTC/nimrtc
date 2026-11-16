// src/modules/video_pipeline/src/roi.cpp
// ----------------------------------------------------------------------------
// The region-of-interest (remote-desktop plan C4).
//
// The scaling *rule* itself lives in `nimrtc::video_frame` (see
// `nimrtc/video_frame/roi.hpp`), because the engine needs it too and the
// engine is only allowed to reach the exempt modules.  This file is the
// *policy* around it: the minimum size, the cache, and the counter that
// makes a silently-dropped ROI visible.
// ----------------------------------------------------------------------------

#include "nimrtc/video_pipeline/roi.hpp"

#include <nimrtc/video_frame/roi.hpp>

namespace nimrtc::video_pipeline {

RoiScaler::RoiScaler(std::uint32_t capture_width,
                     std::uint32_t capture_height,
                     std::uint32_t min_size)
    : capture_width_{capture_width},
      capture_height_{capture_height},
      min_size_{min_size} {}

void RoiScaler::set_roi(std::optional<plugins::VideoRegion> roi) noexcept {
    roi_ = roi;
    // The cached scaling was computed for the old ROI.
    cache_valid_ = false;
    cache_value_.reset();
}

std::optional<plugins::VideoRegion> RoiScaler::scale(
    std::uint32_t out_width, std::uint32_t out_height) const noexcept {
    if (!roi_.has_value()) {
        return std::nullopt;
    }
    const plugins::VideoRegion& src = *roi_;
    if (src.empty()) {
        return std::nullopt;
    }

    const auto scaled = video_frame::scale_region(src.x,
                                                  src.y,
                                                  src.width,
                                                  src.height,
                                                  capture_width_,
                                                  capture_height_,
                                                  out_width,
                                                  out_height,
                                                  min_size_);
    if (!scaled.has_value()) {
        return std::nullopt;
    }

    plugins::VideoRegion out;
    out.x = scaled->x;
    out.y = scaled->y;
    out.width = scaled->width;
    out.height = scaled->height;
    return out;
}

std::optional<plugins::VideoRegion> RoiScaler::scaled(std::uint32_t out_width,
                                                      std::uint32_t out_height) noexcept {
    if (cache_valid_ && cache_width_ == out_width && cache_height_ == out_height) {
        return cache_value_;
    }
    auto result = scale(out_width, out_height);
    if (!result.has_value() && roi_.has_value()) {
        // The ROI exists but did not survive the scaling.  That is the
        // case the counter exists for.
        ++dropped_;
    }
    cache_valid_ = true;
    cache_width_ = out_width;
    cache_height_ = out_height;
    cache_value_ = result;
    return result;
}

void RoiScaler::reset() noexcept {
    cache_valid_ = false;
    cache_width_ = 0;
    cache_height_ = 0;
    cache_value_.reset();
    dropped_ = 0;
}

} // namespace nimrtc::video_pipeline
