/**
 * @file tests/test_engine_transport_profile.cpp
 * @brief The full Profile → transport-strategy chain, end to end
 *        (architecture remediation plan §4.2 / D2 — `assembly` disposition).
 *
 * ## Why this test exists
 *
 * The `assembly` module was, until this change, *referenced* by the
 * transport layer but never *consumed* by it:
 *
 *   - `assembly::Profile` carried a `transport` section with a strategy
 *     id and a per-instance media p99 budget;
 *   - the transport layer had a mirror (`nimrtc::ProfileTransportInput`)
 *     and a resolver (`requirements_from_profile_input()`);
 *   - **nothing connected the two**, and the per-instance budget was
 *     parsed, JSON round-tripped and then silently dropped.
 *
 * The wiring now is:
 *
 *   Profile.transport  ──(assembly::to_transport_input)──▶  ProfileTransportInput
 *            │
 *            └──(assembly::transport_strategy_name)──▶  EngineConfig::transport_strategy_name
 *            └──(media.max_p99_ms)───────────────────▶  EngineConfig::transport_media_max_p99_ms
 *                                                            │
 *                                                            ▼
 *                                       transport::requirements_from_profile_input()
 *                                                            │
 *                                                            ▼
 *                                       TransportRequirements::max_p99_ms
 *
 * This test walks that whole chain and asserts the value survives every
 * hop.  The unit-level pieces live next to their owners:
 *
 *   - `src/modules/assembly/tests/test_assembly.cpp` — the bridge itself
 *     (field-by-field copy, canonical resolution, unknown-name throw).
 *   - `src/modules/transport/tests/test_transport_strategy.cpp` — the
 *     transport-side resolver.
 *
 * This file is the *integration* test: it is the only place where the
 * assembly module, the transport module and the engine meet.
 *
 * ## Network note
 *
 * The engine opens a real ICE transport here, exactly like the existing
 * `EnginePluginLoading` strategy tests do.  No peer is contacted — the
 * test only inspects the *resolved* strategy state that `open()`
 * records.
 */

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>

#include <nimrtc/assembly/engine_bridge.hpp>
#include <nimrtc/assembly/profiles.hpp>
#include <nimrtc/assembly/transport_bridge.hpp>
#include <nimrtc/engine/engine.hpp>

namespace {

using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

/** Fill an EngineConfig from a Profile's transport section.
 *
 *  This is the hand-off the host application performs.  It is written
 *  out here (rather than hidden behind a helper in the library) because
 *  it is the *contract* the D2 change establishes: the Profile's
 *  transport section reaches the engine through exactly these two
 *  EngineConfig fields. */
void apply_transport_profile(const nimrtc::assembly::Profile& profile,
                              EngineConfig& cfg) {
    const auto in = nimrtc::assembly::to_transport_input(profile);
    cfg.transport_strategy_name    = std::string{in.strategy};
    cfg.transport_media_max_p99_ms = in.media_max_p99_ms;
}

/** The same hand-off, but through the *published* authority.
 *
 *  `assembly::to_engine_config()` is what a real host calls — it is the
 *  single authority for the Profile → EngineConfig mapping.  The two
 *  must agree, otherwise a host using one would get a different engine
 *  than a host using the other. */
EngineConfig via_authority(const nimrtc::assembly::Profile& profile) {
    return nimrtc::assembly::to_engine_config(profile);
}

class EngineTransportProfile : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
    }
};

// ---------------------------------------------------------------------------
// 1. The strategy name reaches the engine
// ---------------------------------------------------------------------------

TEST_F(EngineTransportProfile, profile_strategy_reaches_the_engine) {
    const auto& profile = nimrtc::assembly::kProfileAgentGateway;

    EngineConfig cfg;
    apply_transport_profile(profile, cfg);
    ASSERT_EQ(cfg.transport_strategy_name, "agent-gateway");

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    EXPECT_EQ(engine.transport_strategy_name(), "agent-gateway");
    engine.close();
}

TEST_F(EngineTransportProfile, teleop_profile_selects_the_raw_udp_stack) {
    // §5.3 rule 2: no browser interop + high-frequency control →
    // media=raw-udp-arq, control=raw-udp-arq.  The Profile's strategy
    // id is what drives this; nothing else in the config mentions the
    // stack ids.
    const auto& profile = nimrtc::assembly::kProfileTeleop;

    EngineConfig cfg;
    apply_transport_profile(profile, cfg);

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    const auto session = engine.transport_strategy_session();
    EXPECT_EQ(session.media_factory_id, "raw-udp-arq");
    EXPECT_EQ(session.control_factory_id, "raw-udp-arq");
    engine.close();
}

TEST_F(EngineTransportProfile, browser_call_profile_has_no_control_stack) {
    // The mirror image of the teleop case: the Profile's strategy id
    // alone decides whether a control side-stack exists.
    const auto& profile = nimrtc::assembly::kProfileCall;

    EngineConfig cfg;
    apply_transport_profile(profile, cfg);

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    const auto session = engine.transport_strategy_session();
    EXPECT_EQ(session.media_factory_id, "webrtc-classic");
    EXPECT_EQ(session.control_factory_id, "")
        << "browser-call must NOT enable a control side-stack";
    engine.close();
}

