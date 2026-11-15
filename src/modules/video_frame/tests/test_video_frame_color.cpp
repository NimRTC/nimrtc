/**
 * @file src/modules/video_frame/tests/test_video_frame_color.cpp
 * @brief Unit tests for the colour-aware pixel conversions.
 *
 * Covers the remote-desktop plan's C3 / C5 rows:
 *
 *   - **C3** — format normalisation: the capture path may deliver BGRA /
 *     NV12 / I420, the encoder path needs one canonical format, and the
 *     *format actually used* is reported.
 *   - **C5** — colour range / matrix correctness: screen content is
 *     full-range BT.709 and camera content is limited-range BT.601, and
 *     getting it wrong produces a picture that looks like "the codec is
 *     broken".
 *
 * The properties pinned here:
 *
 *   1. A **round trip** (BGRA -> I420 -> BGRA) reproduces the source
 *      within a small tolerance, for *every* matrix / range pair.  This
 *      is the property that matters: if the forward and the inverse
 *      conversions disagree, the picture is visibly wrong.
 *   2. The **ranges differ**: the same RGB pixel produces a *different*
 *      luma under full range and under limited range.  A conversion that
 *      ignored the range would fail this.
 *   3. The **matrices differ**: the same RGB pixel produces a different
 *      chroma under BT.601 and BT.709.
 *   4. An **unspecified** description resolves to the desktop default
 *      (full range, BT.709).
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <vector>

#include <nimrtc/video_frame/frame.hpp>

using nimrtc::video_frame::ColorDescription;
using nimrtc::video_frame::ColorMatrix;
using nimrtc::video_frame::ColorRange;
using nimrtc::video_frame::convert_bgra_to_i420_color;
using nimrtc::video_frame::convert_bgra_to_nv12_color;
using nimrtc::video_frame::convert_i420_to_bgra_color;
using nimrtc::video_frame::resolve_color_description;

namespace {

constexpr std::uint32_t kW = 64;
constexpr std::uint32_t kH = 32;

/**
 * A screen-like test picture.
 *
 * The shape of this image matters.  A remote-desktop picture is mostly
 * *flat* panels with *sharp* text-like edges, and that is what the
 * 4:2:0 chroma subsampling has to survive.  A per-pixel noise pattern
 * (every pixel a different colour) is the pathological case for 4:2:0:
 * it aliases by construction, and a round trip through it can never be
 * tight — so a test built on noise would measure the subsampling, not
 * the colour maths.
 *
 * The picture therefore mixes:
 *   - a flat dark chrome panel,
 *   - a flat light panel,
 *   - a panel of alternating "ink" strokes (text-like detail).
 */
std::vector<std::uint8_t> make_bgra() {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kW) * kH * 4u);
    for (std::uint32_t y = 0; y < kH; ++y) {
        for (std::uint32_t x = 0; x < kW; ++x) {
            const std::size_t off =
                (static_cast<std::size_t>(y) * kW + x) * 4u;

            std::uint8_t r = 0;
            std::uint8_t g = 0;
            std::uint8_t b = 0;
            if (x < kW / 3) {
                r = 30; g = 30; b = 30;                       // dark chrome
            } else if (y < kH / 2) {
                r = 220; g = 220; b = 220;                   // light panel
            } else {
                const bool ink = ((x / 3u) % 2u == 0u) && ((y / 4u) % 2u == 0u);
                r = ink ? 20 : 200;
                g = ink ? 20 : 210;
                b = ink ? 60 : 220;
            }

            pixels[off + 0] = b;
            pixels[off + 1] = g;
            pixels[off + 2] = r;
            pixels[off + 3] = 0xFF;
        }
    }
    return pixels;
}

/** A picture of one flat colour, for the exactness checks. */
std::vector<std::uint8_t> make_flat(std::uint8_t r,
                                     std::uint8_t g,
                                     std::uint8_t b) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kW) * kH * 4u);
    for (std::size_t i = 0; i < pixels.size() / 4u; ++i) {
        pixels[i * 4u + 0] = b;
        pixels[i * 4u + 1] = g;
        pixels[i * 4u + 2] = r;
        pixels[i * 4u + 3] = 0xFF;
    }
    return pixels;
}

/**
 * A picture of strongly saturated colours.
 *
 * The chroma difference between BT.601 and BT.709 is a *chroma* effect,
 * so a picture that is mostly grey shows almost nothing.  This one is
 * built from the primaries and the secondaries, where the two matrices
 * disagree the most.
 */
