/**
 * @file tests/test_engine_video_codec_backend.cpp
 * @brief The `codec_backend` selector (remote-desktop plan B2).
 *
 * ## What the plan asks for
 *
 * B2's acceptance criterion is three sentences:
 *
 *   1. "On a machine with no hardware encoder the pipeline still runs
 *      (software path) and *says so* in the stats."
 *   2. "On a machine with one, the hardware path is selected and
 *      reported."
 *   3. "An explicit `software` override really does use software."
 *
 * The third one is the interesting one, because it is the only one that
 * can be tested on *any* machine: the test pins the backend class and
 * checks that the codec that comes back is of that class.  On a machine
 * with no hardware encoder, criterion 3 is the observable case of
 * criterion 1 -- the "auto" path lands on software, and the stats say
 * so.
 *
 * ## The design
 *
 * The test drives the *registry* directly for the selection logic (that
 * is where the policy lives), and the *engine* for the reporting (that
 * is where the host reads the answer).  Both are needed: a selection
 * policy that works but is not reported fails the plan just as much as
 * a reported selection that does not work.
 */

#include <cstdint>
#include <string>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/hw_video_backend.hpp>
#include <nimrtc/plugins/video_codec.hpp>

#include <gtest/gtest.h>

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
namespace np = nimrtc::plugins;

// ---------------------------------------------------------------------------
// The preference parsing
// ---------------------------------------------------------------------------

TEST(VideoCodecBackendPreference, TheSpellingIsParsed) {
    np::VideoCodecBackendPreference pref{};

    EXPECT_TRUE(np::parse_video_codec_backend_preference("auto", pref));
    EXPECT_EQ(pref, np::VideoCodecBackendPreference::kAuto);

    EXPECT_TRUE(np::parse_video_codec_backend_preference("hardware", pref));
    EXPECT_EQ(pref, np::VideoCodecBackendPreference::kHardware);

    EXPECT_TRUE(np::parse_video_codec_backend_preference("software", pref));
    EXPECT_EQ(pref, np::VideoCodecBackendPreference::kSoftware);

    // The empty string means "auto" -- the default must be spelled the
    // same way as the explicit request.
    EXPECT_TRUE(np::parse_video_codec_backend_preference("", pref));
    EXPECT_EQ(pref, np::VideoCodecBackendPreference::kAuto);
}

TEST(VideoCodecBackendPreference, AnUnknownSpellingIsRejected) {
    np::VideoCodecBackendPreference pref{};

    // A typo must not be read as "auto": a host that wrote "hardwre"
    // and silently got software would have a frame budget it cannot
    // meet, and would not know why.
    EXPECT_FALSE(np::parse_video_codec_backend_preference("hardwre", pref));
    EXPECT_FALSE(np::parse_video_codec_backend_preference("gpu", pref));
    EXPECT_FALSE(np::parse_video_codec_backend_preference("nvenc", pref));
}

TEST(VideoCodecBackendPreference, TheNameRoundTrips) {
    EXPECT_EQ(np::video_codec_backend_preference_name(
                  np::VideoCodecBackendPreference::kAuto), "auto");
    EXPECT_EQ(np::video_codec_backend_preference_name(
                  np::VideoCodecBackendPreference::kHardware), "hardware");
    EXPECT_EQ(np::video_codec_backend_preference_name(
                  np::VideoCodecBackendPreference::kSoftware), "software");
}

// ---------------------------------------------------------------------------
// The selection policy
// ---------------------------------------------------------------------------

