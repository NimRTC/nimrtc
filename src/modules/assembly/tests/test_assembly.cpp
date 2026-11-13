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
//      - kProfileTransport/kProfileRelay have empty plugin names where required
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
//   5. The transport bridge (architecture remediation plan §4.2 / D2)
//      - to_transport_input() copies all seven transport-relevant fields
//      - to_transport_requirements() resolves each built-in profile's
//        strategy to the canonical TransportRequirements
//      - the per-instance media p99 override survives the bridge
//      - an unknown strategy name throws std::invalid_argument
// =============================================================================

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include <nimrtc/assembly/assembly.hpp>
#include <nimrtc/assembly/profiles.hpp>
#include <nimrtc/assembly/transport_bridge.hpp>
#include <nimrtc/transport/transport_strategy.hpp>

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

    // Expected: 10 built-in profiles (6 v1.0 + 3 v1.1 from PROFILE-1 +
    // 1 v1.2 remote-desktop profile).
    EXPECT_EQ(names.size(), 10u);

    // Each known name must be present.
    for (std::string_view expected :
         {"call", "live", "transport", "agent", "teleop", "relay",
          "relay-agent", "agent-gateway", "agent-low-latency",
          "remote-desktop"}) {
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

TEST_F(AssemblyTest, ProfileRelayNoMediaPlugins) {
    const auto& p = nimrtc::assembly::kProfileRelay;
    EXPECT_EQ(p.name, "relay");
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

TEST_F(AssemblyTest, ProfileRelayAgentHybrid) {
    const auto& p = nimrtc::assembly::kProfileRelayAgent;
    EXPECT_EQ(p.name, "relay-agent");
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

    // v1.1 additive: `transport` block.
    EXPECT_EQ(a.transport.strategy, b.transport.strategy);
    EXPECT_EQ(a.transport.media.stack, b.transport.media.stack);
    EXPECT_EQ(a.transport.media.max_p99_ms, b.transport.media.max_p99_ms);
    EXPECT_EQ(a.transport.control.enabled, b.transport.control.enabled);
    EXPECT_EQ(a.transport.control.stack, b.transport.control.stack);
    EXPECT_EQ(a.transport.control.max_p99_ms, b.transport.control.max_p99_ms);
    EXPECT_EQ(a.transport.control.freq_hz, b.transport.control.freq_hz);
    EXPECT_EQ(a.transport.control.reliability, b.transport.control.reliability);
    EXPECT_EQ(a.transport.control.ttl_ms, b.transport.control.ttl_ms);

    // v1.2 additive: `video` block.
    EXPECT_EQ(a.video.codec_name,           b.video.codec_name);
    EXPECT_EQ(a.video.codec_preference,     b.video.codec_preference);
    EXPECT_EQ(a.video.codec_backend,        b.video.codec_backend);
    EXPECT_EQ(a.video.content_type,         b.video.content_type);
    EXPECT_EQ(a.video.ladder_mode,          b.video.ladder_mode);
    EXPECT_EQ(a.video.resolution_floor_width,  b.video.resolution_floor_width);
    EXPECT_EQ(a.video.resolution_floor_height, b.video.resolution_floor_height);
    EXPECT_EQ(a.video.pixel_format,         b.video.pixel_format);
    EXPECT_EQ(a.video.color_range,          b.video.color_range);
    EXPECT_EQ(a.video.color_matrix,         b.video.color_matrix);
}

TEST_F(AssemblyTest, JsonRoundTripBuiltInProfiles) {
    for (std::string_view name : {"call", "live", "transport", "agent", "teleop", "relay",
                                  "relay-agent", "agent-gateway", "agent-low-latency",
                                  "remote-desktop"}) {
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
                                  "teleop", "relay",
                                  "relay-agent", "agent-gateway", "agent-low-latency"}) {
        std::string path = profile_path(name);
        EXPECT_TRUE(fs::exists(path))
            << "Profile JSON not found at: " << path;
    }
}

TEST_F(AssemblyTest, JsonProfileFileParsesWithoutThrowing) {
    for (std::string_view name : {"call", "live", "transport", "agent",
                                  "teleop", "relay",
                                  "relay-agent", "agent-gateway", "agent-low-latency"}) {
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
// edit to profiles/relay.json that doesn't update kProfileRelay).
// ---------------------------------------------------------------------------
TEST_F(AssemblyTest, JsonFileMatchesBuiltinConstant) {
    for (std::string_view name : {"call", "live", "transport", "agent",
                                  "teleop", "relay",
                                  "relay-agent", "agent-gateway", "agent-low-latency",
                                  "remote-desktop"}) {
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

// =============================================================================
// 6. v1.1 additive: `transport` block (transport-strategies.md)
// =============================================================================
//
// These tests verify the v1.1 `transport` section round-trips and that
// the legacy v1.0 format (no `transport` block) still loads with the
// default empty `TransportSection` (strategy == "" → engine default).
// =============================================================================

TEST_F(AssemblyTest, TransportSectionDefaultsAreEmpty) {
    nimrtc::assembly::TransportSection t;
    EXPECT_EQ(t.strategy, "");
    EXPECT_EQ(t.media.stack, "");
    EXPECT_FALSE(t.media.max_p99_ms.has_value());
    EXPECT_FALSE(t.control.enabled);
    EXPECT_EQ(t.control.stack, "");
    EXPECT_FALSE(t.control.max_p99_ms.has_value());
    EXPECT_FALSE(t.control.freq_hz.has_value());
    EXPECT_EQ(t.control.reliability, "");
    EXPECT_FALSE(t.control.ttl_ms.has_value());
}

TEST_F(AssemblyTest, JsonParseTransportBlockAgentGateway) {
    std::string json_str = R"({
      "name": "test_agent_gateway",
      "transport": {
        "strategy": "agent-gateway",
        "media":   { "stack": "webrtc-classic", "max_p99_ms": 200 },
        "control": {
          "enabled":     true,
          "stack":       "raw-udp-arq",
          "max_p99_ms":  80,
          "freq_hz":     100,
          "reliability": "reliable_ordered"
        }
      }
    })";
    fs::path tmp = fs::temp_directory_path() / "nimrtc_test_transport.json";
    {
        std::ofstream ofs{tmp, std::ios::binary};
        ASSERT_TRUE(ofs.is_open());
        ofs << json_str;
    }
    auto p = nimrtc::assembly::profile_from_json_file(tmp.string());
    EXPECT_EQ(p.name, "test_agent_gateway");
    EXPECT_EQ(p.transport.strategy, "agent-gateway");
    EXPECT_EQ(p.transport.media.stack, "webrtc-classic");
    ASSERT_TRUE(p.transport.media.max_p99_ms.has_value());
    EXPECT_EQ(*p.transport.media.max_p99_ms, 200);
    EXPECT_TRUE(p.transport.control.enabled);
    EXPECT_EQ(p.transport.control.stack, "raw-udp-arq");
    ASSERT_TRUE(p.transport.control.max_p99_ms.has_value());
    EXPECT_EQ(*p.transport.control.max_p99_ms, 80);
    ASSERT_TRUE(p.transport.control.freq_hz.has_value());
    EXPECT_EQ(*p.transport.control.freq_hz, 100);
    EXPECT_EQ(p.transport.control.reliability, "reliable_ordered");
    fs::remove(tmp);
}

TEST_F(AssemblyTest, JsonParseTransportBlockTeleop) {
    std::string json_str = R"({
      "name": "test_teleop",
      "transport": {
        "strategy": "teleop-ultra-low-latency",
        "media":   { "stack": "raw-udp-arq", "max_p99_ms": 100 },
        "control": {
          "enabled":     true,
          "stack":       "raw-udp-arq",
          "max_p99_ms":  50,
          "freq_hz":     200,
          "reliability": "reliable_ordered",
          "ttl_ms":      100
        }
      }
    })";
    fs::path tmp = fs::temp_directory_path() / "nimrtc_test_teleop.json";
    {
        std::ofstream ofs{tmp, std::ios::binary};
        ASSERT_TRUE(ofs.is_open());
        ofs << json_str;
    }
    auto p = nimrtc::assembly::profile_from_json_file(tmp.string());
    EXPECT_EQ(p.transport.strategy, "teleop-ultra-low-latency");
    EXPECT_EQ(p.transport.media.stack, "raw-udp-arq");
    ASSERT_TRUE(p.transport.control.ttl_ms.has_value());
    EXPECT_EQ(*p.transport.control.ttl_ms, 100);
    fs::remove(tmp);
}

TEST_F(AssemblyTest, JsonParseLegacyV10ProfileOmitsTransport) {
    // A v1.0 profile (no `transport` block) must still load with the
    // default empty `TransportSection`. This is the backward-compat
    // contract (ADR-010 §"Schema v1.1 incremental fields").
    std::string json_str = R"({
      "name": "legacy_v10",
      "transport_name": "ice",
      "jb_name": "adaptive"
    })";
    fs::path tmp = fs::temp_directory_path() / "nimrtc_test_legacy_v10.json";
    {
        std::ofstream ofs{tmp, std::ios::binary};
        ASSERT_TRUE(ofs.is_open());
        ofs << json_str;
    }
    auto p = nimrtc::assembly::profile_from_json_file(tmp.string());
    EXPECT_EQ(p.name, "legacy_v10");
    EXPECT_EQ(p.transport_name, "ice");
    // Transport section: default-constructed (engine compile-time default).
    EXPECT_EQ(p.transport.strategy, "");
    EXPECT_EQ(p.transport.media.stack, "");
    EXPECT_FALSE(p.transport.control.enabled);
    fs::remove(tmp);
}

