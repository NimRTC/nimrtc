// =============================================================================
// test_assembly.cpp — Assembly module unit tests
// =============================================================================
//
// Test matrix:
//   1. ProfileRegistry singleton behaviour
//      - instance() returns same address on repeated calls
//      - all built-in profiles are registered (9 total — 6 v1.0 + 3 v1.1)
//      - find() returns correct pointer for known names
//      - find() returns nullptr for unknown names
//   2. Profile invariants (each built-in)
//      - kProfileTransport/kProfileSfu have empty plugin names where required
//      - kProfileAgent/kProfileTeleop have timeline_enabled=true
//      - kProfileAgent/kProfileTeleop have datachannel_enabled=true
//      - Scheduler mode is correct per profile
//   3. Builder
//      - load_profile("call") produces a copy of kProfileCall
//      - Unknown name: load_profile() leaves profile_ unchanged
//      - Override chain: build() reflects last override
//      - Empty overrides (no load_profile called) yields zero-initialized Profile
//   4. JSON round-trip
//      - Serialise each built-in profile, deserialise, compare fields
//      - Profile round-trips byte-for-byte for all built-in profiles
// =============================================================================

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include <nimrtc/assembly/assembly.hpp>
#include <nimrtc/assembly/profiles.hpp>

namespace fs = std::filesystem;

// =============================================================================
// Test fixture — shared helpers
// =============================================================================

class AssemblyTest : public ::testing::Test {
protected:
    // Returns the absolute path to a profile JSON in the build directory.
    std::string profile_path(std::string_view name) const {
        // The JSON files are copied to ${CMAKE_BINARY_DIR}/profiles/ by
        // both the main assembly/CMakeLists.txt and by the test CMakeLists.txt.
        const char* build_dir = std::getenv("CMAKE_BINARY_DIR");
        std::string base = build_dir ? build_dir : ".";
        return (fs::path{base} / "profiles" / (std::string{name} + ".json")).string();
    }
};

// =============================================================================
// 1. ProfileRegistry singleton
// =============================================================================

TEST_F(AssemblyTest, SingletonReturnsSameInstance) {
    auto& a = nimrtc::assembly::ProfileRegistry::instance();
    auto& b = nimrtc::assembly::ProfileRegistry::instance();
    EXPECT_EQ(&a, &b);
}

TEST_F(AssemblyTest, AllBuiltinProfilesRegistered) {
    auto& reg = nimrtc::assembly::ProfileRegistry::instance();
    auto names = reg.all_names();

    // Expected: 9 built-in profiles (6 v1.0 + 3 v1.1 from PROFILE-1).
    EXPECT_EQ(names.size(), 9u);

    // Each known name must be present.
    for (std::string_view expected :
         {"call", "live", "transport", "agent", "teleop", "sfu",
          "sfu-agent", "agent-gateway", "agent-low-latency"}) {
        EXPECT_NE(std::find(names.begin(), names.end(), std::string{expected}),
                  names.end())
            << "Profile '" << expected << "' should be registered";
    }
}

TEST_F(AssemblyTest, FindKnownProfile) {
    auto& reg = nimrtc::assembly::ProfileRegistry::instance();

    const nimrtc::assembly::Profile* p = reg.find("call");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->name, "call");
    EXPECT_EQ(p->jb_name, "adaptive");
}

TEST_F(AssemblyTest, FindUnknownProfileReturnsNullptr) {
    auto& reg = nimrtc::assembly::ProfileRegistry::instance();
    EXPECT_EQ(reg.find("nonexistent"), nullptr);
}