std::vector<std::uint8_t> make_saturated() {
    const std::uint8_t kCols[][3] = {
        {255, 0, 0}, {0, 255, 0}, {0, 0, 255},
        {255, 255, 0}, {0, 255, 255}, {255, 0, 255},
    };
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kW) * kH * 4u);
    for (std::uint32_t y = 0; y < kH; ++y) {
        for (std::uint32_t x = 0; x < kW; ++x) {
            const auto& c = kCols[(x / 8u + y / 8u) % 6u];
            const std::size_t off =
                (static_cast<std::size_t>(y) * kW + x) * 4u;
            pixels[off + 0] = c[2];
            pixels[off + 1] = c[1];
            pixels[off + 2] = c[0];
            pixels[off + 3] = 0xFF;
        }
    }
    return pixels;
}

/** Convert BGRA -> I420 -> BGRA and return the result. */
std::vector<std::uint8_t> round_trip(const std::vector<std::uint8_t>& src,
                                     ColorDescription color) {
    std::vector<std::uint8_t> y(static_cast<std::size_t>(kW) * kH);
    std::vector<std::uint8_t> u(static_cast<std::size_t>(kW / 2) * (kH / 2));
    std::vector<std::uint8_t> v(u.size());
    std::vector<std::uint8_t> dst(src.size());

    convert_bgra_to_i420_color(kW, kH, src.data(),
                               static_cast<std::int32_t>(kW * 4), color,
                               y.data(), static_cast<std::int32_t>(kW),
                               u.data(), static_cast<std::int32_t>(kW / 2),
                               v.data(), static_cast<std::int32_t>(kW / 2));

    convert_i420_to_bgra_color(kW, kH,
                               y.data(), static_cast<std::int32_t>(kW),
                               u.data(), static_cast<std::int32_t>(kW / 2),
                               v.data(), static_cast<std::int32_t>(kW / 2),
                               color,
                               dst.data(), static_cast<std::int32_t>(kW * 4));
    return dst;
}

/** The average absolute error per channel, in 8-bit units. */
double mean_error(const std::vector<std::uint8_t>& a,
                  const std::vector<std::uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) return 999.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        sum += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
    }
    return sum / static_cast<double>(a.size());
}

const ColorDescription kAllPairs[] = {
    {ColorRange::kFull,    ColorMatrix::kBT601},
    {ColorRange::kFull,    ColorMatrix::kBT709},
    {ColorRange::kFull,    ColorMatrix::kBT2020},
    {ColorRange::kLimited, ColorMatrix::kBT601},
    {ColorRange::kLimited, ColorMatrix::kBT709},
    {ColorRange::kLimited, ColorMatrix::kBT2020},
};

} // namespace

// ---------------------------------------------------------------------------
// The description itself
// ---------------------------------------------------------------------------

TEST(ColorDescription, UnspecifiedResolvesToTheDesktopDefault) {
    const auto resolved = resolve_color_description(ColorDescription{});
    EXPECT_EQ(resolved.range, ColorRange::kFull);
    EXPECT_EQ(resolved.matrix, ColorMatrix::kBT709);
}

TEST(ColorDescription, AStatedFieldSurvivesResolution) {
    const auto resolved = resolve_color_description(
        ColorDescription{ColorRange::kLimited, ColorMatrix::kUnspecified});
    EXPECT_EQ(resolved.range, ColorRange::kLimited);
    EXPECT_EQ(resolved.matrix, ColorMatrix::kBT709);
}

TEST(ColorDescription, AgreementIgnoresUnstatedFields) {
    const ColorDescription a{ColorRange::kFull, ColorMatrix::kBT709};
    const ColorDescription b{ColorRange::kUnspecified, ColorMatrix::kBT709};
    const ColorDescription c{ColorRange::kFull, ColorMatrix::kBT601};

    EXPECT_TRUE(a.agrees_with(b));
    EXPECT_FALSE(a.agrees_with(c));
}

TEST(ColorDescription, NamesAreStable) {
    EXPECT_EQ(nimrtc::video_frame::color_range_name(ColorRange::kFull),
              "full");
    EXPECT_EQ(nimrtc::video_frame::color_range_name(ColorRange::kLimited),
              "limited");
    EXPECT_EQ(nimrtc::video_frame::color_matrix_name(ColorMatrix::kBT709),
              "bt709");
    EXPECT_EQ(nimrtc::video_frame::color_matrix_name(ColorMatrix::kBT601),
              "bt601");
}