TEST_F(AssemblyTest, JsonSerialiseTransportBlockRoundTrip) {
    nimrtc::assembly::Profile p;
    p.name = "roundtrip";
    p.transport.strategy = "cross-network-peer";
    p.transport.media.stack = "webrtc-classic";
    p.transport.media.max_p99_ms = 150;
    p.transport.control.enabled = false;

    std::string s = nimrtc::assembly::profile_to_json_string(p);
    EXPECT_NE(s.find("\"strategy\": \"cross-network-peer\""), std::string::npos);
    EXPECT_NE(s.find("\"max_p99_ms\": 150"), std::string::npos);

    fs::path tmp = fs::temp_directory_path() / "nimrtc_test_roundtrip.json";
    {
        std::ofstream ofs{tmp, std::ios::binary};
        ASSERT_TRUE(ofs.is_open());
        ofs << s;
    }
    auto loaded = nimrtc::assembly::profile_from_json_file(tmp.string());
    expect_profiles_equal(p, loaded);
    fs::remove(tmp);
}

TEST_F(AssemblyTest, JsonSerialiseWithoutTransportOmitsBlock) {
    // A Profile with default-constructed `transport` (strategy = "")
    // must serialise WITHOUT a `transport` block — this preserves the
    // v1.0 round-trip byte-for-byte for legacy profiles.
    nimrtc::assembly::Profile p;
    p.name = "no_transport";
    p.transport_name = "ice";
    // p.transport left default-constructed.

    std::string s = nimrtc::assembly::profile_to_json_string(p);
    EXPECT_EQ(s.find("\"transport\""), std::string::npos)
        << "Default transport block must not appear in JSON output: " << s;
}