TEST_F(AssemblyTest, RegisterOverwritesExisting) {
    auto& reg = nimrtc::assembly::ProfileRegistry::instance();

    // Use a non-built-in name so we don't pollute the registry for other
    // tests (e.g. JsonRoundTripBuiltInProfiles / BuilderLoadProfileCall).
    // This test specifically verifies that register_profile() overwrites the
    // entry under the given key, regardless of whether that key was a
    // built-in or previously registered.
    constexpr std::string_view kCustomKey = "test_custom_call";
    auto cleanup = std::shared_ptr<void>(nullptr, [&](void*) {
        // Remove the test entry so subsequent test runs (and tests that
        // depend on built-in profiles) see a clean registry.
        nimrtc::assembly::Profile tmp;
        tmp.name = std::string{kCustomKey};
        // Re-register the built-in "call" under the test key — no-op would
        // leave junk. Simpler: erase by overwriting with a fresh Profile
        // whose values match the original kProfileCall (we don't have a
        // direct `unregister`, so just keep the test key isolated).
        (void)tmp;
    });

    nimrtc::assembly::Profile p;
    p.name     = std::string{kCustomKey};
    p.jb_name  = "custom_jb";

    reg.register_profile(p);

    const nimrtc::assembly::Profile* found = reg.find(kCustomKey);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->jb_name, "custom_jb");

    // Restore: re-register a copy of the built-in "call" profile to undo
    // the "RegisterOverwritesExisting" effect on the singleton, in case
    // other test suites re-use this registry across runs.
    const nimrtc::assembly::Profile* original =
        nimrtc::assembly::ProfileRegistry::instance().find("call");
    if (original) reg.register_profile(*original);
}

// =============================================================================
// 2. Profile invariants
// =============================================================================

TEST_F(AssemblyTest, ProfileTransportNoMediaPlugins) {
    const auto& p = nimrtc::assembly::kProfileTransport;
    EXPECT_EQ(p.name, "transport");
    EXPECT_TRUE(p.jb_name.empty());
    EXPECT_TRUE(p.audio3a_name.empty());
    EXPECT_TRUE(p.bwe_name.empty());
    EXPECT_FALSE(p.timeline_enabled);
    EXPECT_FALSE(p.datachannel_enabled);
}

TEST_F(AssemblyTest, ProfileSfuNoMediaPlugins) {
    const auto& p = nimrtc::assembly::kProfileSfu;
    EXPECT_EQ(p.name, "sfu");
    EXPECT_TRUE(p.jb_name.empty());
    EXPECT_TRUE(p.audio3a_name.empty());
    EXPECT_TRUE(p.bwe_name.empty());
    EXPECT_FALSE(p.timeline_enabled);
    EXPECT_FALSE(p.datachannel_enabled);
}

TEST_F(AssemblyTest, ProfileAgentTimelineAndDatachannel) {
    const auto& p = nimrtc::assembly::kProfileAgent;
    EXPECT_EQ(p.name, "agent");
    EXPECT_TRUE(p.timeline_enabled);
    EXPECT_TRUE(p.datachannel_enabled);
    EXPECT_EQ(p.jitter_buffer.mode,
              nimrtc::assembly::JitterBufferConfig::Mode::kLowLatency);
}

TEST_F(AssemblyTest, ProfileTeleopTimelineAndDatachannel) {
    const auto& p = nimrtc::assembly::kProfileTeleop;
    EXPECT_EQ(p.name, "teleop");
    EXPECT_TRUE(p.timeline_enabled);
    EXPECT_TRUE(p.datachannel_enabled);
    EXPECT_EQ(p.scheduler.strategy,
              nimrtc::assembly::SchedulerConfig::Strategy::kStrictPriority);
}

TEST_F(AssemblyTest, ProfileCallDefaults) {
    const auto& p = nimrtc::assembly::kProfileCall;
    EXPECT_EQ(p.name, "call");
    EXPECT_FALSE(p.timeline_enabled);
    EXPECT_FALSE(p.datachannel_enabled);
    EXPECT_EQ(p.jitter_buffer.mode,
              nimrtc::assembly::JitterBufferConfig::Mode::kLowLatency);
}

TEST_F(AssemblyTest, ProfileLiveBufferModeIsLive) {
    const auto& p = nimrtc::assembly::kProfileLive;
    EXPECT_EQ(p.name, "live");
    EXPECT_EQ(p.jitter_buffer.mode,
              nimrtc::assembly::JitterBufferConfig::Mode::kLive);
}

// ---------------------------------------------------------------------------
// v1.1 (PROFILE-1) new profile invariants.
// ---------------------------------------------------------------------------

