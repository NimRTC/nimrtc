/**
 * @file src/modules/h265/tests/test_hw_backends.cpp
 * @brief Unit tests for the H.265 hardware backend table.
 *
 * The remote-desktop plan's B5 asks for the HEVC table to carry the
 * *non-NVIDIA* rows — VA-API, Quick Sync, AMF, VideoToolbox and the
 * Android MediaCodec seam — and asks for the acceptance:
 *
 *   "unsupported combinations are **explicitly reported**, not silently
 *    ignored"
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

#include <nimrtc/h265/codec_plugin.hpp>
#include <nimrtc/h265/hw_backends.hpp>
#include <nimrtc/plugins/hw_video_backend.hpp>
#include <nimrtc/plugins/video_codec.hpp>

namespace h265 = nimrtc::h265;
namespace plugins = nimrtc::plugins;

namespace {

const plugins::VideoEncoderBackend* find_encoder(const char* id) {
    auto& reg = plugins::HwVideoBackendRegistry::instance();
    for (const auto* be : reg.list_encoders(plugins::VideoCodecKind::kH265)) {
        if (std::string(be->id) == id) return be;
    }
    return nullptr;
}

const plugins::VideoDecoderBackend* find_decoder(const char* id) {
    auto& reg = plugins::HwVideoBackendRegistry::instance();
    for (const auto* be : reg.list_decoders(plugins::VideoCodecKind::kH265)) {
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

/** The rows the B5 plan requires, with their documented priorities.
 *
 *  `always_available` marks the synthetic stub, which is available on
 *  every host and therefore carries no unavailability reason. */
struct ExpectedRow {
    const char* id;
    int priority;
    bool always_available = false;
};

constexpr ExpectedRow kExpectedEncoderRows[] = {
    {"nvenc_h265", 450},
    {"mediacodec_h265", 420},
    {"videotoolbox_h265", 400},
    {"amf_h265", 380},
    {"vaapi_h265", 360},
    {"quicksync_h265", 340},
    {"stub_h265", 0, true},
};

constexpr ExpectedRow kExpectedDecoderRows[] = {
    {"nvdec_h265", 430},
    {"videotoolbox_h265", 400},
    {"stub_h265", 0, true},
};

} // namespace

// ---------------------------------------------------------------------------
// The rows exist
// ---------------------------------------------------------------------------

TEST(H265HwBackends, RegistryPopulated) {
    h265::register_default_video_backends();
    auto& reg = plugins::HwVideoBackendRegistry::instance();

    EXPECT_FALSE(reg.list_encoders(plugins::VideoCodecKind::kH265).empty());
    EXPECT_FALSE(reg.list_decoders(plugins::VideoCodecKind::kH265).empty());
}

TEST(H265HwBackends, EveryExpectedEncoderRowExistsWithItsPriority) {
    h265::register_default_video_backends();

    for (const auto& row : kExpectedEncoderRows) {
        const auto* be = find_encoder(row.id);
        ASSERT_NE(be, nullptr) << "missing H.265 encoder row: " << row.id;
        EXPECT_EQ(be->priority, row.priority) << "row: " << row.id;
        EXPECT_EQ(be->codec_kind, plugins::VideoCodecKind::kH265);
    }
}

TEST(H265HwBackends, EveryExpectedDecoderRowExistsWithItsPriority) {
    h265::register_default_video_backends();

    for (const auto& row : kExpectedDecoderRows) {
        const auto* be = find_decoder(row.id);
        ASSERT_NE(be, nullptr) << "missing H.265 decoder row: " << row.id;
        EXPECT_EQ(be->priority, row.priority) << "row: " << row.id;
        EXPECT_EQ(be->codec_kind, plugins::VideoCodecKind::kH265);
    }
}

// ---------------------------------------------------------------------------
// The rows are honest: unavailable means "unavailable *and* explained"
// ---------------------------------------------------------------------------

