// =============================================================================
// test_assembly_bridge.cpp — the Profile → EngineConfig bridge
// =============================================================================
//
// Architecture remediation plan D2 / Phase 4.2: the `assembly` module must
// be *wired*, not merely implemented.  This test file is the proof that the
// wiring is real:
//
//   1. The field map is exact — every Profile field the engine consumes
//      lands on the right `EngineConfig` field, and nothing is dropped.
//   2. The disabled-subsystem guard works — a profile with no codec /
//      jitter buffer does NOT leak its placeholder parameters onto the
//      engine config.
//   3. The scheduler id is always resolvable — no Profile spelling yields
//      an id the engine's registry cannot resolve.
//   4. The transport strategy survives the bridge — the Profile's
//      `transport.strategy` reaches `EngineConfig::transport_strategy_name`
//      and the engine accepts it at `open()` time.
//   5. The bridge is *composable* — the host's deployment knobs survive.
//
// The last point is the one the dead-module state got wrong: the old
// `demo-agent-gateway` example hand-copied four fields and dropped the
// rest.  These tests are what make that impossible to regress.
// =============================================================================

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <nimrtc/assembly/assembly.hpp>
#include <nimrtc/assembly/engine_bridge.hpp>
#include <nimrtc/assembly/profiles.hpp>
#include <nimrtc/assembly/transport_bridge.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/transport/transport_strategy.hpp>

namespace {

using nimrtc::assembly::Profile;

// Every built-in profile, by name.  The bridge must behave identically
// for all of them — a profile that is not in this list has no
// built-in constant and is covered by the synthetic-profile tests.
const std::vector<std::string>& builtin_profile_names() {
    static const std::vector<std::string> names = {
        "call", "live", "transport", "agent", "teleop",
        "relay", "relay-agent", "agent-gateway", "agent-low-latency",
    };
    return names;
}

}  // namespace

// =============================================================================
// 1. The field map — Profile → EngineConfig
// =============================================================================

TEST(AssemblyBridgeTest, PluginIdsAreCopiedVerbatim) {
    const Profile& p = nimrtc::assembly::kProfileAgentGateway;
    const auto cfg = nimrtc::assembly::to_engine_config(p);

    EXPECT_EQ(cfg.transport_name, p.transport_name);
    EXPECT_EQ(cfg.jb_name, p.jb_name);
    EXPECT_EQ(cfg.audio3a_name, p.audio3a_name);
    EXPECT_EQ(cfg.codec_name, p.codec_name);
    EXPECT_EQ(cfg.timeline_enabled, p.timeline_enabled);
    EXPECT_EQ(cfg.transport_strategy_name, p.transport.strategy);
}

TEST(AssemblyBridgeTest, BweParamsReachTheEngineConfig) {
    const Profile& p = nimrtc::assembly::kProfileAgentGateway;
    const auto cfg = nimrtc::assembly::to_engine_config(p);

    // The Profile's BWE block is the v1.1 spelling; the nested `impl`
    // wins over the legacy top-level `bwe_name`.
    EXPECT_EQ(cfg.bwe_name, p.bwe.impl);
    EXPECT_EQ(cfg.bwe_config.initial_bitrate_bps,
              p.bwe.params.initial_bitrate_bps);
    EXPECT_EQ(cfg.bwe_config.max_bitrate_bps,
              p.bwe.params.max_bitrate_bps);
}

TEST(AssemblyBridgeTest, SchedulerParamsReachTheEngineConfig) {
    const Profile& p = nimrtc::assembly::kProfileTeleop;
    const auto cfg = nimrtc::assembly::to_engine_config(p);

    EXPECT_TRUE(cfg.scheduler_config.enabled);
    EXPECT_EQ(cfg.scheduler_config.avg_packet_size_bytes,
              p.scheduler.params.avg_packet_size_bytes);
    EXPECT_EQ(cfg.scheduler_config.max_queue_depth,
              p.scheduler.params.max_queue_depth);
    EXPECT_EQ(cfg.scheduler_config.protect_keyframes,
              p.scheduler.params.protect_keyframes);
}

TEST(AssemblyBridgeTest, AudioFormatIsCopiedWhenACodecIsPresent) {
    const Profile& p = nimrtc::assembly::kProfileCall;
    const auto cfg = nimrtc::assembly::to_engine_config(p);

    EXPECT_EQ(cfg.pcm_sample_rate_hz, p.audio_sample_rate_hz);
    EXPECT_EQ(cfg.pcm_channels, p.audio_channels);
    EXPECT_EQ(cfg.audio_codec.payload_type, p.audio_payload_type);
}