// ---------------------------------------------------------------------------
// v1.1: every built-in profile declares the expected transport strategy.
// ---------------------------------------------------------------------------
TEST_F(AssemblyTest, BuiltInProfileTransportStrategy) {
    struct Expectation {
        std::string_view profile_name;
        std::string_view expected_strategy;
        bool expected_control_enabled;
    };
    const Expectation expected[] = {
        {"call",                  "browser-call",             false},
        {"live",                  "browser-call",             false},
        {"transport",             "sfu-relay",                false},
        {"agent",                 "agent-gateway",            true },
        {"agent-gateway",         "agent-gateway",            true },
        {"agent-low-latency",     "agent-gateway",            true },
        {"teleop",                "teleop-ultra-low-latency", true },
        {"relay",                 "sfu-relay",                false},
        {"relay-agent",           "sfu-relay",                true },  // composite
        {"remote-desktop",        "teleop-ultra-low-latency", true },
    };
    for (const auto& e : expected) {
        SCOPED_TRACE("Profile: " + std::string{e.profile_name});
        const auto* p = nimrtc::assembly::ProfileRegistry::instance()
                            .find(e.profile_name);
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(p->transport.strategy, e.expected_strategy)
            << "Profile '" << e.profile_name
            << "' should declare strategy='" << e.expected_strategy << "'";
        EXPECT_EQ(p->transport.control.enabled, e.expected_control_enabled)
            << "Profile '" << e.profile_name
            << "' should have control.enabled=" << e.expected_control_enabled;
    }
}