TEST(H265HwBackends, UnavailableRowsReportANonEmptyReason) {
    h265::register_default_video_backends();

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

TEST(H265HwBackends, UnavailableDecoderRowsReportANonEmptyReason) {
    h265::register_default_video_backends();

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
TEST(H265HwBackends, VideoToolboxRowIsUnavailableOnThisHostWithAReason) {
    h265::register_default_video_backends();

    const auto* be = find_encoder("videotoolbox_h265");
    ASSERT_NE(be, nullptr);

    const auto cfg = test_config();
    EXPECT_FALSE(be->available(cfg));
    const std::string reason = be->unavailable_reason(cfg);
    EXPECT_FALSE(reason.empty());
    EXPECT_NE(reason.find("VideoToolbox"), std::string::npos);

    // The factory must not hand out a codec that cannot work.
    ASSERT_TRUE(static_cast<bool>(be->create));
    EXPECT_FALSE(be->create(cfg));
}
#endif

// ---------------------------------------------------------------------------
// Priority ordering
// ---------------------------------------------------------------------------

TEST(H265HwBackends, PriorityOrderingIsSensibleAndStable) {
    h265::register_default_video_backends();

    // The ladder, highest first:
    //   nvenc 450 > mediacodec 420 > videotoolbox 400 > amf 380 >
    //   vaapi 360 > quicksync 340 > stub 0
    int previous = 100000;
    for (const auto& row : kExpectedEncoderRows) {
        const auto* be = find_encoder(row.id);
        ASSERT_NE(be, nullptr) << row.id;
        EXPECT_LT(be->priority, previous)
            << "priority ladder is out of order at row: " << row.id;
        previous = be->priority;
    }

    // The registry's own snapshot must be sorted the same way.
    const auto encoders =
        plugins::HwVideoBackendRegistry::instance().list_encoders(
            plugins::VideoCodecKind::kH265);
    for (std::size_t i = 1; i < encoders.size(); ++i) {
        EXPECT_GE(encoders[i - 1]->priority, encoders[i]->priority);
    }
}

TEST(H265HwBackends, SelectionPicksTheHighestPriorityAvailableBackend) {
    h265::register_default_video_backends();

    const auto cfg = test_config();
    const auto* be = h265::select_encoder_backend(cfg);
    ASSERT_NE(be, nullptr);

    // The winner must be *available* — the selector never hands back a
    // backend whose own probe says "no" while another one says "yes".
    EXPECT_TRUE(be->available(cfg));

    // And it must be the highest-priority available row of the table.
    int best = -1;
    for (const auto* row : plugins::HwVideoBackendRegistry::instance()
                               .list_encoders(plugins::VideoCodecKind::kH265)) {
        if (!row->available(cfg)) continue;
        if (row->priority > best) best = row->priority;
    }
    EXPECT_EQ(be->priority, best)
        << "the selector must pick the highest-priority *available* row";
}

TEST(H265HwBackends, PreferredNameRespected) {
    h265::register_default_video_backends();

    auto cfg = test_config();
    cfg.name = "stub_h265";

    const auto* be = h265::select_encoder_backend(cfg);
    ASSERT_NE(be, nullptr);
    EXPECT_EQ(std::string(be->id), "stub_h265");
}

#if !defined(__APPLE__) && !defined(__linux__)
TEST(H265HwBackends, HardwarePreferenceFailsExplicitlyWithoutHardware) {
    h265::register_default_video_backends();

    auto cfg = test_config();
    cfg.backend_preference = plugins::VideoCodecBackendPreference::kHardware;

    bool any_hw = false;
    for (const auto* be : plugins::HwVideoBackendRegistry::instance()
                              .list_encoders(plugins::VideoCodecKind::kH265)) {
        if (be->is_hw && be->available(cfg)) {
            any_hw = true;
            break;
        }
    }
    if (!any_hw) {
        // The honest answer to "I asked for hardware and there is none"
        // is nullptr, not the software stub.
        EXPECT_EQ(h265::select_encoder_backend(cfg), nullptr);
    }
}
#endif
