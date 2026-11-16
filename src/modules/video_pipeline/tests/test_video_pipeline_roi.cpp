// src/modules/video_pipeline/tests/test_video_pipeline_roi.cpp
// ----------------------------------------------------------------------------
// The region-of-interest (remote-desktop plan C4).
//
// The acceptance criterion is: "A test asserts the encoder receives the
// expected ROI rectangle and that the ROI survives a ladder resolution
// change."  The second half is the interesting one: the ROI is expressed
// in capture coordinates and has to be rescaled when the rate
// controller's ladder steps the encoder to another resolution.
// ----------------------------------------------------------------------------

#include <gtest/gtest.h>

#include "nimrtc/video_pipeline/roi.hpp"

namespace nimrtc::video_pipeline {
namespace {

using plugins::VideoRegion;

TEST(Roi, NoRoiMeansNoRoi) {
    RoiScaler scaler{1920, 1080};
    EXPECT_FALSE(scaler.has_roi());
    EXPECT_FALSE(scaler.scale(1920, 1080).has_value());
    EXPECT_FALSE(scaler.scaled(1920, 1080).has_value());
}

TEST(Roi, ARoiAtTheCaptureResolutionIsUnchanged) {
    RoiScaler scaler{1920, 1080};
    VideoRegion roi;
    roi.x = 100;
    roi.y = 200;
    roi.width = 300;
    roi.height = 400;
    scaler.set_roi(roi);

    const auto scaled = scaler.scale(1920, 1080);
    ASSERT_TRUE(scaled.has_value());
    EXPECT_EQ(scaled->x, 100u);
    EXPECT_EQ(scaled->y, 200u);
    EXPECT_EQ(scaled->width, 300u);
    EXPECT_EQ(scaled->height, 400u);
}

TEST(Roi, TheRoiSurvivesALadderResolutionChange) {
    RoiScaler scaler{1920, 1080};
    VideoRegion roi;
    roi.x = 960;
    roi.y = 540;
    roi.width = 480;
    roi.height = 360;
    scaler.set_roi(roi);

    // The ladder steps down to 960x540: everything halves.
    const auto scaled = scaler.scale(960, 540);
    ASSERT_TRUE(scaled.has_value());
    EXPECT_EQ(scaled->x, 480u);
    EXPECT_EQ(scaled->y, 270u);
    EXPECT_EQ(scaled->width, 240u);
    EXPECT_EQ(scaled->height, 180u);

    // And back up to 1280x720.
    const auto scaled2 = scaler.scale(1280, 720);
    ASSERT_TRUE(scaled2.has_value());
    EXPECT_EQ(scaled2->x, 640u);
    EXPECT_EQ(scaled2->y, 360u);
    EXPECT_EQ(scaled2->width, 320u);
    EXPECT_EQ(scaled2->height, 240u);
}

TEST(Roi, TheRoiIsClampedToTheOutputFrame) {
    RoiScaler scaler{1920, 1080};
    VideoRegion roi;
    roi.x = 1800;  // hangs off the right edge
    roi.y = 1000;  // hangs off the bottom edge
    roi.width = 400;
    roi.height = 300;
    scaler.set_roi(roi);

    const auto scaled = scaler.scale(1920, 1080);
    ASSERT_TRUE(scaled.has_value());
    EXPECT_EQ(scaled->x, 1800u);
    EXPECT_EQ(scaled->y, 1000u);
    // Clamped to the frame.
    EXPECT_EQ(scaled->width, 120u);
    EXPECT_EQ(scaled->height, 80u);
    EXPECT_LE(scaled->x + scaled->width, 1920u);
    EXPECT_LE(scaled->y + scaled->height, 1080u);
}

TEST(Roi, ARoiCompletelyOutsideTheFrameIsDropped) {
    RoiScaler scaler{1920, 1080};
    VideoRegion roi;
    roi.x = 5000;
    roi.y = 5000;
    roi.width = 100;
    roi.height = 100;
    scaler.set_roi(roi);

    EXPECT_FALSE(scaler.scale(1920, 1080).has_value());
}

TEST(Roi, ADegenerateRoiIsDropped) {
    RoiScaler scaler{1920, 1080};
    VideoRegion roi;
    roi.x = 10;
    roi.y = 10;
    roi.width = 0;  // empty
    roi.height = 100;
    scaler.set_roi(roi);

    EXPECT_FALSE(scaler.scale(1920, 1080).has_value());
}

TEST(Roi, ARoiBelowTheMinimumSizeIsDropped) {
    RoiScaler scaler{1920, 1080, 16};
    VideoRegion roi;
    roi.x = 10;
    roi.y = 10;
    roi.width = 8;  // below the floor
    roi.height = 100;
    scaler.set_roi(roi);

    EXPECT_FALSE(scaler.scale(1920, 1080).has_value());

    // The drop is counted -- an ROI that silently stops being honoured
    // must be visible.
    EXPECT_FALSE(scaler.scaled(1920, 1080).has_value());
    EXPECT_EQ(scaler.dropped_count(), 1u);
}

TEST(Roi, TheMinimumSizeCanBeDisabled) {
    RoiScaler scaler{1920, 1080, 0};
    VideoRegion roi;
    roi.x = 10;
    roi.y = 10;
    roi.width = 4;
    roi.height = 4;
    scaler.set_roi(roi);

    const auto scaled = scaler.scale(1920, 1080);
    ASSERT_TRUE(scaled.has_value());
    EXPECT_EQ(scaled->width, 4u);
}

TEST(Roi, ChangingTheRoiInvalidatesTheCache) {
    RoiScaler scaler{1920, 1080};
    VideoRegion roi;
    roi.x = 0;
    roi.y = 0;
    roi.width = 100;
    roi.height = 100;
    scaler.set_roi(roi);

    const auto first = scaler.scaled(1920, 1080);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->width, 100u);

    roi.x = 500;
    roi.width = 200;
    scaler.set_roi(roi);

    const auto second = scaler.scaled(1920, 1080);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->x, 500u);
    EXPECT_EQ(second->width, 200u);
}

TEST(Roi, TheCachedScalingIsReused) {
    RoiScaler scaler{1920, 1080};
    VideoRegion roi;
    roi.x = 0;
    roi.y = 0;
    roi.width = 100;
    roi.height = 100;
    scaler.set_roi(roi);

    EXPECT_TRUE(scaler.scaled(1920, 1080).has_value());
    EXPECT_TRUE(scaler.scaled(1920, 1080).has_value());
    EXPECT_TRUE(scaler.scaled(1920, 1080).has_value());
    // Nothing was dropped, so the counter stayed at zero.
    EXPECT_EQ(scaler.dropped_count(), 0u);
}

TEST(Roi, ClearingTheRoiClearsTheResult) {
    RoiScaler scaler{1920, 1080};
    VideoRegion roi;
    roi.x = 0;
    roi.y = 0;
    roi.width = 100;
    roi.height = 100;
    scaler.set_roi(roi);
    EXPECT_TRUE(scaler.scaled(1920, 1080).has_value());

    scaler.set_roi(std::nullopt);
    EXPECT_FALSE(scaler.has_roi());
    EXPECT_FALSE(scaler.scaled(1920, 1080).has_value());
}

TEST(Roi, AZeroCaptureResolutionDropsTheRoi) {
    RoiScaler scaler{0, 0, 0};
    VideoRegion roi;
    roi.x = 0;
    roi.y = 0;
    roi.width = 100;
    roi.height = 100;
    scaler.set_roi(roi);

    EXPECT_FALSE(scaler.scale(1920, 1080).has_value());
}

} // namespace
} // namespace nimrtc::video_pipeline