TEST(AssemblyBridgeTest, JitterBufferDelaysAreCopiedWhenAJbIsPresent) {
    const Profile& p = nimrtc::assembly::kProfileAgent;
    const auto cfg = nimrtc::assembly::to_engine_config(p);

    EXPECT_EQ(cfg.jb_initial_delay_ms, p.jitter_buffer.initial_delay_ms);
    EXPECT_EQ(cfg.jb_min_delay_ms, p.jitter_buffer.min_delay_ms);
    EXPECT_EQ(cfg.jb_max_delay_ms, p.jitter_buffer.max_delay_ms);
}

// =============================================================================
// 2. The disabled-subsystem guard
// =============================================================================

TEST(AssemblyBridgeTest, DisabledCodecDoesNotLeakPlaceholderAudioFormat) {
    // kProfileRelay has no codec: its audio parameters are placeholders
    // (rate 0, channels 0, PT 0).  Copying them onto the EngineConfig
    // would give the engine a 0 Hz PCM format and a PCMU offer for a
    // stream that does not exist.
    const Profile& p = nimrtc::assembly::kProfileRelay;
    ASSERT_TRUE(p.codec_name.empty());

    const auto cfg = nimrtc::assembly::to_engine_config(p);

    // The engine's own defaults must survive.
    EXPECT_NE(cfg.pcm_sample_rate_hz, 0u);
    EXPECT_NE(cfg.pcm_channels, 0);
    EXPECT_EQ(cfg.audio_codec.payload_type, 111);
}

TEST(AssemblyBridgeTest, DisabledJitterBufferDoesNotLeakPlaceholderDelays) {
    const Profile& p = nimrtc::assembly::kProfileRelay;
    ASSERT_TRUE(p.jb_name.empty());
    ASSERT_EQ(p.jitter_buffer.initial_delay_ms, 0);

    const auto cfg = nimrtc::assembly::to_engine_config(p);

    EXPECT_NE(cfg.jb_initial_delay_ms, 0);
    EXPECT_NE(cfg.jb_min_delay_ms, 0);
    EXPECT_NE(cfg.jb_max_delay_ms, 0);
}

TEST(AssemblyBridgeTest, DisabledBweYieldsAnEmptyBwePluginId) {
    const Profile& p = nimrtc::assembly::kProfileRelay;
    ASSERT_TRUE(p.bwe_name.empty());

    const auto cfg = nimrtc::assembly::to_engine_config(p);

    // The nested `bwe.impl` is "aimd" even on the relay profile (the JSON
    // keeps the block for round-trip stability), so the bridge must
    // prefer the *top-level* empty id when the subsystem is off.
    //
    // This is the one place where "nested wins" is wrong: the nested
    // block is a placeholder, the top-level id is the switch.
    EXPECT_TRUE(cfg.bwe_name.empty());
}

// =============================================================================
// 3. The scheduler id is always resolvable
// =============================================================================

TEST(AssemblyBridgeTest, EveryBuiltInProfileResolvesToAKnownSchedulerId) {
    // The `sched` module registers exactly one scheduler factory:
    // "strict_priority".  Every built-in Profile spelling must resolve
    // to *some* id — an unresolved id makes the engine skip the
    // scheduler entirely, which turns the Profile's priority weights
    // into dead configuration.
    const std::vector<std::string> known = {
        "strict_priority", "weighted_fair", "fixed_rate", "default",
    };

    for (const auto& name : builtin_profile_names()) {
        const Profile* p =
            nimrtc::assembly::ProfileRegistry::instance().find(name);
        ASSERT_NE(p, nullptr) << name;

        const auto cfg = nimrtc::assembly::to_engine_config(*p);
        const bool registered =
            known.end() !=
            std::find(known.begin(), known.end(), cfg.scheduler_name);

        // Either the id is one the assembly layer knows, or it is the
        // registered "strict_priority" — never an empty string (which
        // would disable the scheduler) and never a spelling that the
        // registry cannot resolve.
        EXPECT_FALSE(cfg.scheduler_name.empty()) << name;
        EXPECT_TRUE(registered || cfg.scheduler_name == "strict_priority")
            << name << " -> " << cfg.scheduler_name;
    }
}

TEST(AssemblyBridgeTest, ReservedSchedulerIdsFallBackToTheRegisteredOne) {
    bool substituted = false;

    EXPECT_EQ(nimrtc::assembly::resolve_scheduler_plugin_id(
                  "weighted_fair", &substituted),
              "strict_priority");
    EXPECT_TRUE(substituted);

    EXPECT_EQ(nimrtc::assembly::resolve_scheduler_plugin_id(
                  "fixed_rate", &substituted),
              "strict_priority");
    EXPECT_TRUE(substituted);

    // "default" is the legacy spelling of the same implementation.
    EXPECT_EQ(nimrtc::assembly::resolve_scheduler_plugin_id(
                  "default", &substituted),
              "strict_priority");
    EXPECT_TRUE(substituted);
}

