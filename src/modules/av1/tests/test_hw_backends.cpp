/**
 * @file src/modules/av1/tests/test_hw_backends.cpp
 * @brief Unit tests for the AV1 hardware backend table.
 *
 * The remote-desktop plan's B6 asks for the AV1 table to carry the
 * non-NVIDIA rows — AMF, VA-API, MediaCodec and VideoToolbox — and for
 * the acceptance:
 *
 *   "a backend that exists in the table but not in the silicon reports
 *    `available() == false` with a reason."
 *
 * These tests therefore assert, for every row of the table:
 *
 *   1. the row *exists* in the registry (the seam id is stable across
 *      platforms — a row is never conditionally compiled out),
 *   2. the row has a *defined priority*,
 *   3. the row's `available()` probe is callable on any host, and
 *   4. on a host that cannot use the row, `available()` is false **and
 *      `unavailable_reason()` explains why**.
 *
 * They do NOT require a GPU: the whole point is that the *seam* is
 * visible and honest on every host.
 */

#include <gtest/gtest.h>

#include <string>

#include <nimrtc/av1/codec_plugin.hpp>
#include <nimrtc/av1/hw_backends.hpp>
#include <nimrtc/plugins/hw_video_backend.hpp>
#include <nimrtc/plugins/video_codec.hpp>

namespace av1 = nimrtc::av1;
namespace plugins = nimrtc::plugins;

namespace {

const plugins::VideoEncoderBackend* find_encoder(const char* id) {
    auto& reg = plugins::HwVideoBackendRegistry::instance();
    for (const auto* be : reg.list_encoders(plugins::VideoCodecKind::kAV1)) {
        if (std::string(be->id) == id) return be;
    }
    return nullptr;
}

const plugins::VideoDecoderBackend* find_decoder(const char* id) {
    auto& reg = plugins::HwVideoBackendRegistry::instance();
    for (const auto* be : reg.list_decoders(plugins::VideoCodecKind::kAV1)) {
        if (std::string(be->id) == id) return be;
    }
    return nullptr;
}

plugins::VideoCodecConfig test_config() {
    plugins::VideoCodecConfig cfg;
    cfg.width = 1920;
    cfg.height = 1080;
    return cfg;
}

/** The rows the B6 plan requires, with their documented priorities.
 *
 *  `always_available` marks the synthetic stub, which is available on
 *  every host and therefore carries no unavailability reason. */
struct ExpectedRow {
    const char* id;
    int priority;
    bool always_available = false;
};

constexpr ExpectedRow kExpectedEncoderRows[] = {
    {"nvenc_av1", 450},
    {"mediacodec_av1", 420},
    {"videotoolbox_av1", 400},
    {"amf_av1", 380},
    {"vaapi_av1", 360},
    {"stub_av1", 0, true},
};

/** The optional software AV1 encoder (plan item B11).
 *
 *  Its row is registered ONLY when the vendored SVT-AV1 build is compiled
 *  in.  With `NIMRTC_VENDORED_SVT_AV1=OFF` (the default) the plug-in must
 *  be *absent* from the registry — that is the plan's B11 acceptance —
 *  so the row is not part of the always-present table above. */
constexpr ExpectedRow kSvtAv1EncoderRow{"svt_av1", 50};

constexpr ExpectedRow kExpectedDecoderRows[] = {
    {"nvdec_av1", 430},
    {"videotoolbox_av1", 400},
    {"stub_av1", 0, true},
};

} // namespace

// ---------------------------------------------------------------------------
// The rows exist
// ---------------------------------------------------------------------------

TEST(Av1HwBackends, RegistryPopulated) {
    av1::register_default_video_backends();
    auto& reg = plugins::HwVideoBackendRegistry::instance();

    EXPECT_FALSE(reg.list_encoders(plugins::VideoCodecKind::kAV1).empty());
    EXPECT_FALSE(reg.list_decoders(plugins::VideoCodecKind::kAV1).empty());
}

