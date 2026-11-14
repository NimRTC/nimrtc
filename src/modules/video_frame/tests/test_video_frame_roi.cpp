// src/modules/video_frame/tests/test_video_frame_roi.cpp
// ----------------------------------------------------------------------------
// The shared source-space -> output-space rectangle scaling rule
// (remote-desktop plan C4).
//
// The rule is the one the engine and the encoder-side policy share, so it
// is tested here -- once -- rather than twice.
// ----------------------------------------------------------------------------

#include <gtest/gtest.h>

#include "nimrtc/video_frame/roi.hpp"

namespace nimrtc::video_frame {
namespace {

TEST(VideoFrameRoi, ARectangleAtTheSameResolutionIsUnchanged) {
    const auto scaled = scale_region(100, 200, 300, 400, 1920, 1080, 1920, 1080);
    ASSERT_TRUE(scaled.has_value());
    EXPECT_EQ(scaled->x, 100u);
    EXPECT_EQ(scaled->y, 200u);
    EXPECT_EQ(scaled->width, 300u);
    EXPECT_EQ(scaled->height, 400u);
}

TEST(VideoFrameRoi, ARectangleScalesWithTheResolution) {
    const auto scaled = scale_region(960, 540, 480, 360, 1920, 1080, 960, 540);
    ASSERT_TRUE(scaled.has_value());
    EXPECT_EQ(scaled->x, 480u);
    EXPECT_EQ(scaled->y, 270u);
    EXPECT_EQ(scaled->width, 240u);
    EXPECT_EQ(scaled->height, 180u);
}

TEST(VideoFrameRoi, ARectangleIsClampedToTheOutput) {
    const auto scaled = scale_region(1800, 1000, 400, 300, 1920, 1080, 1920, 1080);
    ASSERT_TRUE(scaled.has_value());
    EXPECT_EQ(scaled->x, 1800u);
    EXPECT_EQ(scaled->width, 120u);
    EXPECT_EQ(scaled->y, 1000u);
    EXPECT_EQ(scaled->height, 80u);
}

TEST(VideoFrameRoi, ARectangleOutsideTheFrameIsDropped) {
    EXPECT_FALSE(scale_region(5000, 5000, 100, 100, 1920, 1080, 1920, 1080).has_value());
}

TEST(VideoFrameRoi, ADegenerateRectangleIsDropped) {
    EXPECT_FALSE(scale_region(10, 10, 0, 100, 1920, 1080, 1920, 1080).has_value());
    EXPECT_FALSE(scale_region(10, 10, 100, 0, 1920, 1080, 1920, 1080).has_value());
}

TEST(VideoFrameRoi, ARectangleBelowTheMinimumSizeIsDropped) {
    EXPECT_FALSE(scale_region(10, 10, 8, 100, 1920, 1080, 1920, 1080, 16).has_value());
    EXPECT_TRUE(scale_region(10, 10, 100, 100, 1920, 1080, 1920, 1080, 16).has_value());
}

TEST(VideoFrameRoi, DegenerateResolutionsAreDropped) {
    EXPECT_FALSE(scale_region(0, 0, 10, 10, 0, 1080, 1920, 1080).has_value());
    EXPECT_FALSE(scale_region(0, 0, 10, 10, 1920, 0, 1920, 1080).has_value());
    EXPECT_FALSE(scale_region(0, 0, 10, 10, 1920, 1080, 0, 1080).has_value());
    EXPECT_FALSE(scale_region(0, 0, 10, 10, 1920, 1080, 1920, 0).has_value());
}

} // namespace
} // namespace nimrtc::video_frame
