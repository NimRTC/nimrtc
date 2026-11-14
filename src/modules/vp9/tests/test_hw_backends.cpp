/**
 * @file src/modules/vp9/tests/test_hw_backends.cpp
 * @brief Unit tests for the VP9 backend table.
 *
 * The remote-desktop plan's B11 asks for the *optional* software VP9
 * encoder (the vendored libvpx), behind `NIMRTC_VENDORED_LIBVPX`, which
 * **defaults OFF**.  The acceptance is:
 *
 *   "with the option OFF the build still configures and the plug-in is
 *    absent."
 *
 * So the tests here assert:
 *
 *   1. the stub row exists on every host (the seam is stable),
 *   2. the `libvpx_vp9` row is present **iff** the vendored build was
 *      compiled in — with the option OFF the row is *absent*, not
 *      "present but unavailable",
 *   3. every row that reports `available() == false` explains why,
 *   4. the selector honours the preference and the explicit name.
 *
 * The test never requires a GPU or a codec: the whole point is that the
 * *seam* is visible and honest on every host.
 */

#include <gtest/gtest.h>

#include <string>

#include <nimrtc/plugins/hw_video_backend.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/vp9/codec_plugin.hpp>
#include <nimrtc/vp9/hw_backends.hpp>

namespace vp9 = nimrtc::vp9;
namespace plugins = nimrtc::plugins;

namespace {

const plugins::VideoEncoderBackend* find_encoder(const char* id) {
    auto& reg = plugins::HwVideoBackendRegistry::instance();
    for (const auto* be : reg.list_encoders(plugins::VideoCodecKind::kVP9)) {
        if (std::string(be->id) == id) return be;
    }
    return nullptr;
}

const plugins::VideoDecoderBackend* find_decoder(const char* id) {
    auto& reg = plugins::HwVideoBackendRegistry::instance();
    for (const auto* be : reg.list_decoders(plugins::VideoCodecKind::kVP9)) {
        if (std::string(be->id) == id) return be;
    }
    return nullptr;
}

plugins::VideoCodecConfig test_config() {
    plugins::VideoCodecConfig cfg;
    cfg.width  = 640;
    cfg.height = 480;
    cfg.codec  = plugins::VideoCodecKind::kVP9;
    return cfg;
}

} // namespace

// ---------------------------------------------------------------------------
// The rows exist
// ---------------------------------------------------------------------------

TEST(Vp9HwBackends, RegistryPopulated) {
    vp9::register_default_video_backends();
    auto& reg = plugins::HwVideoBackendRegistry::instance();

    EXPECT_FALSE(reg.list_encoders(plugins::VideoCodecKind::kVP9).empty());
    EXPECT_FALSE(reg.list_decoders(plugins::VideoCodecKind::kVP9).empty());
}

TEST(Vp9HwBackends, TheStubRowAlwaysExists) {
    vp9::register_default_video_backends();

    const auto* enc = find_encoder("stub_vp9");
    ASSERT_NE(enc, nullptr) << "the stub VP9 encoder row must always exist";
    EXPECT_EQ(enc->priority, 0);
    EXPECT_EQ(enc->codec_kind, plugins::VideoCodecKind::kVP9);
    EXPECT_FALSE(enc->is_hw);
    EXPECT_FALSE(enc->zero_copy_supported);

    const auto* dec = find_decoder("stub_vp9");
    ASSERT_NE(dec, nullptr) << "the stub VP9 decoder row must always exist";
    EXPECT_EQ(dec->priority, 0);
    EXPECT_EQ(dec->codec_kind, plugins::VideoCodecKind::kVP9);
}

TEST(Vp9HwBackends, TheStubRowIsAlwaysAvailable) {
    vp9::register_default_video_backends();

    const auto cfg = test_config();
    const auto* enc = find_encoder("stub_vp9");
    ASSERT_NE(enc, nullptr);
    EXPECT_TRUE(enc->available(cfg));
}

// ---------------------------------------------------------------------------
// The B11 acceptance: with the option OFF the libvpx row is *absent*.
// ---------------------------------------------------------------------------