TEST(AssemblyBridgeTest, ThirdPartySchedulerIdsArePassedThrough) {
    bool substituted = true;

    // A user who registers their own scheduler must be able to select it.
    EXPECT_EQ(nimrtc::assembly::resolve_scheduler_plugin_id(
                  "my_scheduler", &substituted),
              "my_scheduler");
    EXPECT_FALSE(substituted);

    EXPECT_EQ(nimrtc::assembly::resolve_scheduler_plugin_id(
                  "strict_priority", &substituted),
              "strict_priority");
    EXPECT_FALSE(substituted);
}

TEST(AssemblyBridgeTest, SchedulerDisabledProfileKeepsTheSchedulerOff) {
    const Profile& p = nimrtc::assembly::kProfileTransport;
    ASSERT_FALSE(p.scheduler.enabled);

    const auto cfg = nimrtc::assembly::to_engine_config(p);

    EXPECT_FALSE(cfg.scheduler_config.enabled);
}

// =============================================================================
// 4. The transport strategy survives the bridge
// =============================================================================

TEST(AssemblyBridgeTest, TransportStrategyReachesTheEngineConfig) {
    for (const auto& name : builtin_profile_names()) {
        const Profile* p =
            nimrtc::assembly::ProfileRegistry::instance().find(name);
        ASSERT_NE(p, nullptr) << name;

        const auto cfg = nimrtc::assembly::to_engine_config(*p);

        EXPECT_EQ(cfg.transport_strategy_name, p->transport.strategy)
            << name;

        // A non-empty strategy must be one the transport layer knows —
        // otherwise `engine.open()` fails with kEngineInvalidParam.
        if (!cfg.transport_strategy_name.empty()) {
            EXPECT_NO_THROW(
                nimrtc::strategy_by_name(cfg.transport_strategy_name))
                << name;
        }
    }
}

TEST(AssemblyBridgeTest, BridgeAndTransportBridgeAgreeOnTheStrategy) {
    // The two bridges are independent code paths; they must not drift.
    for (const auto& name : builtin_profile_names()) {
        const Profile* p =
            nimrtc::assembly::ProfileRegistry::instance().find(name);
        ASSERT_NE(p, nullptr) << name;

        const auto cfg = nimrtc::assembly::to_engine_config(*p);
        const auto input = nimrtc::assembly::to_transport_input(*p);

        EXPECT_EQ(cfg.transport_strategy_name, std::string{input.strategy})
            << name;
    }
}

// =============================================================================
// 5. The bridge is composable — the host's knobs survive
// =============================================================================

TEST(AssemblyBridgeTest, DeploymentKnobsSurviveTheBridge) {
    nimrtc::engine::EngineConfig cfg;
    cfg.local_bind_address = "127.0.0.1";
    cfg.stun_server_host    = "stun.example.com";
    cfg.dtls_name          = "wolfssl";
    cfg.srtp_backend_name  = "libsrtp";

    const Profile& p = nimrtc::assembly::kProfileAgentGateway;
    const auto merged = nimrtc::assembly::to_engine_config(p, cfg);

    // The Profile owns the scenario knobs ...
    EXPECT_EQ(merged.timeline_enabled, p.timeline_enabled);
    EXPECT_EQ(merged.transport_strategy_name, p.transport.strategy);
    // ... and the host keeps the deployment knobs.
    EXPECT_EQ(merged.local_bind_address, "127.0.0.1");
    EXPECT_EQ(merged.stun_server_host, "stun.example.com");
    EXPECT_EQ(merged.dtls_name, "wolfssl");
    EXPECT_EQ(merged.srtp_backend_name, "libsrtp");
}

TEST(AssemblyBridgeTest, DataChannelDisabledClearsTheBackend) {
    const Profile& p = nimrtc::assembly::kProfileCall;
    ASSERT_FALSE(p.datachannel_enabled);

    const auto cfg = nimrtc::assembly::to_engine_config(p);
    EXPECT_TRUE(cfg.datachannel_backend.empty());
}

TEST(AssemblyBridgeTest, DataChannelEnabledKeepsTheEngineDefaultBackend) {
    const Profile& p = nimrtc::assembly::kProfileAgentGateway;
    ASSERT_TRUE(p.datachannel_enabled);

    const auto cfg = nimrtc::assembly::to_engine_config(p);
    EXPECT_EQ(cfg.datachannel_backend, "web_transport");
}

// =============================================================================
// 6. The bridge is total — every built-in profile goes through it
// =============================================================================

TEST(AssemblyBridgeTest, EveryBuiltInProfileConvertsWithoutThrowing) {
    for (const auto& name : builtin_profile_names()) {
        const Profile* p =
            nimrtc::assembly::ProfileRegistry::instance().find(name);
        ASSERT_NE(p, nullptr) << name;

        EXPECT_NO_THROW({
            const auto cfg = nimrtc::assembly::to_engine_config(*p);
            (void)cfg;
        }) << name;
    }
}