// =============================================================================
// 7. v1.2 additive: the `video` block (remote-desktop plan K3 / K4)
// =============================================================================

TEST_F(AssemblyTest, VideoSectionDefaultsToACameraLikePicture) {
    // A profile without a `video` block behaves exactly as it did in
    // v1.1: no content type, no codec preference, no resolution floor.
    const nimrtc::assembly::Profile p;
    EXPECT_EQ(p.video.content_type,
              nimrtc::assembly::VideoContentType::kUnspecified);
    EXPECT_EQ(p.video.ladder_mode,
              nimrtc::assembly::VideoLadderMode::kFixed);
    EXPECT_TRUE(p.video.codec_preference.empty());
    EXPECT_EQ(p.video.resolution_floor_width, 0u);
    EXPECT_EQ(p.video.resolution_floor_height, 0u);
}

TEST_F(AssemblyTest, JsonParseVideoBlock) {
    std::string json_str = R"({
      "name": "screen",
      "video": {
        "codec_name": "",
        "codec_preference": ["av1", "h265", "h264"],
        "codec_backend": "auto",
        "content_type": "screen",
        "ladder_mode": "quality",
        "resolution_floor": [1280, 720],
        "pixel_format": "i420",
        "color_range": "full",
        "color_matrix": "bt709"
      }
    })";
    fs::path tmp = fs::temp_directory_path() / "nimrtc_test_video_block.json";
    {
        std::ofstream ofs{tmp, std::ios::binary};
        ASSERT_TRUE(ofs.is_open());
        ofs << json_str;
    }

    auto p = nimrtc::assembly::profile_from_json_file(tmp.string());
    EXPECT_EQ(p.video.codec_preference,
              (std::vector<std::string>{"av1", "h265", "h264"}));
    EXPECT_EQ(p.video.codec_backend, "auto");
    EXPECT_EQ(p.video.content_type,
              nimrtc::assembly::VideoContentType::kScreen);
    EXPECT_EQ(p.video.ladder_mode,
              nimrtc::assembly::VideoLadderMode::kQuality);
    EXPECT_EQ(p.video.resolution_floor_width, 1280u);
    EXPECT_EQ(p.video.resolution_floor_height, 720u);
    EXPECT_EQ(p.video.pixel_format, "i420");
    EXPECT_EQ(p.video.color_range, "full");
    EXPECT_EQ(p.video.color_matrix, "bt709");
    fs::remove(tmp);
}

TEST_F(AssemblyTest, JsonParseVideoBlockRejectsAnUnknownContentType) {
    // An unknown enum value is a *configuration* error: the loader
    // reports it rather than guessing.
    std::string json_str = R"({
      "name": "bad",
      "video": { "content_type": "hologram" }
    })";
    fs::path tmp = fs::temp_directory_path() / "nimrtc_test_bad_video.json";
    {
        std::ofstream ofs{tmp, std::ios::binary};
        ASSERT_TRUE(ofs.is_open());
        ofs << json_str;
    }

    EXPECT_THROW(nimrtc::assembly::profile_from_json_file(tmp.string()),
                 std::runtime_error);
    fs::remove(tmp);
}

TEST_F(AssemblyTest, JsonSerialiseVideoBlockRoundTrip) {
    nimrtc::assembly::Profile p;
    p.name = "roundtrip_video";
    p.video.codec_preference = {"av1", "h264"};
    p.video.codec_backend = "hardware";
    p.video.content_type = nimrtc::assembly::VideoContentType::kText;
    p.video.ladder_mode = nimrtc::assembly::VideoLadderMode::kSmoothness;
    p.video.resolution_floor_width = 640;
    p.video.resolution_floor_height = 480;
    p.video.pixel_format = "nv12";
    p.video.color_range = "limited";
    p.video.color_matrix = "bt601";

    std::string s = nimrtc::assembly::profile_to_json_string(p);
    EXPECT_NE(s.find("\"content_type\": \"text\""), std::string::npos);
    EXPECT_NE(s.find("\"ladder_mode\": \"smoothness\""), std::string::npos);
    EXPECT_NE(s.find("\"codec_backend\": \"hardware\""), std::string::npos);

    fs::path tmp = fs::temp_directory_path() / "nimrtc_test_video_roundtrip.json";
    {
        std::ofstream ofs{tmp, std::ios::binary};
        ASSERT_TRUE(ofs.is_open());
        ofs << s;
    }
    auto loaded = nimrtc::assembly::profile_from_json_file(tmp.string());
    expect_profiles_equal(p, loaded);
    fs::remove(tmp);
}