TEST(Vp9HwBackends, TheLibvpxRowIsPresentOnlyWhenTheVendoredBuildIsCompiledIn) {
    vp9::register_default_video_backends();

    const auto* be = find_encoder("libvpx_vp9");
#if defined(NIMRTC_PLUGINS_LIBVPX_ON)
    // The option is ON and the vendored build is compiled in: the row
    // must be there, at priority 50, above the stub.
    ASSERT_NE(be, nullptr)
        << "the libvpx row must be registered when the vendored build is "
           "compiled in";
    EXPECT_EQ(be->priority, 50);
    EXPECT_EQ(be->codec_kind, plugins::VideoCodecKind::kVP9);
    EXPECT_FALSE(be->is_hw);
    EXPECT_FALSE(be->zero_copy_supported);
#else
    // The option is OFF (the default): the plug-in must be *absent* from
    // the registry — not "present but unavailable".
    EXPECT_EQ(be, nullptr)
        << "with NIMRTC_VENDORED_LIBVPX=OFF the libvpx VP9 encoder must be "
           "absent from the registry (the plan's B11 acceptance)";
#endif
}

// ---------------------------------------------------------------------------
// The rows are honest: unavailable means "unavailable *and* explained"
// ---------------------------------------------------------------------------

TEST(Vp9HwBackends, UnavailableRowsReportANonEmptyReason) {
    vp9::register_default_video_backends();

    const auto cfg = test_config();

    for (const auto* be : plugins::HwVideoBackendRegistry::instance()
                               .list_encoders(plugins::VideoCodecKind::kVP9)) {
        ASSERT_TRUE(static_cast<bool>(be->available)) << be->id;
        if (std::string(be->id) == "stub_vp9") continue;
        ASSERT_TRUE(static_cast<bool>(be->unavailable_reason)) << be->id;
        if (!be->available(cfg)) {
            EXPECT_FALSE(be->unavailable_reason(cfg).empty()) << be->id;
        }
    }
}

TEST(Vp9HwBackends, EveryEncoderRowHasAUniqueId) {
    vp9::register_default_video_backends();

    const auto encoders =
        plugins::HwVideoBackendRegistry::instance().list_encoders(
            plugins::VideoCodecKind::kVP9);
    ASSERT_FALSE(encoders.empty());

    for (std::size_t i = 0; i < encoders.size(); ++i) {
        for (std::size_t j = i + 1; j < encoders.size(); ++j) {
            EXPECT_NE(std::string(encoders[i]->id), std::string(encoders[j]->id))
                << "duplicate VP9 encoder row id: " << encoders[i]->id;
        }
    }
}

TEST(Vp9HwBackends, PriorityOrderingIsDescending) {
    vp9::register_default_video_backends();

    const auto encoders =
        plugins::HwVideoBackendRegistry::instance().list_encoders(
            plugins::VideoCodecKind::kVP9);
    for (std::size_t i = 1; i < encoders.size(); ++i) {
        EXPECT_GE(encoders[i - 1]->priority, encoders[i]->priority);
    }
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

TEST(Vp9HwBackends, SelectionPicksTheHighestPriorityAvailableBackend) {
    vp9::register_default_video_backends();

    const auto cfg = test_config();
    const auto* be = vp9::select_encoder_backend(cfg);
    ASSERT_NE(be, nullptr);
    EXPECT_TRUE(be->available(cfg));

    int best = -1;
    for (const auto* row : plugins::HwVideoBackendRegistry::instance()
                               .list_encoders(plugins::VideoCodecKind::kVP9)) {
        if (!row->available(cfg)) continue;
        if (row->priority > best) best = row->priority;
    }
    EXPECT_EQ(be->priority, best);
}

TEST(Vp9HwBackends, PreferredNameRespected) {
    vp9::register_default_video_backends();

    auto cfg = test_config();
    cfg.name = "stub_vp9";

    const auto* be = vp9::select_encoder_backend(cfg);
    ASSERT_NE(be, nullptr);
    EXPECT_EQ(std::string(be->id), "stub_vp9");
}

TEST(Vp9HwBackends, SoftwarePreferenceResolvesToTheSoftwareRows) {
    vp9::register_default_video_backends();

    auto cfg = test_config();
    cfg.backend_preference = plugins::VideoCodecBackendPreference::kSoftware;

    const auto* be = vp9::select_encoder_backend(cfg);
    ASSERT_NE(be, nullptr);
    EXPECT_FALSE(be->is_hw)
        << "the kSoftware preference must never select a hardware row";
}

TEST(Vp9HwBackends, HardwarePreferenceFailsExplicitlyWithoutHardware) {
    vp9::register_default_video_backends();

    auto cfg = test_config();
    cfg.backend_preference = plugins::VideoCodecBackendPreference::kHardware;

    // There is no hardware VP9 encoder in this tree at all, so the
    // hardware-only selection must fail *loudly* rather than fall back
    // to the software stub (the plan's B2 rule).
    EXPECT_EQ(vp9::select_encoder_backend(cfg), nullptr);
}