TEST(Av1HwBackends, EveryExpectedEncoderRowExistsWithItsPriority) {
    av1::register_default_video_backends();

    for (const auto& row : kExpectedEncoderRows) {
        const auto* be = find_encoder(row.id);
        ASSERT_NE(be, nullptr) << "missing AV1 encoder row: " << row.id;
        EXPECT_EQ(be->priority, row.priority) << "row: " << row.id;
        EXPECT_EQ(be->codec_kind, plugins::VideoCodecKind::kAV1);
    }
}

TEST(Av1HwBackends, EveryExpectedDecoderRowExistsWithItsPriority) {
    av1::register_default_video_backends();

    for (const auto& row : kExpectedDecoderRows) {
        const auto* be = find_decoder(row.id);
        ASSERT_NE(be, nullptr) << "missing AV1 decoder row: " << row.id;
        EXPECT_EQ(be->priority, row.priority) << "row: " << row.id;
        EXPECT_EQ(be->codec_kind, plugins::VideoCodecKind::kAV1);
    }
}

// ---------------------------------------------------------------------------
// The B11 acceptance: the SV-AV1 row exists *iff* the vendored build is
// compiled in.
//
// With `NIMRTC_VENDORED_SVT_AV1=OFF` (the default) the plug-in must be
// ABSENT from the registry — not "present but unavailable".  With the
// option ON the row must be there, at priority 50, below every hardware
// row and above the stub.
// ---------------------------------------------------------------------------

TEST(Av1HwBackends, TheSvtAv1RowIsPresentOnlyWhenTheVendoredBuildIsCompiledIn) {
    av1::register_default_video_backends();

    const auto* be = find_encoder(kSvtAv1EncoderRow.id);
#if defined(NIMRTC_PLUGINS_SVT_AV1_ON)
    ASSERT_NE(be, nullptr)
        << "the svt_av1 row must be registered when the vendored SVT-AV1 "
           "build is compiled in";
    EXPECT_EQ(be->priority, kSvtAv1EncoderRow.priority);
    EXPECT_EQ(be->codec_kind, plugins::VideoCodecKind::kAV1);
    EXPECT_FALSE(be->is_hw);
    EXPECT_FALSE(be->zero_copy_supported);
#else
    EXPECT_EQ(be, nullptr)
        << "with NIMRTC_VENDORED_SVT_AV1=OFF the SV-AV1 encoder must be "
           "absent from the registry (the plan's B11 acceptance)";
#endif
}

// ---------------------------------------------------------------------------
// The rows are honest: unavailable means "unavailable *and* explained"
// ---------------------------------------------------------------------------

TEST(Av1HwBackends, UnavailableRowsReportANonEmptyReason) {
    av1::register_default_video_backends();

    const auto cfg = test_config();

    for (const auto& row : kExpectedEncoderRows) {
        const auto* be = find_encoder(row.id);
        ASSERT_NE(be, nullptr) << row.id;

        ASSERT_TRUE(static_cast<bool>(be->available))
            << "row without an availability probe: " << row.id;

        if (!row.always_available) {
            ASSERT_TRUE(static_cast<bool>(be->unavailable_reason))
                << "row without an unavailable_reason: " << row.id;
        }

        if (!be->available(cfg)) {
            const std::string reason = be->unavailable_reason(cfg);
            EXPECT_FALSE(reason.empty())
                << "row reported unavailable without a reason: " << row.id;
        }
    }
}

TEST(Av1HwBackends, UnavailableDecoderRowsReportANonEmptyReason) {
    av1::register_default_video_backends();

    const auto cfg = test_config();

    for (const auto& row : kExpectedDecoderRows) {
        const auto* be = find_decoder(row.id);
        ASSERT_NE(be, nullptr) << row.id;

        ASSERT_TRUE(static_cast<bool>(be->available)) << row.id;

        if (!row.always_available) {
            ASSERT_TRUE(static_cast<bool>(be->unavailable_reason)) << row.id;
        }

        if (!be->available(cfg)) {
            EXPECT_FALSE(be->unavailable_reason(cfg).empty()) << row.id;
        }
    }
}

