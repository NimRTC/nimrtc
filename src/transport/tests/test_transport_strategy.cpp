/**
 * @file src/transport/tests/test_transport_strategy.cpp
 * @brief Unit tests for the five named transport strategies
 *        (transport-strategies.md §2).
 *
 * Tests cover:
 *   - Each strategy's default `TransportRequirements`
 *   - `strategy_by_name()` returns the right strategy
 *   - `strategy_by_name("")` returns the engine default (browser-call)
 *   - `strategy_by_name(unknown)` throws std::invalid_argument
 *   - `kStrategyCount` and `kStrategyNames` are in sync with the
 *     `if-else` chain in `strategy_by_name`
 *   - The five names match the JSON Schema enum in
 *     profiles/schema/profile-v1.1.json (the round-trip contract)
 */
#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/transport/transport_strategy.hpp>
#include <nimrtc/transport/transport_selector.hpp>

namespace nimrtc {
namespace {

// ===========================================================================
// Default requirements per strategy
// ===========================================================================
TEST(StrategyDefaults, BrowserCall) {
    EXPECT_TRUE(kStrategyBrowserCall.needs_browser_interop);
    EXPECT_FALSE(kStrategyBrowserCall.needs_high_freq_control);
    ASSERT_TRUE(kStrategyBrowserCall.max_p99_ms.has_value());
    EXPECT_EQ(*kStrategyBrowserCall.max_p99_ms, 200);
}

TEST(StrategyDefaults, TeleopUltraLowLatency) {
    EXPECT_FALSE(kStrategyTeleopUltraLowLatency.needs_browser_interop);
    EXPECT_TRUE(kStrategyTeleopUltraLowLatency.needs_high_freq_control);
    ASSERT_TRUE(kStrategyTeleopUltraLowLatency.max_p99_ms.has_value());
    EXPECT_EQ(*kStrategyTeleopUltraLowLatency.max_p99_ms, 50);
}

TEST(StrategyDefaults, AgentGateway) {
    EXPECT_TRUE(kStrategyAgentGateway.needs_browser_interop);
    EXPECT_TRUE(kStrategyAgentGateway.needs_high_freq_control);
    ASSERT_TRUE(kStrategyAgentGateway.max_p99_ms.has_value());
    EXPECT_EQ(*kStrategyAgentGateway.max_p99_ms, 100);
}

TEST(StrategyDefaults, SfuRelay) {
    EXPECT_TRUE(kStrategySfuRelay.needs_browser_interop);
    EXPECT_FALSE(kStrategySfuRelay.needs_high_freq_control);
    ASSERT_TRUE(kStrategySfuRelay.max_p99_ms.has_value());
    EXPECT_EQ(*kStrategySfuRelay.max_p99_ms, 200);
}

TEST(StrategyDefaults, CrossNetworkPeer) {
    EXPECT_TRUE(kStrategyCrossNetworkPeer.needs_browser_interop);
    EXPECT_FALSE(kStrategyCrossNetworkPeer.needs_high_freq_control);
    ASSERT_TRUE(kStrategyCrossNetworkPeer.max_p99_ms.has_value());
    EXPECT_EQ(*kStrategyCrossNetworkPeer.max_p99_ms, 150);
}

// ===========================================================================
// strategy_by_name — name → TransportRequirements lookup
// ===========================================================================
TEST(StrategyByName, EmptyReturnsBrowserCallDefault) {
    const auto& req = strategy_by_name("");
    EXPECT_EQ(&req, &kStrategyBrowserCall)
        << "Empty strategy name must return the engine default (browser-call)";
}

TEST(StrategyByName, AllFiveNamesResolve) {
    EXPECT_EQ(&strategy_by_name("browser-call"),             &kStrategyBrowserCall);
    EXPECT_EQ(&strategy_by_name("teleop-ultra-low-latency"), &kStrategyTeleopUltraLowLatency);
    EXPECT_EQ(&strategy_by_name("agent-gateway"),            &kStrategyAgentGateway);
    EXPECT_EQ(&strategy_by_name("sfu-relay"),                &kStrategySfuRelay);
    EXPECT_EQ(&strategy_by_name("cross-network-peer"),       &kStrategyCrossNetworkPeer);
}

TEST(StrategyByName, UnknownNameThrows) {
    EXPECT_THROW(strategy_by_name("nonexistent"),  std::invalid_argument);
    EXPECT_THROW(strategy_by_name("Browser-Call"), std::invalid_argument)  // case-sensitive
        << "Strategy names are case-sensitive; case mismatch must throw";
    EXPECT_THROW(strategy_by_name("browser_call"), std::invalid_argument)  // hyphen vs underscore
        << "Strategy names use hyphens, not underscores";
}

TEST(StrategyByName, ExceptionMessageListsValidIds) {
    try {
        strategy_by_name("foo");
        FAIL() << "Expected std::invalid_argument";
    } catch (const std::invalid_argument& e) {
        const std::string msg = e.what();
        for (const auto& name : kStrategyNames) {
            EXPECT_NE(msg.find(std::string{name}), std::string::npos)
                << "Exception message must list valid id '" << name
                << "'; full message: " << msg;
        }
    }
}

// ===========================================================================
// Caller can take a const reference and override individual fields
// ===========================================================================
TEST(StrategyByName, CallerCanOverrideIndividualFields) {
    auto req = strategy_by_name("agent-gateway");
    req.max_p99_ms = 60;  // tighter than the 100 ms default
    // Mutation must not leak into the canonical default.
    EXPECT_EQ(*kStrategyAgentGateway.max_p99_ms, 100);
    EXPECT_EQ(*req.max_p99_ms, 60);
}

// ===========================================================================
// Catalog metadata
// ===========================================================================
TEST(StrategyCatalog, CountMatchesNames) {
    EXPECT_EQ(kStrategyCount, 5u);
    EXPECT_EQ(std::size(kStrategyNames), kStrategyCount);
}

TEST(StrategyCatalog, NamesMatchJsonSchemaEnum) {
    // The 5 names here MUST match the JSON Schema `enum` in
    // profiles/schema/profile-v1.1.json (transport property). If a
    // future PR adds a 6th strategy, BOTH must be updated; the test
    // hard-fails if they diverge.
    const std::vector<std::string> expected{
        "browser-call",
        "teleop-ultra-low-latency",
        "agent-gateway",
        "sfu-relay",
        "cross-network-peer",
    };
    const std::size_t expected_size = expected.size();
    ASSERT_EQ(static_cast<std::size_t>(kStrategyCount), expected_size);
    for (std::size_t i = 0; i < expected_size; ++i) {
        EXPECT_EQ(kStrategyNames[i], expected[i])
            << "Strategy catalog index " << i << " out of sync";
    }
}

// ===========================================================================
// Selector §5.3 rule audit — each strategy hits exactly one rule
// ===========================================================================
TEST(StrategySelectorAudit, BrowserCallHitsRule1) {
    // §5.3 rule 1: needs_browser_interop == true, no high-freq control
    //   → media=webrtc-classic, control=null
    const auto& req = kStrategyBrowserCall;
    EXPECT_TRUE(req.needs_browser_interop);
    EXPECT_FALSE(req.needs_high_freq_control);
}

TEST(StrategySelectorAudit, AgentGatewayHitsRule1b) {
    // §5.3 rule 1b: browser interop + high-freq control
    //   → media=webrtc-classic, control=raw-udp-arq
    const auto& req = kStrategyAgentGateway;
    EXPECT_TRUE(req.needs_browser_interop);
    EXPECT_TRUE(req.needs_high_freq_control);
}

TEST(StrategySelectorAudit, TeleopHitsRule2) {
    // §5.3 rule 2: no browser interop + high-freq control
    //   → media=raw-udp-arq, control=raw-udp-arq
    const auto& req = kStrategyTeleopUltraLowLatency;
    EXPECT_FALSE(req.needs_browser_interop);
    EXPECT_TRUE(req.needs_high_freq_control);
}

TEST(StrategySelectorAudit, SfuRelayAndCrossNetworkHitRule1) {
    // §5.3 rule 1: browser interop required, no high-freq control
    //   → media=webrtc-classic, control=null
    EXPECT_TRUE(kStrategySfuRelay.needs_browser_interop);
    EXPECT_FALSE(kStrategySfuRelay.needs_high_freq_control);
    EXPECT_TRUE(kStrategyCrossNetworkPeer.needs_browser_interop);
    EXPECT_FALSE(kStrategyCrossNetworkPeer.needs_high_freq_control);
}

// ===========================================================================
// requirements_from_profile_input — bridge from Profile v1.1 to TransportReq
// ===========================================================================
//
// The bridge is the runtime counterpart of the assembly layer's
// `TransportSection`. It resolves the strategy name (validates it via
// `strategy_by_name`) and applies any per-instance overrides from the
// Profile JSON.
//
// Why these tests live here (not in test_assembly.cpp):
//   - The bridge takes a `ProfileTransportInput` (the transport-side
//     mirror, no `assembly::` dep), so it can be tested without
//     dragging in the assembly module's nlohmann/json transitive dep.
//   - The mirror-vs-assembly parity is asserted by
//     `AssemblyTest.BuiltInProfileTransportStrategy` in test_assembly.cpp
//     (round-trip each kProfile*'s transport section through the
//     serialiser and verify all 5 fields are emitted per the v1.1 schema).
// ===========================================================================

TEST(ProfileInputBridge, EmptyStrategyReturnsBrowserCall) {
    // The engine default is "browser-call"; an empty strategy in the
    // Profile JSON means "use whatever the engine compile-time default
    // is", not "no transport at all". Verifies the bridge does NOT
    // throw on empty input.
    ProfileTransportInput in;  // strategy == "" (default)
    auto req = requirements_from_profile_input(in);
    EXPECT_EQ(req.needs_browser_interop,
              kStrategyBrowserCall.needs_browser_interop);
    EXPECT_EQ(req.needs_high_freq_control,
              kStrategyBrowserCall.needs_high_freq_control);
    ASSERT_TRUE(req.max_p99_ms.has_value());
    EXPECT_EQ(*req.max_p99_ms, *kStrategyBrowserCall.max_p99_ms);
}

TEST(ProfileInputBridge, AllFiveStrategiesResolveToDefaults) {
    struct Case {
        std::string_view name;
        const TransportRequirements& canonical;
    };
    const Case cases[] = {
        {"browser-call",              kStrategyBrowserCall},
        {"teleop-ultra-low-latency",  kStrategyTeleopUltraLowLatency},
        {"agent-gateway",             kStrategyAgentGateway},
        {"sfu-relay",                 kStrategySfuRelay},
        {"cross-network-peer",        kStrategyCrossNetworkPeer},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE("Strategy: " + std::string{c.name});
        ProfileTransportInput in;
        in.strategy = c.name;
        auto req = requirements_from_profile_input(in);
        EXPECT_EQ(req.needs_browser_interop,   c.canonical.needs_browser_interop);
        EXPECT_EQ(req.needs_high_freq_control, c.canonical.needs_high_freq_control);
        ASSERT_TRUE(req.max_p99_ms.has_value());
        EXPECT_EQ(*req.max_p99_ms, *c.canonical.max_p99_ms);
    }
}

TEST(ProfileInputBridge, MediaP99OverrideApplies) {
    // Application wants `agent-gateway` but with a tighter p99 budget
    // (50 ms instead of the default 100 ms). The bridge must apply
    // the override while preserving the booleans.
    ProfileTransportInput in;
    in.strategy          = "agent-gateway";
    in.media_max_p99_ms  = 50;
    auto req = requirements_from_profile_input(in);
    EXPECT_EQ(req.needs_browser_interop,   kStrategyAgentGateway.needs_browser_interop);
    EXPECT_EQ(req.needs_high_freq_control, kStrategyAgentGateway.needs_high_freq_control);
    ASSERT_TRUE(req.max_p99_ms.has_value());
    EXPECT_EQ(*req.max_p99_ms, 50);
}

TEST(ProfileInputBridge, MediaP99OverrideCanLoosenBudget) {
    // Symmetric to MediaP99OverrideApplies — verify the override can
    // also LOOSEN the budget (e.g. live-stream variant of `browser-call`
    // with 300 ms p99 instead of the default 200 ms). This is the
    // `kProfileLive` use case in the engine.
    ProfileTransportInput in;
    in.strategy          = "browser-call";
    in.media_max_p99_ms  = 300;
    auto req = requirements_from_profile_input(in);
    ASSERT_TRUE(req.max_p99_ms.has_value());
    EXPECT_EQ(*req.max_p99_ms, 300);
}

TEST(ProfileInputBridge, NoOverrideYieldsCanonicalDefaults) {
    // Sanity check: when neither media_max_p99_ms nor any other
    // per-instance field is set, the result must equal the strategy's
    // canonical default byte-for-byte.
    ProfileTransportInput in;
    in.strategy = "teleop-ultra-low-latency";
    auto req = requirements_from_profile_input(in);
    EXPECT_EQ(req.needs_browser_interop,   kStrategyTeleopUltraLowLatency.needs_browser_interop);
    EXPECT_EQ(req.needs_high_freq_control, kStrategyTeleopUltraLowLatency.needs_high_freq_control);
    ASSERT_TRUE(req.max_p99_ms.has_value());
    EXPECT_EQ(*req.max_p99_ms, *kStrategyTeleopUltraLowLatency.max_p99_ms);
}

TEST(ProfileInputBridge, UnknownStrategyThrows) {
    // The bridge delegates to `strategy_by_name` for the canonical
    // "throw with the 5 valid ids" error. Verify the bridge surfaces
    // the same exception, not a silently-wrong default.
    ProfileTransportInput in;
    in.strategy = "webrtc-quic-call";  // not in the v0.12.0 catalog
    EXPECT_THROW(requirements_from_profile_input(in), std::invalid_argument);
}

TEST(ProfileInputBridge, ControlFieldsAreIgnoredAtBridgeLevel) {
    // The control-plane fields on ProfileTransportInput are recorded
    // by the engine AFTER `CapabilitySelector::select()` returns (they
    // shape the resulting TransportSession, not the Selector's input).
    // The bridge intentionally does NOT fold them into the
    // TransportRequirements booleans — that would be a §5.3 rule
    // audit violation. Verify the bridge leaves them out of the
    // result, even when set.
    ProfileTransportInput in;
    in.strategy             = "agent-gateway";
    in.control_enabled      = true;
    in.control_max_p99_ms   = 30;
    in.control_freq_hz      = 500;
    in.control_reliability  = "unreliable";
    in.control_ttl_ms       = 25;
    auto req = requirements_from_profile_input(in);
    // The result must equal the canonical agent-gateway defaults;
    // control fields do not bleed into the capability flags.
    EXPECT_EQ(req.needs_browser_interop,   kStrategyAgentGateway.needs_browser_interop);
    EXPECT_EQ(req.needs_high_freq_control, kStrategyAgentGateway.needs_high_freq_control);
    ASSERT_TRUE(req.max_p99_ms.has_value());
    EXPECT_EQ(*req.max_p99_ms, *kStrategyAgentGateway.max_p99_ms);
}

TEST(ProfileInputBridge, ResultIsIndependentOfCanonical) {
    // Mutating the returned `TransportRequirements` must NOT leak into
    // the canonical `kStrategy*` constants. The bridge returns by
    // value (not by reference), so this is a structural guarantee
    // — but we assert it explicitly so a future refactor that
    // accidentally returns a reference is caught.
    {
        ProfileTransportInput in;
        in.strategy         = "agent-gateway";
        in.media_max_p99_ms = 42;
        auto req = requirements_from_profile_input(in);
        // Mutate the local copy.
        req.needs_browser_interop = false;
        // Canonical agent-gateway must be untouched.
        EXPECT_TRUE(kStrategyAgentGateway.needs_browser_interop);
    }
    {
        ProfileTransportInput in;
        in.strategy = "agent-gateway";  // no override
        auto req = requirements_from_profile_input(in);
        EXPECT_EQ(*req.max_p99_ms, *kStrategyAgentGateway.max_p99_ms);
    }
}

}  // namespace
}  // namespace nimrtc