namespace {

/**
 * @brief The codec family the fake backends register under.
 *
 * VP8 is a real codec family that no module in this tree registers a
 * backend for, so the fakes cannot collide with a real backend.
 */
constexpr np::VideoCodecKind kFakeKind = np::VideoCodecKind::kVP8;

/**
 * @brief Build a fake encoder backend for the test.
 *
 * The fake carries an explicit `is_hw` flag, so the test can build the
 * exact topology it wants (a hardware backend, a software backend, or
 * both) without depending on what the host machine happens to have.
 */
np::VideoEncoderBackend MakeFakeEncoder(std::string_view id,
                                         bool is_hw,
                                         int priority,
                                         bool available) {
    np::VideoEncoderBackend be{};
    be.id          = id;
    be.description = id;
    be.backend     = is_hw ? np::HwBackend::Nvenc : np::HwBackend::Software;
    be.priority    = priority;
    be.codec_kind  = kFakeKind;
    be.is_hw       = is_hw;
    be.available   = [available](const np::VideoCodecConfig&) {
        return available;
    };
    // The factory is never called by the selection tests -- only the
    // selection policy is under test here.
    be.create      = nullptr;
    return be;
}

/**
 * @brief The registry with the fake codec family's backends removed.
 *
 * The fakes accumulate across tests, so each test starts from a clean
 * slate for `kFakeKind`.  The clear is *kind-scoped* on purpose:
 * `clear_for_tests()` empties the whole registry, and the modules
 * register their backends exactly once -- so a test that called it
 * would permanently remove the real H.264 / H.265 / AV1 backends and
 * break every later test that opens a real engine.
 */
np::HwVideoBackendRegistry& FreshFakeRegistry() {
    auto& reg = np::HwVideoBackendRegistry::instance();
    reg.clear_for_kind_for_tests(kFakeKind);
    return reg;
}

}  // namespace

TEST(VideoCodecBackendSelection, AutoPrefersHardwareOverSoftware) {
    auto& reg = FreshFakeRegistry();

    reg.register_encoder(MakeFakeEncoder("fake_sw", false, 50, true));
    reg.register_encoder(MakeFakeEncoder("fake_hw", true,  400, true));

    const auto* be = reg.select_encoder(
        kFakeKind, std::string_view{},
        np::VideoCodecBackendPreference::kAuto);

    ASSERT_NE(be, nullptr);
    EXPECT_EQ(std::string(be->id), "fake_hw")
        << "auto must pick the hardware backend when one is available";
    EXPECT_TRUE(be->is_hw);
}

TEST(VideoCodecBackendSelection, AutoFallsBackToSoftwareAndTheStatsSaySo) {
    auto& reg = FreshFakeRegistry();

    // Only a software backend exists -- the "machine with no hardware
    // encoder" case.
    reg.register_encoder(MakeFakeEncoder("fake_sw", false, 50, true));

    const auto* be = reg.select_encoder(
        kFakeKind, std::string_view{},
        np::VideoCodecBackendPreference::kAuto);

    ASSERT_NE(be, nullptr);
    EXPECT_EQ(std::string(be->id), "fake_sw");
    EXPECT_FALSE(be->is_hw)
        << "the backend must report that it is not hardware-accelerated";
}

TEST(VideoCodecBackendSelection, HardwareOnlyNeverReturnsASoftwareBackend) {
    auto& reg = FreshFakeRegistry();

    reg.register_encoder(MakeFakeEncoder("fake_sw", false, 50, true));

    const auto* be = reg.select_encoder(
        kFakeKind, std::string_view{},
        np::VideoCodecBackendPreference::kHardware);

    EXPECT_EQ(be, nullptr)
        << "asking for hardware when there is none must fail, not fall back";
}

TEST(VideoCodecBackendSelection, SoftwareOnlyNeverReturnsAHardwareBackend) {
    auto& reg = FreshFakeRegistry();

    reg.register_encoder(MakeFakeEncoder("fake_hw", true, 400, true));

    const auto* be = reg.select_encoder(
        kFakeKind, std::string_view{},
        np::VideoCodecBackendPreference::kSoftware);

    EXPECT_EQ(be, nullptr)
        << "asking for software when only hardware exists must fail";
}