// ---------------------------------------------------------------------------
// C5 — the range and the matrix actually matter
// ---------------------------------------------------------------------------

TEST(ColorConversion, LimitedRangeCompressesTheChroma) {
    // Regression guard: the limited-range chroma gain must be *below*
    // one (the studio swing is 224/255 of the full swing).  A gain
    // above one pushes saturated colours past 0/255, where they are
    // clamped and lose their hue entirely — the picture comes back
    // looking like the codec is broken.
    //
    // The chroma offset of a saturated colour is the clearest probe:
    // pure red under BT.709 has a Cr offset of about +104 and a Cb
    // offset of about -38, and neither may leave the studio window
    // (Cb/Cr in 16..240).
    const auto src = make_flat(255, 0, 0);

    std::vector<std::uint8_t> y(static_cast<std::size_t>(kW) * kH);
    std::vector<std::uint8_t> u(static_cast<std::size_t>(kW / 2) * (kH / 2));
    std::vector<std::uint8_t> v(u.size());

    convert_bgra_to_i420_color(kW, kH, src.data(),
                               static_cast<std::int32_t>(kW * 4),
                               ColorDescription{ColorRange::kLimited,
                                                ColorMatrix::kBT709},
                               y.data(), static_cast<std::int32_t>(kW),
                               u.data(), static_cast<std::int32_t>(kW / 2),
                               v.data(), static_cast<std::int32_t>(kW / 2));

    // The studio window for the chroma planes is 16..240.
    for (const auto& plane : {u, v}) {
        for (const auto sample : plane) {
            EXPECT_GE(sample, 16);
            EXPECT_LE(sample, 240);
        }
    }

    // And the saturation must be *reduced*, not amplified: the full
    // range version of the same pixel has a strictly larger chroma
    // excursion from the pedestal.
    std::vector<std::uint8_t> u_full(u.size());
    std::vector<std::uint8_t> v_full(v.size());
    convert_bgra_to_i420_color(kW, kH, src.data(),
                               static_cast<std::int32_t>(kW * 4),
                               ColorDescription{ColorRange::kFull,
                                                ColorMatrix::kBT709},
                               y.data(), static_cast<std::int32_t>(kW),
                               u_full.data(), static_cast<std::int32_t>(kW / 2),
                               v_full.data(), static_cast<std::int32_t>(kW / 2));

    // The excursion of a flat plane is its distance from the pedestal
    // (128), not its spread: every sample of a flat plane is equal.
    const auto excursion = [](const std::vector<std::uint8_t>& plane) {
        int worst = 0;
        for (const auto s : plane) {
            worst = std::max(worst, std::abs(static_cast<int>(s) - 128));
        }
        return worst;
    };
    EXPECT_LT(excursion(v), excursion(v_full));
    EXPECT_LT(excursion(u), excursion(u_full));
}

TEST(ColorConversion, FullRangeAndLimitedRangeProduceDifferentLuma) {
    const auto src = make_bgra();

    std::vector<std::uint8_t> y_full(static_cast<std::size_t>(kW) * kH);
    std::vector<std::uint8_t> y_limited(y_full.size());
    std::vector<std::uint8_t> u(static_cast<std::size_t>(kW / 2) * (kH / 2));
    std::vector<std::uint8_t> v(u.size());

    convert_bgra_to_i420_color(kW, kH, src.data(),
                               static_cast<std::int32_t>(kW * 4),
                               ColorDescription{ColorRange::kFull,
                                                ColorMatrix::kBT709},
                               y_full.data(), static_cast<std::int32_t>(kW),
                               u.data(), static_cast<std::int32_t>(kW / 2),
                               v.data(), static_cast<std::int32_t>(kW / 2));

    convert_bgra_to_i420_color(kW, kH, src.data(),
                               static_cast<std::int32_t>(kW * 4),
                               ColorDescription{ColorRange::kLimited,
                                                ColorMatrix::kBT709},
                               y_limited.data(), static_cast<std::int32_t>(kW),
                               u.data(), static_cast<std::int32_t>(kW / 2),
                               v.data(), static_cast<std::int32_t>(kW / 2));

    // The limited-range luma is compressed into 16..235, so it must not
    // be the same picture.
    EXPECT_GT(mean_error(y_full, y_limited), 1.0);

    // And the limited-range luma must actually be inside the studio
    // window (the pure-black / pure-white pixels are pushed inwards).
    std::uint8_t min_y = 255;
    std::uint8_t max_y = 0;
    for (const auto v_ : y_limited) {
        min_y = std::min(min_y, v_);
        max_y = std::max(max_y, v_);
    }
    EXPECT_GE(min_y, 16);
    EXPECT_LE(max_y, 235);
}