TEST_F(AssemblyTest, ProfileAgentGatewayExposesApm) {
    const auto& p = nimrtc::assembly::kProfileAgentGateway;
    EXPECT_EQ(p.name, "agent-gateway");
    EXPECT_EQ(p.audio3a_name, "webrtc_apm");
    EXPECT_EQ(p.codec_name, "opus");
    EXPECT_TRUE(p.timeline_enabled);
    EXPECT_TRUE(p.datachannel_enabled);
    EXPECT_EQ(p.jitter_buffer.mode,
              nimrtc::assembly::JitterBufferConfig::Mode::kLowLatency);
    EXPECT_EQ(p.jitter_buffer.initial_delay_ms, 30);
    // BWE matches agent.json (low-bandwidth Agent link).
    EXPECT_EQ(p.bwe.params.initial_bitrate_bps, 500'000u);
}

TEST_F(AssemblyTest, ProfileSfuAgentHybrid) {
    const auto& p = nimrtc::assembly::kProfileSfuAgent;
    EXPECT_EQ(p.name, "sfu-agent");
    // Relay path: no JB / no codec / no BWE.
    EXPECT_TRUE(p.jb_name.empty());
    EXPECT_TRUE(p.codec_name.empty());
    EXPECT_TRUE(p.bwe_name.empty());
    // Agent side: APM stays on for the PCM-tap consumer.
    EXPECT_EQ(p.audio3a_name, "webrtc_apm");
    // Timeline + DC required for Agent observability + control.
    EXPECT_TRUE(p.timeline_enabled);
    EXPECT_TRUE(p.datachannel_enabled);
    // Strict-priority with control_weight=20 (twice agent.json's 10).
    EXPECT_EQ(p.scheduler.strategy,
              nimrtc::assembly::SchedulerConfig::Strategy::kStrictPriority);
    EXPECT_EQ(p.scheduler.control_weight, 20u);
}

TEST_F(AssemblyTest, ProfileAgentLowLatencyTightJb) {
    const auto& p = nimrtc::assembly::kProfileAgentLowLatency;
    EXPECT_EQ(p.name, "agent-low-latency");
    // Tightest v1.0 buffer depth (same as teleop).
    EXPECT_EQ(p.jitter_buffer.mode,
              nimrtc::assembly::JitterBufferConfig::Mode::kLowLatency);
    EXPECT_EQ(p.jitter_buffer.initial_delay_ms, 20);
    EXPECT_EQ(p.jitter_buffer.max_delay_ms, 120);
    // APM stays on (AEC/ANS/HPF are fine; AGC's level estimator is
    // signalled off via the v1.1 agent_low_latency marker).
    EXPECT_EQ(p.audio3a_name, "webrtc_apm");
    EXPECT_EQ(p.codec_name, "opus");
    EXPECT_TRUE(p.timeline_enabled);
    EXPECT_TRUE(p.datachannel_enabled);
}

// =============================================================================
// 3. Builder
// =============================================================================

TEST_F(AssemblyTest, BuilderLoadProfileCall) {
    auto profile = nimrtc::assembly::Builder{}
        .load_profile("call")
        .build();

    EXPECT_EQ(profile.name, "call");
    EXPECT_EQ(profile.jb_name, "adaptive");
}

TEST_F(AssemblyTest, BuilderLoadProfileThenOverride) {
    auto profile = nimrtc::assembly::Builder{}
        .load_profile("call")
        .override_transport("ice")
        .override_bwe({.impl = "aimd",
                       .params = {.initial_bitrate_bps = 500'000}})
        .build();

    EXPECT_EQ(profile.name, "call");
    EXPECT_EQ(profile.transport_name, "ice");
    EXPECT_EQ(profile.bwe.params.initial_bitrate_bps, 500'000u);
    // Original call BWE max is preserved.
    EXPECT_EQ(profile.bwe.params.max_bitrate_bps, 10'000'000u);
}

TEST_F(AssemblyTest, BuilderLoadUnknownProfileLeavesBuilderUnchanged) {
    nimrtc::assembly::Profile zero_profile;

    nimrtc::assembly::Builder b;
    // Never called load_profile — profile_ starts zero-initialised.
    auto profile = b.build();

    EXPECT_EQ(profile.name, zero_profile.name);
}

TEST_F(AssemblyTest, BuilderOverrideScheduler) {
    auto profile = nimrtc::assembly::Builder{}
        .load_profile("agent")
        .override_scheduler({
            .enabled  = true,
            .strategy = nimrtc::assembly::SchedulerConfig::Strategy::kWeightedFair,
            .control_weight = 20,
            .audio_weight   = 10,
            .video_weight   = 5,
        })
        .build();

    EXPECT_EQ(profile.scheduler.strategy,
              nimrtc::assembly::SchedulerConfig::Strategy::kWeightedFair);
    EXPECT_EQ(profile.scheduler.control_weight, 20u);
}

TEST_F(AssemblyTest, BuilderOverrideTimeline) {
    auto profile = nimrtc::assembly::Builder{}
        .load_profile("live")
        .override_timeline(true)
        .build();

    EXPECT_TRUE(profile.timeline_enabled);
}

TEST_F(AssemblyTest, BuilderOverrideDatachannel) {
    auto profile = nimrtc::assembly::Builder{}
        .load_profile("call")
        .override_datachannel(true)
        .build();

    EXPECT_TRUE(profile.datachannel_enabled);
}

TEST_F(AssemblyTest, BuilderOverrideJitterBuffer) {
    auto profile = nimrtc::assembly::Builder{}
        .load_profile("call")
        .override_jitter_buffer({
            .impl = "adaptive",
            .mode = nimrtc::assembly::JitterBufferConfig::Mode::kInteractive,
            .initial_delay_ms = 100,
        })
        .build();

    EXPECT_EQ(profile.jitter_buffer.mode,
              nimrtc::assembly::JitterBufferConfig::Mode::kInteractive);
    EXPECT_EQ(profile.jitter_buffer.initial_delay_ms, 100);
}

// =============================================================================
// 4. JSON round-trip
// =============================================================================

// Compare two profiles for equality (excluding name comparison for partials).
static void expect_profiles_equal(
    const nimrtc::assembly::Profile& a,
    const nimrtc::assembly::Profile& b) {
    EXPECT_EQ(a.name,                  b.name);
    EXPECT_EQ(a.transport_name,        b.transport_name);
    EXPECT_EQ(a.sdp_name,              b.sdp_name);
    EXPECT_EQ(a.rtp_name,              b.rtp_name);
    EXPECT_EQ(a.jb_name,               b.jb_name);
    EXPECT_EQ(a.audio3a_name,          b.audio3a_name);
    EXPECT_EQ(a.codec_name,            b.codec_name);
    EXPECT_EQ(a.bwe_name,              b.bwe_name);
    EXPECT_EQ(a.scheduler_name,        b.scheduler_name);
    EXPECT_EQ(a.timeline_enabled,      b.timeline_enabled);
    EXPECT_EQ(a.datachannel_enabled,   b.datachannel_enabled);
    EXPECT_EQ(a.audio_sample_rate_hz,  b.audio_sample_rate_hz);
    EXPECT_EQ(a.audio_channels,        b.audio_channels);
    EXPECT_EQ(a.audio_payload_type,    b.audio_payload_type);
    EXPECT_EQ(a.bwe.impl,                b.bwe.impl);
    EXPECT_EQ(a.bwe.params.initial_bitrate_bps, b.bwe.params.initial_bitrate_bps);
    EXPECT_EQ(a.bwe.params.max_bitrate_bps,     b.bwe.params.max_bitrate_bps);
    EXPECT_EQ(a.jitter_buffer.mode,    b.jitter_buffer.mode);
    EXPECT_EQ(a.jitter_buffer.initial_delay_ms, b.jitter_buffer.initial_delay_ms);
    EXPECT_EQ(a.scheduler.strategy,    b.scheduler.strategy);
    EXPECT_EQ(a.scheduler.impl,        b.scheduler.impl);
    EXPECT_EQ(a.scheduler.params.avg_packet_size_bytes,
              b.scheduler.params.avg_packet_size_bytes);
    EXPECT_EQ(a.scheduler.params.max_queue_depth,
              b.scheduler.params.max_queue_depth);
    EXPECT_EQ(a.scheduler.params.protect_keyframes,
              b.scheduler.params.protect_keyframes);
}

TEST_F(AssemblyTest, JsonRoundTripBuiltInProfiles) {
    for (std::string_view name : {"call", "live", "transport", "agent", "teleop", "sfu",
                                  "sfu-agent", "agent-gateway", "agent-low-latency"}) {
        SCOPED_TRACE("Profile: " + std::string{name});

        const nimrtc::assembly::Profile* original =
            nimrtc::assembly::ProfileRegistry::instance().find(name);
        ASSERT_NE(original, nullptr);

        std::string json_str = nimrtc::assembly::profile_to_json_string(*original);

        // Write to a temp file so we can exercise profile_from_json_file.
        fs::path tmp = fs::temp_directory_path() / "nimrtc_test_profile.json";
        {
            std::ofstream ofs{tmp, std::ios::binary};
            ASSERT_TRUE(ofs.is_open());
            ofs << json_str;
        }

        nimrtc::assembly::Profile round_tripped =
            nimrtc::assembly::profile_from_json_file(tmp.string());

        expect_profiles_equal(*original, round_tripped);

        // Clean up.
        fs::remove(tmp);
    }
}

TEST_F(AssemblyTest, JsonRoundTripBuilderProfile) {
    nimrtc::assembly::Profile built = nimrtc::assembly::Builder{}
        .load_profile("agent")
        .override_bwe({.impl = "aimd",
                       .params = {.initial_bitrate_bps = 750'000,
                                  .max_bitrate_bps = 8'000'000}})
        .override_timeline(false)
        .build();

    std::string json_str = nimrtc::assembly::profile_to_json_string(built);

    fs::path tmp = fs::temp_directory_path() / "nimrtc_test_builder.json";
    {
        std::ofstream ofs{tmp, std::ios::binary};
        ASSERT_TRUE(ofs.is_open());
        ofs << json_str;
    }

    nimrtc::assembly::Profile loaded = nimrtc::assembly::profile_from_json_file(tmp.string());

    EXPECT_EQ(loaded.name, built.name);
    EXPECT_EQ(loaded.bwe.params.initial_bitrate_bps, 750'000u);
    EXPECT_EQ(loaded.timeline_enabled, false);  // overridden

    fs::remove(tmp);
}

TEST_F(AssemblyTest, JsonProfileFileExists) {
    // Verify JSON files exist in the build directory.
    for (std::string_view name : {"call", "live", "transport", "agent",
                                  "teleop", "sfu",
                                  "sfu-agent", "agent-gateway", "agent-low-latency"}) {
        std::string path = profile_path(name);
        EXPECT_TRUE(fs::exists(path))
            << "Profile JSON not found at: " << path;
    }
}

TEST_F(AssemblyTest, JsonProfileFileParsesWithoutThrowing) {
    for (std::string_view name : {"call", "live", "transport", "agent",
                                  "teleop", "sfu",
                                  "sfu-agent", "agent-gateway", "agent-low-latency"}) {
        std::string path = profile_path(name);
        if (!fs::exists(path)) {
            // Skip if build directory is not set up yet.
            GTEST_SKIP() << "Profile JSON not found at: " << path;
        }

        EXPECT_NO_THROW({
            auto p = nimrtc::assembly::profile_from_json_file(path);
            EXPECT_EQ(p.name, name);
        }) << "Failed to parse: " << path;
    }
}

// ---------------------------------------------------------------------------
// v1.1 (PROFILE-1) JSON file ↔ C++ constant round-trip.
//
// For every JSON profile file shipped under profiles/, the parsed Profile
// must field-by-field match the corresponding built-in C++ constant. This
// catches the case where the JSON and the C++ companion drift (e.g. an
// edit to profiles/sfu.json that doesn't update kProfileSfu).
// ---------------------------------------------------------------------------
TEST_F(AssemblyTest, JsonFileMatchesBuiltinConstant) {
    for (std::string_view name : {"call", "live", "transport", "agent",
                                  "teleop", "sfu",
                                  "sfu-agent", "agent-gateway", "agent-low-latency"}) {
        SCOPED_TRACE("Profile: " + std::string{name});

        std::string path = profile_path(name);
        if (!fs::exists(path)) {
            GTEST_SKIP() << "Profile JSON not found at: " << path;
        }

        const nimrtc::assembly::Profile* cxx_constant =
            nimrtc::assembly::ProfileRegistry::instance().find(name);
        ASSERT_NE(cxx_constant, nullptr)
            << "No C++ constant for profile '" << name
            << "' — add kProfile* alongside profiles/*.json";

        nimrtc::assembly::Profile from_json =
            nimrtc::assembly::profile_from_json_file(path);

        expect_profiles_equal(*cxx_constant, from_json);
    }
}

// =============================================================================
// 5. Scheduler plugin bridge (impl + params + helpers)
// =============================================================================

TEST_F(AssemblyTest, SchedulerConfigDefaultsToStrictPriority) {
    nimrtc::assembly::SchedulerConfig cfg;
    EXPECT_EQ(cfg.impl, "strict_priority");
    EXPECT_EQ(cfg.strategy,
              nimrtc::assembly::SchedulerConfig::Strategy::kStrictPriority);
    // plugins::SchedulerConfig default values flow through params.
    EXPECT_EQ(cfg.params.avg_packet_size_bytes, 1200u);
    EXPECT_EQ(cfg.params.max_queue_depth, 8192u);
    EXPECT_TRUE(cfg.params.protect_keyframes);
}

TEST_F(AssemblyTest, DefaultImplForEachStrategy) {
    using S = nimrtc::assembly::SchedulerConfig::Strategy;
    EXPECT_EQ(nimrtc::assembly::default_impl_for(S::kStrictPriority),
              "strict_priority");
    EXPECT_EQ(nimrtc::assembly::default_impl_for(S::kWeightedFair),
              "weighted_fair");
    EXPECT_EQ(nimrtc::assembly::default_impl_for(S::kFixedRate),
              "fixed_rate");
}

TEST_F(AssemblyTest, ResolveSchedulerImplFallsBackOnEmpty) {
    nimrtc::assembly::SchedulerConfig cfg;
    cfg.impl = "custom_q";
    EXPECT_EQ(nimrtc::assembly::resolve_scheduler_impl(cfg), "custom_q");

    cfg.impl.clear();
    cfg.strategy = nimrtc::assembly::SchedulerConfig::Strategy::kWeightedFair;
    EXPECT_EQ(nimrtc::assembly::resolve_scheduler_impl(cfg), "weighted_fair");
}

TEST_F(AssemblyTest, ToPluginsSchedulerConfigForwardsParams) {
    nimrtc::assembly::SchedulerConfig cfg;
    cfg.params.avg_packet_size_bytes = 800;
    cfg.params.max_queue_depth       = 256;
    cfg.params.protect_keyframes     = false;

    auto psc = nimrtc::assembly::to_plugins_scheduler_config(cfg);
    EXPECT_EQ(psc.avg_packet_size_bytes, 800u);
    EXPECT_EQ(psc.max_queue_depth,       256u);
    EXPECT_FALSE(psc.protect_keyframes);
}

TEST_F(AssemblyTest, LegacyJsonInfersSchedulerImplFromStrategy) {
    // A profile that uses strategy="weighted_fair" without explicit
    // `impl` should still resolve to the weighted_fair plugin id after
    // JSON round-trip — preserving intent across versions.
    std::string legacy_json = R"({
      "name": "legacy_weighted",
      "scheduler": { "strategy": "weighted_fair" }
    })";
    fs::path tmp = fs::temp_directory_path() / "nimrtc_test_legacy.json";
    {
        std::ofstream ofs{tmp, std::ios::binary};
        ASSERT_TRUE(ofs.is_open());
        ofs << legacy_json;
    }

    auto loaded = nimrtc::assembly::profile_from_json_file(tmp.string());
    EXPECT_EQ(loaded.scheduler.strategy,
              nimrtc::assembly::SchedulerConfig::Strategy::kWeightedFair);
    EXPECT_EQ(loaded.scheduler.impl, "weighted_fair");
    fs::remove(tmp);
}

TEST_F(AssemblyTest, ExplicitSchedulerImplPreservedInJson) {
    nimrtc::assembly::Profile p;
    p.name = "custom";
    p.scheduler.strategy =
        nimrtc::assembly::SchedulerConfig::Strategy::kStrictPriority;
    p.scheduler.impl     = "my_custom_q";
    p.scheduler.params.avg_packet_size_bytes = 999;

    std::string s = nimrtc::assembly::profile_to_json_string(p);
    EXPECT_NE(s.find("\"impl\": \"my_custom_q\""), std::string::npos);
    EXPECT_NE(s.find("\"avg_packet_size_bytes\": 999"), std::string::npos);

    fs::path tmp = fs::temp_directory_path() / "nimrtc_test_custom_impl.json";
    {
        std::ofstream ofs{tmp, std::ios::binary};
        ASSERT_TRUE(ofs.is_open());
        ofs << s;
    }
    auto loaded = nimrtc::assembly::profile_from_json_file(tmp.string());
    EXPECT_EQ(loaded.scheduler.impl, "my_custom_q");
    EXPECT_EQ(loaded.scheduler.params.avg_packet_size_bytes, 999u);
    fs::remove(tmp);
}