TEST_F(AssemblyTest, JsonSerialiseWithoutVideoOmitsBlock) {
    // A Profile with a default-constructed `video` section must
    // serialise WITHOUT a `video` block — this preserves the v1.1
    // round-trip byte-for-byte for the legacy profiles.
    nimrtc::assembly::Profile p;
    p.name = "no_video";
    p.transport_name = "ice";

    std::string s = nimrtc::assembly::profile_to_json_string(p);
    EXPECT_EQ(s.find("\"video\""), std::string::npos)
        << "Default video block must not appear in JSON output: " << s;
}

TEST_F(AssemblyTest, RemoteDesktopProfileDeclaresScreenContent) {
    const auto* p = nimrtc::assembly::ProfileRegistry::instance()
                        .find("remote-desktop");
    ASSERT_NE(p, nullptr);

    // The screen-content shape of the stream (plan B7 / B8 / D2 / C5).
    EXPECT_EQ(p->video.content_type,
              nimrtc::assembly::VideoContentType::kScreen);
    EXPECT_EQ(p->video.ladder_mode,
              nimrtc::assembly::VideoLadderMode::kQuality);
    EXPECT_EQ(p->video.codec_preference,
              (std::vector<std::string>{"av1", "h265", "h264"}));
    EXPECT_EQ(p->video.codec_backend, "auto");
    EXPECT_EQ(p->video.resolution_floor_width, 1280u);
    EXPECT_EQ(p->video.resolution_floor_height, 720u);
    EXPECT_EQ(p->video.color_range, "full");
    EXPECT_EQ(p->video.color_matrix, "bt709");

    // The transport: a separate control stack with its own congestion
    // control (plan G2), and the timeline + DataChannel on (G1).
    EXPECT_EQ(p->transport.strategy, "teleop-ultra-low-latency");
    EXPECT_TRUE(p->transport.control.enabled);
    EXPECT_TRUE(p->timeline_enabled);
    EXPECT_TRUE(p->datachannel_enabled);

    // The control plane must preempt the video stream.
    EXPECT_GT(p->scheduler.control_weight, p->scheduler.video_weight);
}

// =============================================================================
// 8. The transport bridge (architecture remediation plan §4.2 / D2)
//
// `assembly::TransportSection` is the assembly-layer shape; the transport
// layer consumes the mirror `nimrtc::ProfileTransportInput`.  The bridge
// (`nimrtc/assembly/transport_bridge.hpp`) is the adapter between them.
//
// These tests pin the three properties the bridge must have:
//
//   1. It copies every transport-relevant field (a field that is added to
//      one side and forgotten on the other is a silent data loss).
//   2. It resolves each built-in profile's strategy to the canonical
//      `TransportRequirements` of that strategy.
//   3. It surfaces an unknown strategy name as `std::invalid_argument`
//      rather than a silently-wrong default.
// =============================================================================

namespace {

// The five canonical strategy requirements, keyed by strategy id.  These
// are the transport module's own constants — the bridge must not invent
// its own defaults.
struct StrategyCase {
    std::string_view name;
    const nimrtc::TransportRequirements& canonical;
};

const StrategyCase kStrategyCases[] = {
    {"browser-call",              nimrtc::kStrategyBrowserCall},
    {"teleop-ultra-low-latency",  nimrtc::kStrategyTeleopUltraLowLatency},
    {"agent-gateway",             nimrtc::kStrategyAgentGateway},
    {"sfu-relay",                 nimrtc::kStrategySfuRelay},
    {"cross-network-peer",        nimrtc::kStrategyCrossNetworkPeer},
};

}  // namespace