TEST(ColorConversion, Bt601AndBt709ProduceDifferentChroma) {
    // A saturated picture: the two matrices disagree the most on the
    // primaries, and a grey picture would not show the difference at
    // all.
    const auto src = make_saturated();

    std::vector<std::uint8_t> y(static_cast<std::size_t>(kW) * kH);
    std::vector<std::uint8_t> u601(static_cast<std::size_t>(kW / 2) * (kH / 2));
    std::vector<std::uint8_t> v601(u601.size());
    std::vector<std::uint8_t> u709(u601.size());
    std::vector<std::uint8_t> v709(u601.size());

    convert_bgra_to_i420_color(kW, kH, src.data(),
                               static_cast<std::int32_t>(kW * 4),
                               ColorDescription{ColorRange::kFull,
                                                ColorMatrix::kBT601},
                               y.data(), static_cast<std::int32_t>(kW),
                               u601.data(), static_cast<std::int32_t>(kW / 2),
                               v601.data(), static_cast<std::int32_t>(kW / 2));

    convert_bgra_to_i420_color(kW, kH, src.data(),
                               static_cast<std::int32_t>(kW * 4),
                               ColorDescription{ColorRange::kFull,
                                                ColorMatrix::kBT709},
                               y.data(), static_cast<std::int32_t>(kW),
                               u709.data(), static_cast<std::int32_t>(kW / 2),
                               v709.data(), static_cast<std::int32_t>(kW / 2));

    EXPECT_GT(mean_error(u601, u709), 0.5);
    EXPECT_GT(mean_error(v601, v709), 0.5);
}

// ---------------------------------------------------------------------------
// The round trip
// ---------------------------------------------------------------------------

TEST(ColorConversion, RoundTripReproducesTheSource) {
    const auto src = make_bgra();
    for (const auto& color : kAllPairs) {
        const auto dst = round_trip(src, color);
        // A tolerance of ~2 levels covers the fixed-point rounding of the
        // 4:2:0 chroma subsampling (which loses one chroma sample per
        // 2x2 block) without hiding a real matrix / range error.  A
        // wrong matrix shows up as a hue shift of tens of levels; a
        // wrong range shows up as a luma offset of tens of levels.
        EXPECT_LT(mean_error(src, dst), 2.0)            << "round trip failed for range="
            << nimrtc::video_frame::color_range_name(color.range)
            << " matrix="
            << nimrtc::video_frame::color_matrix_name(color.matrix);
    }
}

TEST(ColorConversion, FlatColoursSurviveTheRoundTripAlmostExactly) {
    // A flat colour is the strongest check of the *matrix* and the
    // *range* maths: the luma and both chroma offsets are constant, so
    // the 4:2:0 subsampling loses nothing and the only error left is
    // the 8-bit quantisation of the chroma planes.
    //
    // That quantisation is the floor: the chroma is stored as an 8-bit
    // offset from a pedestal, so a half-level error in the stored
    // chroma comes back as a one-level error in the reconstructed RGB.
    // A tolerance of ~1.5 levels therefore *is* the exactness floor,
    // and it is still two orders of magnitude below what a wrong matrix
    // or a wrong range produces (tens of levels of hue / luma shift).
    struct Case {
        std::uint8_t r;
        std::uint8_t g;
        std::uint8_t b;
    };
    const Case kCases[] = {
        {0, 0, 0}, {255, 255, 255}, {200, 100, 50},
        {0, 255, 0}, {255, 0, 255}, {128, 128, 128},
        {255, 0, 0}, {0, 0, 255},
    };

    for (const auto& c : kCases) {
        const auto src = make_flat(c.r, c.g, c.b);
        for (const auto& color : kAllPairs) {
            const auto dst = round_trip(src, color);
            EXPECT_LT(mean_error(src, dst), 1.5)
                << "flat (" << static_cast<int>(c.r) << ","
                << static_cast<int>(c.g) << "," << static_cast<int>(c.b)
                << ") failed for range="
                << nimrtc::video_frame::color_range_name(color.range)
                << " matrix="
                << nimrtc::video_frame::color_matrix_name(color.matrix);
        }
    }
}

