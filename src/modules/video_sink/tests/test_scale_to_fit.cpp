/**
 * @file modules/video_sink/tests/test_scale_to_fit.cpp
 * @brief Unit tests for the portable scale-to-fit helper.
 *
 * Remote-desktop plan E7 (full-screen / scale-to-fit presentation).
 *
 * ## What is tested here
 *
 * `compute_scale_to_fit` is a **pure function** — source size, target
 * size, mode in; destination rectangle out.  It therefore needs no
 * display, no GPU and no platform SDK, and it is tested on every
 * platform.
 *
 * The three modes are the ones the plan names:
 *
 *   - **letterbox** — the whole picture is visible, the aspect ratio is
 *     preserved and the bars are on the short axis;
 *   - **crop** — the target is filled, the aspect ratio is preserved and
 *     the overflow is cropped away;
 *   - **stretch** — the target is filled and the aspect ratio is *not*
 *     preserved.
 *
 * The degenerate cases matter as much as the nominal ones: a zero
 * dimension must not divide by zero.
 */

#include <gtest/gtest.h>

#include <cstdint>

#include <nimrtc/video_sink/scale_to_fit.hpp>

namespace {

namespace vs = nimrtc::video_sink;

using vs::ScaleMode;

// ---------------------------------------------------------------------------
// Letterbox
// ---------------------------------------------------------------------------

// A 16:9 source in a square target: the picture is 1000 wide and
// 1000 * 9 / 16 = 562.5 tall, rounded to 563, and centred vertically
// (the bars are above and below).
TEST(ScaleToFit, LetterboxKeepsTheAspectRatio) {
    const auto rect = vs::compute_scale_to_fit(1920, 1080, 1000, 1000,
                                                ScaleMode::kLetterbox);
    EXPECT_EQ(rect.w, 1000);
    EXPECT_EQ(rect.h, 563);
    // Centred vertically: (1000 - 563) / 2 = 218 (integer division).
    EXPECT_EQ(rect.x, 0);
    EXPECT_EQ(rect.y, 218);
}

// A tall source in a wide target: the bars are on the left and the
// right.
TEST(ScaleToFit, LetterboxPutsTheBarsOnTheShortAxis) {
    // 1080x1920 (9:16) into 1600x900 (16:9).
    const auto rect = vs::compute_scale_to_fit(1080, 1920, 1600, 900,
                                                ScaleMode::kLetterbox);
    // The picture fills the height: 900 * 1080 / 1920 = 506.25 -> 506.
    EXPECT_EQ(rect.h, 900);
    EXPECT_EQ(rect.w, 506);
    // Centred horizontally: (1600 - 506) / 2 = 547.
    EXPECT_EQ(rect.x, 547);
    EXPECT_EQ(rect.y, 0);
}

// A matching aspect ratio fills the target exactly — no bars.
TEST(ScaleToFit, LetterboxOfAMatchingAspectFillsTheTarget) {
    const auto rect = vs::compute_scale_to_fit(1920, 1080, 1280, 720,
                                                ScaleMode::kLetterbox);
    EXPECT_EQ(rect.x, 0);
    EXPECT_EQ(rect.y, 0);
    EXPECT_EQ(rect.w, 1280);
    EXPECT_EQ(rect.h, 720);
}

// The result never exceeds the target (a rounding artefact must not
// spill out).
TEST(ScaleToFit, LetterboxNeverExceedsTheTarget) {
    for (std::int32_t sw = 1; sw <= 40; sw += 7) {
        for (std::int32_t sh = 1; sh <= 40; sh += 5) {
            const auto rect = vs::compute_scale_to_fit(sw, sh, 37, 23,
                                                        ScaleMode::kLetterbox);
            EXPECT_LE(rect.w, 37);
            EXPECT_LE(rect.h, 23);
            EXPECT_GE(rect.x, 0);
            EXPECT_GE(rect.y, 0);
            EXPECT_LE(rect.x + rect.w, 37);
            EXPECT_LE(rect.y + rect.h, 23);
        }
    }
}

// ---------------------------------------------------------------------------
// Crop
// ---------------------------------------------------------------------------

// The crop mode fills the target exactly: the aspect ratio is preserved
// and the overflow on the long axis is cropped away.
TEST(ScaleToFit, CropFillsTheTarget) {
    const auto rect = vs::compute_scale_to_fit(1920, 1080, 1000, 1000,
                                                ScaleMode::kCrop);
    EXPECT_EQ(rect.x, 0);
    EXPECT_EQ(rect.y, 0);
    EXPECT_EQ(rect.w, 1000);
    EXPECT_EQ(rect.h, 1000);
}

// The crop mode fills the target whatever the aspect ratios are.
TEST(ScaleToFit, CropAlwaysFillsTheTarget) {
    const auto rect = vs::compute_scale_to_fit(1080, 1920, 640, 480,
                                                ScaleMode::kCrop);
    EXPECT_EQ(rect.w, 640);
    EXPECT_EQ(rect.h, 480);
}

// ---------------------------------------------------------------------------
// Stretch
// ---------------------------------------------------------------------------

// The stretch mode fills the target and ignores the aspect ratio.
TEST(ScaleToFit, StretchFillsTheTarget) {
    const auto rect = vs::compute_scale_to_fit(1920, 1080, 1000, 1000,
                                                ScaleMode::kStretch);
    EXPECT_EQ(rect.x, 0);
    EXPECT_EQ(rect.y, 0);
    EXPECT_EQ(rect.w, 1000);
    EXPECT_EQ(rect.h, 1000);
}

// ---------------------------------------------------------------------------
// Degenerate inputs
// ---------------------------------------------------------------------------

// A zero (or negative) dimension must not divide by zero.
TEST(ScaleToFit, DegenerateInputsDoNotDivideByZero) {
    for (const auto mode : {ScaleMode::kLetterbox, ScaleMode::kCrop,
                            ScaleMode::kStretch}) {
        const auto a = vs::compute_scale_to_fit(0, 1080, 1000, 1000, mode);
        const auto b = vs::compute_scale_to_fit(1920, 0, 1000, 1000, mode);
        const auto c = vs::compute_scale_to_fit(1920, 1080, 0, 1000, mode);
        const auto d = vs::compute_scale_to_fit(1920, 1080, 1000, 0, mode);
        const auto e = vs::compute_scale_to_fit(-4, -8, 1000, 1000, mode);

        EXPECT_EQ(a.w, 0);
        EXPECT_EQ(a.h, 0);
        EXPECT_EQ(b.w, 0);
        EXPECT_EQ(b.h, 0);
        EXPECT_EQ(c.w, 0);
        EXPECT_EQ(c.h, 0);
        EXPECT_EQ(d.w, 0);
        EXPECT_EQ(d.h, 0);
        EXPECT_EQ(e.w, 0);
        EXPECT_EQ(e.h, 0);
    }
}

// The letterbox of a degenerate source is empty — the caller skips the
// present.
TEST(ScaleToFit, LetterboxOfADegenerateSourceIsEmpty) {
    const auto rect = vs::compute_scale_to_fit(0, 0, 1920, 1080,
                                                ScaleMode::kLetterbox);
    EXPECT_EQ(rect.w, 0);
    EXPECT_EQ(rect.h, 0);
}

} // namespace