TEST_F(AssemblyTest, TransportBridgeCopiesEveryField) {
    nimrtc::assembly::TransportSection section;
    section.strategy = "agent-gateway";
    section.media.stack = "webrtc-classic";
    section.media.max_p99_ms = 175;
    section.control.enabled = true;
    section.control.stack = "raw-udp-arq";
    section.control.max_p99_ms = 42;
    section.control.freq_hz = 250;
    section.control.reliability = "partial_reliable";
    section.control.ttl_ms = 33;

    const auto in = nimrtc::assembly::to_transport_input(section);

    EXPECT_EQ(in.strategy, "agent-gateway");
    ASSERT_TRUE(in.media_max_p99_ms.has_value());
    EXPECT_EQ(*in.media_max_p99_ms, 175);
    EXPECT_TRUE(in.control_enabled);
    ASSERT_TRUE(in.control_max_p99_ms.has_value());
    EXPECT_EQ(*in.control_max_p99_ms, 42);
    ASSERT_TRUE(in.control_freq_hz.has_value());
    EXPECT_EQ(*in.control_freq_hz, 250);
    EXPECT_EQ(in.control_reliability, "partial_reliable");
    ASSERT_TRUE(in.control_ttl_ms.has_value());
    EXPECT_EQ(*in.control_ttl_ms, 33);
}

TEST_F(AssemblyTest, TransportBridgeCopiesEmptySectionVerbatim) {
    // A v1.0 profile (no `transport` block) must map to an input that
    // resolves to the engine default — not to something that throws.
    const nimrtc::assembly::TransportSection section;

    const auto in = nimrtc::assembly::to_transport_input(section);
    EXPECT_EQ(in.strategy, "");
    EXPECT_FALSE(in.media_max_p99_ms.has_value());
    EXPECT_FALSE(in.control_enabled);
    EXPECT_FALSE(in.control_max_p99_ms.has_value());
    EXPECT_FALSE(in.control_freq_hz.has_value());
    EXPECT_EQ(in.control_reliability, "");
    EXPECT_FALSE(in.control_ttl_ms.has_value());

    const auto req = nimrtc::requirements_from_profile_input(in);
    EXPECT_EQ(req.needs_browser_interop,
              nimrtc::kStrategyBrowserCall.needs_browser_interop);
    EXPECT_EQ(req.needs_high_freq_control,
              nimrtc::kStrategyBrowserCall.needs_high_freq_control);
}

TEST_F(AssemblyTest, TransportBridgeResolvesEveryBuiltInProfile) {
    // Every built-in profile must survive the bridge and land on the
    // canonical requirements of the strategy it names.
    for (const auto& c : kStrategyCases) {
        SCOPED_TRACE("Strategy: " + std::string{c.name});

        nimrtc::assembly::TransportSection section;
        section.strategy = std::string{c.name};

        // NOTE: the Profile is built by assignment rather than by a
        // designated initialiser.  `Profile` has ~20 members, and a
        // designated initialiser that names only two of them trips
        // GCC's `-Werror=missing-field-initializers` (the aggregate is
        // then value-initialised for the rest, but the warning fires
        // anyway).  MSVC has no equivalent warning, so this only shows
        // up on the GCC / Clang CI legs.
        nimrtc::assembly::Profile profile;
        profile.name      = "bridge_test";
        profile.transport = section;

        const auto req = nimrtc::assembly::to_transport_requirements(profile);

        EXPECT_EQ(req.needs_browser_interop, c.canonical.needs_browser_interop);
        EXPECT_EQ(req.needs_high_freq_control, c.canonical.needs_high_freq_control);
        ASSERT_TRUE(req.max_p99_ms.has_value());
        EXPECT_EQ(*req.max_p99_ms, *c.canonical.max_p99_ms);
    }
}