#if !defined(__APPLE__) && !defined(__linux__)
TEST(Av1HwBackends, VideoToolboxRowIsUnavailableOnThisHostWithAReason) {
    av1::register_default_video_backends();

    const auto* be = find_encoder("videotoolbox_av1");
    ASSERT_NE(be, nullptr);

    const auto cfg = test_config();
    EXPECT_FALSE(be->available(cfg));
    const std::string reason = be->unavailable_reason(cfg);
    EXPECT_FALSE(reason.empty());
    EXPECT_NE(reason.find("VideoToolbox"), std::string::npos);

    ASSERT_TRUE(static_cast<bool>(be->create));
    EXPECT_FALSE(be->create(cfg));
}
#endif

// ---------------------------------------------------------------------------
// Priority ordering
// ---------------------------------------------------------------------------

TEST(Av1HwBackends, PriorityOrderingIsSensibleAndStable) {
    av1::register_default_video_backends();

    // The ladder, highest first:
    //   nvenc 450 > mediacodec 420 > videotoolbox 400 > amf 380 >
    //   vaapi 360 > svt_av1 50 (only when the vendored build is in) >
    //   stub 0
    int previous = 100000;
    for (const auto& row : kExpectedEncoderRows) {
        const auto* be = find_encoder(row.id);
        ASSERT_NE(be, nullptr) << row.id;
        EXPECT_LT(be->priority, previous)
            << "priority ladder is out of order at row: " << row.id;
        previous = be->priority;
    }

    const auto encoders =
        plugins::HwVideoBackendRegistry::instance().list_encoders(
            plugins::VideoCodecKind::kAV1);
    for (std::size_t i = 1; i < encoders.size(); ++i) {
        EXPECT_GE(encoders[i - 1]->priority, encoders[i]->priority);
    }
}

TEST(Av1HwBackends, SelectionPicksTheHighestPriorityAvailableBackend) {
    av1::register_default_video_backends();

    const auto cfg = test_config();
    const auto* be = av1::select_encoder_backend(cfg);
    ASSERT_NE(be, nullptr);

    // The winner must be *available* — the selector never hands back a
    // backend whose own probe says "no" while another one says "yes".
    EXPECT_TRUE(be->available(cfg));

    // And it must be the highest-priority available row of the table.
    int best = -1;
    for (const auto* row : plugins::HwVideoBackendRegistry::instance()
                               .list_encoders(plugins::VideoCodecKind::kAV1)) {
        if (!row->available(cfg)) continue;
        if (row->priority > best) best = row->priority;
    }
    EXPECT_EQ(be->priority, best)
        << "the selector must pick the highest-priority *available* row";
}

TEST(Av1HwBackends, PreferredNameRespected) {
    av1::register_default_video_backends();

    auto cfg = test_config();
    cfg.name = "stub_av1";

    const auto* be = av1::select_encoder_backend(cfg);
    ASSERT_NE(be, nullptr);
    EXPECT_EQ(std::string(be->id), "stub_av1");
}

#if !defined(__APPLE__) && !defined(__linux__)
TEST(Av1HwBackends, HardwarePreferenceFailsExplicitlyWithoutHardware) {
    av1::register_default_video_backends();

    auto cfg = test_config();
    cfg.backend_preference = plugins::VideoCodecBackendPreference::kHardware;

    bool any_hw = false;
    for (const auto* be : plugins::HwVideoBackendRegistry::instance()
                              .list_encoders(plugins::VideoCodecKind::kAV1)) {
        if (be->is_hw && be->available(cfg)) {
            any_hw = true;
            break;
        }
    }
    if (!any_hw) {
        EXPECT_EQ(av1::select_encoder_backend(cfg), nullptr);
    }
}
#endif