TEST(ColorConversion, TheOutputDescriptionIsTheResolvedOne) {
    ColorDescription out{};
    const auto src = make_bgra();
    std::vector<std::uint8_t> y(static_cast<std::size_t>(kW) * kH);
    std::vector<std::uint8_t> u(static_cast<std::size_t>(kW / 2) * (kH / 2));
    std::vector<std::uint8_t> v(u.size());

    convert_bgra_to_i420_color(kW, kH, src.data(),
                               static_cast<std::int32_t>(kW * 4),
                               ColorDescription{ColorRange::kUnspecified,
                                                ColorMatrix::kUnspecified},
                               y.data(), static_cast<std::int32_t>(kW),
                               u.data(), static_cast<std::int32_t>(kW / 2),
                               v.data(), static_cast<std::int32_t>(kW / 2),
                               &out);

    EXPECT_EQ(out.range, ColorRange::kFull);
    EXPECT_EQ(out.matrix, ColorMatrix::kBT709);
}

// ---------------------------------------------------------------------------
// NV12
// ---------------------------------------------------------------------------

TEST(ColorConversion, Nv12AndI420AgreeOnTheSameDescription) {
    const auto src = make_bgra();
    const ColorDescription color{ColorRange::kFull, ColorMatrix::kBT709};

    std::vector<std::uint8_t> y_i420(static_cast<std::size_t>(kW) * kH);
    std::vector<std::uint8_t> u_i420(static_cast<std::size_t>(kW / 2) * (kH / 2));
    std::vector<std::uint8_t> v_i420(u_i420.size());

    convert_bgra_to_i420_color(kW, kH, src.data(),
                               static_cast<std::int32_t>(kW * 4), color,
                               y_i420.data(), static_cast<std::int32_t>(kW),
                               u_i420.data(), static_cast<std::int32_t>(kW / 2),
                               v_i420.data(), static_cast<std::int32_t>(kW / 2));

    std::vector<std::uint8_t> y_nv12(static_cast<std::size_t>(kW) * kH);
    std::vector<std::uint8_t> uv_nv12(static_cast<std::size_t>(kW) * (kH / 2));

    convert_bgra_to_nv12_color(kW, kH, src.data(),
                               static_cast<std::int32_t>(kW * 4), color,
                               y_nv12.data(), static_cast<std::int32_t>(kW),
                               uv_nv12.data(), static_cast<std::int32_t>(kW));
    // The luma planes are identical.
    EXPECT_LT(mean_error(y_i420, y_nv12), 0.01);

    // The interleaved UV plane carries the same chroma as the planar
    // one: U at the even byte, V at the odd byte.
    double sum = 0.0;
    std::size_t n = 0;
    for (std::uint32_t y = 0; y < kH / 2; ++y) {
        for (std::uint32_t x = 0; x < kW / 2; ++x) {
            const std::size_t uv = static_cast<std::size_t>(y) * kW + x * 2;
            const std::size_t pl = static_cast<std::size_t>(y) * (kW / 2) + x;
            sum += std::abs(static_cast<int>(uv_nv12[uv + 0]) -
                            static_cast<int>(u_i420[pl]));
            sum += std::abs(static_cast<int>(uv_nv12[uv + 1]) -
                            static_cast<int>(v_i420[pl]));
            n += 2;
        }
    }
    EXPECT_LT(sum / static_cast<double>(n), 0.01);
}

// ---------------------------------------------------------------------------
// Degenerate inputs
// ---------------------------------------------------------------------------

TEST(ColorConversion, NullInputsAreIgnored) {
    std::vector<std::uint8_t> y(static_cast<std::size_t>(kW) * kH);
    std::vector<std::uint8_t> u(static_cast<std::size_t>(kW / 2) * (kH / 2));
    std::vector<std::uint8_t> v(u.size());

    // A null source must not crash.
    convert_bgra_to_i420_color(kW, kH, nullptr, 0,
                               ColorDescription{}, y.data(),
                               static_cast<std::int32_t>(kW),
                               u.data(), static_cast<std::int32_t>(kW / 2),
                               v.data(), static_cast<std::int32_t>(kW / 2));

    // A null destination must not crash either.
    const auto src = make_bgra();
    convert_bgra_to_i420_color(kW, kH, src.data(),
                               static_cast<std::int32_t>(kW * 4),
                               ColorDescription{},
                               nullptr, 0, nullptr, 0, nullptr, 0);
}