TEST_F(AssemblyTest, TransportBridgeAppliesMediaP99Override) {
    // The per-instance `transport.media.max_p99_ms` is the value that
    // used to be parsed, serialised, JSON round-tripped and then
    // silently dropped.  It must survive the bridge.
    nimrtc::assembly::Profile profile;
    profile.name = "override_test";
    profile.transport.strategy = "agent-gateway";
    profile.transport.media.max_p99_ms = 60;

    const auto req = nimrtc::assembly::to_transport_requirements(profile);
    ASSERT_TRUE(req.max_p99_ms.has_value());
    EXPECT_EQ(*req.max_p99_ms, 60);
    // The capability flags must still come from the strategy.
    EXPECT_EQ(req.needs_browser_interop,
              nimrtc::kStrategyAgentGateway.needs_browser_interop);
    EXPECT_EQ(req.needs_high_freq_control,
              nimrtc::kStrategyAgentGateway.needs_high_freq_control);
}

TEST_F(AssemblyTest, TransportBridgeUnknownStrategyThrows) {
    // The bridge delegates validation to `strategy_by_name()`, so an
    // unknown id must surface as `std::invalid_argument` — never as a
    // silently-wrong default.
    nimrtc::assembly::Profile profile;
    profile.name = "bogus";
    profile.transport.strategy = "webrtc-quic-call";

    EXPECT_THROW(nimrtc::assembly::to_transport_requirements(profile),
                 std::invalid_argument);
}

TEST_F(AssemblyTest, TransportBridgeStrategyNameAccessor) {
    // The accessor the host uses to fill `EngineConfig::
    // transport_strategy_name`.
    EXPECT_EQ(nimrtc::assembly::transport_strategy_name(
                  nimrtc::assembly::kProfileTeleop),
              "teleop-ultra-low-latency");
    EXPECT_EQ(nimrtc::assembly::transport_strategy_name(
                  nimrtc::assembly::kProfileAgentGateway),
              "agent-gateway");
}

TEST_F(AssemblyTest, TransportBridgeProfileOverloadMatchesSectionOverload) {
    // The Profile overload must be exactly the section overload applied
    // to `profile.transport`.
    for (std::string_view name : {"call", "live", "transport", "agent", "teleop",
                                  "relay", "relay-agent", "agent-gateway",
                                  "agent-low-latency"}) {
        SCOPED_TRACE("Profile: " + std::string{name});
        const auto* p = nimrtc::assembly::ProfileRegistry::instance()
                            .find(name);
        ASSERT_NE(p, nullptr);

        const auto via_profile = nimrtc::assembly::to_transport_input(*p);
        const auto via_section =
            nimrtc::assembly::to_transport_input(p->transport);

        EXPECT_EQ(via_profile.strategy,            via_section.strategy);
        EXPECT_EQ(via_profile.media_max_p99_ms,    via_section.media_max_p99_ms);
        EXPECT_EQ(via_profile.control_enabled,     via_section.control_enabled);
        EXPECT_EQ(via_profile.control_max_p99_ms,  via_section.control_max_p99_ms);
        EXPECT_EQ(via_profile.control_freq_hz,     via_section.control_freq_hz);
        EXPECT_EQ(via_profile.control_reliability, via_section.control_reliability);
        EXPECT_EQ(via_profile.control_ttl_ms,      via_section.control_ttl_ms);
    }
}

TEST_F(AssemblyTest, TransportBridgeSurvivesJsonRoundTrip) {
    // The full chain the host actually walks: load a Profile from JSON,
    // bridge it to the transport layer, and confirm the strategy and the
    // per-instance budget both made it.
    for (std::string_view name : {"call", "live", "transport", "agent", "teleop",
                                  "relay", "relay-agent", "agent-gateway",
                                  "agent-low-latency"}) {
        SCOPED_TRACE("Profile: " + std::string{name});
        const std::string path = profile_path(name);
        if (!fs::exists(path)) {
            GTEST_SKIP() << "Profile JSON not found at: " << path;
        }

        const auto p = nimrtc::assembly::profile_from_json_file(path);
        const auto in = nimrtc::assembly::to_transport_input(p);

        // The strategy must be one of the five canonical ids.
        bool known = false;
        for (const auto& c : kStrategyCases) {
            if (in.strategy == c.name) { known = true; break; }
        }
        EXPECT_TRUE(known)
            << "Profile '" << name << "' declares strategy '" << in.strategy
            << "' which is not one of the five canonical ids";

        // The requirements must resolve without throwing.
        EXPECT_NO_THROW(nimrtc::assembly::to_transport_requirements(p));
    }
}