TEST(VideoCodecBackendSelection, SoftwareOnlyPicksTheSoftwareBackend) {
    auto& reg = FreshFakeRegistry();

    reg.register_encoder(MakeFakeEncoder("fake_sw", false, 50, true));
    reg.register_encoder(MakeFakeEncoder("fake_hw", true,  400, true));

    const auto* be = reg.select_encoder(
        kFakeKind, std::string_view{},
        np::VideoCodecBackendPreference::kSoftware);

    ASSERT_NE(be, nullptr);
    EXPECT_EQ(std::string(be->id), "fake_sw")
        << "the explicit software override must really use software";
    EXPECT_FALSE(be->is_hw);
}

TEST(VideoCodecBackendSelection, AnUnavailableBackendIsNotSelected) {
    auto& reg = FreshFakeRegistry();

    // A hardware backend that is registered but not available (no driver).
    reg.register_encoder(MakeFakeEncoder("fake_hw", true, 400, false));
    reg.register_encoder(MakeFakeEncoder("fake_sw", false, 50, true));

    const auto* be = reg.select_encoder(
        kFakeKind, std::string_view{},
        np::VideoCodecBackendPreference::kAuto);

    ASSERT_NE(be, nullptr);
    EXPECT_EQ(std::string(be->id), "fake_sw")
        << "an unavailable backend must not be selected, even in auto mode";
}

TEST(VideoCodecBackendSelection, AnExplicitNameContradictingTheClassFails) {
    auto& reg = FreshFakeRegistry();

    reg.register_encoder(MakeFakeEncoder("fake_sw", false, 50, true));

    // The host named a software backend but asked for hardware.  The
    // honest answer is "no match" -- not a silent override in either
    // direction.
    const auto* be = reg.select_encoder(
        kFakeKind, "fake_sw",
        np::VideoCodecBackendPreference::kHardware);

    EXPECT_EQ(be, nullptr);
}

// ---------------------------------------------------------------------------
// The engine's reporting
// ---------------------------------------------------------------------------

TEST(EngineVideoCodecBackend, TheSelectedBackendIsReported) {
    nimrtc::core::register_all_default_plugins();

    EngineConfig cfg{};
    cfg.transport_strategy_name = "shell";
    cfg.transport_name          = "ice";
    cfg.video_codec_name        = "h264";
    cfg.video_sender_name       = "reference";
    cfg.video_receiver_name     = "reference";
    cfg.video_source_name       = "memory";
    cfg.video_sink_name         = "headless";
    cfg.video_width             = 320;
    cfg.video_height            = 240;
    cfg.video_codec_backend     = "software";

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto stats = engine.stats();

    // The preference is reported verbatim.
    EXPECT_EQ(stats.video_codec_backend_preference, "software");

    // And the backend that was actually used.  On a machine with no
    // hardware encoder (the CI case) the software override lands on a
    // software backend, and the stats say so -- which is exactly what
    // B2's acceptance criterion asks for.
    EXPECT_NE(stats.video_codec_backend, "unknown")
        << "the engine must report which backend it selected";
    EXPECT_FALSE(stats.video_codec_hardware_accelerated)
        << "the explicit software override must not produce a "
           "hardware-accelerated backend";
}

TEST(EngineVideoCodecBackend, AnUnknownSpellingFailsTheOpen) {
    nimrtc::core::register_all_default_plugins();

    EngineConfig cfg{};
    cfg.transport_strategy_name = "shell";
    cfg.transport_name          = "ice";
    cfg.video_codec_name        = "h264";
    cfg.video_sender_name       = "reference";
    cfg.video_receiver_name     = "reference";
    cfg.video_source_name       = "memory";
    cfg.video_sink_name         = "headless";
    cfg.video_codec_backend     = "hardwre";   // a typo

    NimRTCEngine engine(cfg);
    EXPECT_NE(engine.open(), 0u)
        << "an unrecognised backend preference must fail the open, not "
           "silently fall back to auto";
}
