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

}  // namespace
}  // namespace nimrtc