// ---------------------------------------------------------------------------
// 2. The per-instance media p99 budget reaches the engine
// ---------------------------------------------------------------------------

TEST_F(EngineTransportProfile, strategy_default_budget_is_recorded) {
    // "agent-gateway" declares a 100 ms budget in the transport
    // strategy catalog.  The Profile does not override it, so the
    // engine must record the strategy's default.
    EngineConfig cfg;
    cfg.transport_strategy_name = "agent-gateway";

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_TRUE(engine.transport_media_max_p99_ms().has_value());
    EXPECT_EQ(*engine.transport_media_max_p99_ms(), 100);
    engine.close();
}

TEST_F(EngineTransportProfile, per_instance_p99_override_is_applied) {
    // THE regression this change fixes: the per-instance budget used to
    // be parsed and then dropped on the floor.
    EngineConfig cfg;
    cfg.transport_strategy_name     = "agent-gateway";
    cfg.transport_media_max_p99_ms  = 60;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_TRUE(engine.transport_media_max_p99_ms().has_value());
    EXPECT_EQ(*engine.transport_media_max_p99_ms(), 60)
        << "the per-instance override must win over the strategy default";
    engine.close();
}

TEST_F(EngineTransportProfile, override_may_also_loosen_the_budget) {
    // The override is not restricted to tightening: "browser-call" with
    // a 300 ms budget is the `live` profile's shape.
    EngineConfig cfg;
    cfg.transport_strategy_name     = "browser-call";
    cfg.transport_media_max_p99_ms  = 300;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_TRUE(engine.transport_media_max_p99_ms().has_value());
    EXPECT_EQ(*engine.transport_media_max_p99_ms(), 300);
    engine.close();
}

TEST_F(EngineTransportProfile, profile_p99_override_survives_the_whole_chain) {
    // The full chain: Profile → bridge → EngineConfig → engine.
    auto profile = nimrtc::assembly::kProfileAgentGateway;
    profile.transport.media.max_p99_ms = 55;

    EngineConfig cfg;
    apply_transport_profile(profile, cfg);
    ASSERT_EQ(cfg.transport_media_max_p99_ms.has_value(), true);
    EXPECT_EQ(*cfg.transport_media_max_p99_ms, 55);

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_TRUE(engine.transport_media_max_p99_ms().has_value());
    EXPECT_EQ(*engine.transport_media_max_p99_ms(), 55);
    engine.close();
}

TEST_F(EngineTransportProfile, published_authority_agrees_with_the_bridge) {
    // `to_engine_config()` is the published authority.  It must fill
    // exactly the two transport fields the bridge fills — a host that
    // uses the authority and a host that uses the bridge must not end
    // up with different engines.
    auto profile = nimrtc::assembly::kProfileAgentGateway;
    profile.transport.media.max_p99_ms = 55;

    EngineConfig manual;
    apply_transport_profile(profile, manual);

    const EngineConfig authority = via_authority(profile);

    EXPECT_EQ(authority.transport_strategy_name,
              manual.transport_strategy_name);
    EXPECT_EQ(authority.transport_media_max_p99_ms,
              manual.transport_media_max_p99_ms);
}

TEST_F(EngineTransportProfile, authority_carries_the_p99_override) {
    // The published authority must carry the per-instance budget too —
    // this is the exact field that used to be dropped.
    auto profile = nimrtc::assembly::kProfileAgentGateway;
    profile.transport.media.max_p99_ms = 55;

    const EngineConfig cfg = via_authority(profile);
    ASSERT_TRUE(cfg.transport_media_max_p99_ms.has_value());
    EXPECT_EQ(*cfg.transport_media_max_p99_ms, 55);
    EXPECT_EQ(cfg.transport_strategy_name, "agent-gateway");
}

TEST_F(EngineTransportProfile, no_override_keeps_the_strategy_default) {
    // The default-constructed `EngineConfig` must behave exactly as it
    // did before the new field existed.
    EngineConfig cfg;
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_TRUE(engine.transport_media_max_p99_ms().has_value());
    EXPECT_EQ(*engine.transport_media_max_p99_ms(), 200)
        << "the engine default strategy is browser-call (200 ms)";
    engine.close();
}

TEST_F(EngineTransportProfile, budget_is_empty_before_open) {
    // The accessor is diagnostic surface: before open() there is no
    // resolved strategy, so there is no budget.
    EngineConfig cfg;
    cfg.transport_strategy_name    = "agent-gateway";
    cfg.transport_media_max_p99_ms = 60;

    NimRTCEngine engine(cfg);
    EXPECT_FALSE(engine.transport_media_max_p99_ms().has_value());
}

TEST_F(EngineTransportProfile, unknown_strategy_still_fails_open) {
    // The bridge does not weaken the engine's loud-fail contract: a
    // Profile naming an unknown strategy must still fail open().
    auto profile = nimrtc::assembly::kProfileAgentGateway;
    profile.transport.strategy = "webrtc-quic-call";

    EngineConfig cfg;
    apply_transport_profile(profile, cfg);

    NimRTCEngine engine(cfg);
    EXPECT_NE(engine.open(), 0u)
        << "an unknown strategy name must fail open()";
    EXPECT_FALSE(engine.is_open());
}

}  // namespace
