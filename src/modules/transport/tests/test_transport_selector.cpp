/**
 * @file src/transport/tests/test_transport_selector.cpp
 * @brief Unit tests for CapabilitySelector (transport-selection.md §5.3).
 *
 * These tests verify the §5.3 four-rule priority table by inspecting the
 * returned `TransportSession`'s id strings and the selector's
 * `last_trace_`. They do NOT construct real ITransportStack instances —
 * the Slice-7 rule engine is fully verifiable from id-string output
 * alone, and factory-resolution tests use stub resolvers that return
 * nullptr for the factory (which exercises the resolver callback path
 * without requiring mock stacks).
 *
 * Coverage:
 *   - Rule 1  (browser interop only)
 *   - Rule 1b (browser interop + high-freq control)
 *   - Rule 2  (no browser interop + high-freq control)
 *   - Rule 3  (no browser interop + prefer_quic; Slice 7.x 二期 stub)
 *   - Rule 4  (default — browser interop)
 *   - Rule precedence: browser_interop beats prefer_quic
 *   - Default-ctor Selector returns empty stacks but correct ids
 *   - Resolver callbacks are invoked with the right factory ids
 *   - TransportSession start/close are no-ops on empty session
 *   - last_trace_ is updated on every select() call
 *   - Resolver returning nullptr → empty stack but id still set
 */
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/transport/transport_selector.hpp>
#include <nimrtc/transport/transport_stack.hpp>

namespace nimrtc {
namespace {

// Types are at nimrtc:: scope — refer to them by full qualified name or
// rely on implicit lookup from enclosing namespace.

// =========================================================================
// Rule 1 — needs_browser_interop=true, no high-freq control
// =========================================================================
TEST(CapabilitySelectorRule1, BrowserInteropOnlyMediaStack) {
    CapabilitySelector s;
    TransportRequirements req;
    req.needs_browser_interop = true;
    req.needs_high_freq_control = false;

    auto session = s.select(req);
    EXPECT_EQ(session.media_stack_id, "webrtc-classic");
    EXPECT_EQ(session.control_stack_id, "");

    auto trace = s.last_trace();
    EXPECT_EQ(trace.media_factory_id, "webrtc-classic");
    EXPECT_EQ(trace.control_factory_id, "");
}

// =========================================================================
// Rule 1b — needs_browser_interop=true + needs_high_freq_control=true
// =========================================================================
TEST(CapabilitySelectorRule1b, BrowserInteropWithHighFreqControl) {
    CapabilitySelector s;
    TransportRequirements req;
    req.needs_browser_interop = true;
    req.needs_high_freq_control = true;

    auto session = s.select(req);
    EXPECT_EQ(session.media_stack_id, "webrtc-classic");
    EXPECT_EQ(session.control_stack_id, "raw-udp-arq");

    auto trace = s.last_trace();
    EXPECT_EQ(trace.media_factory_id, "webrtc-classic");
    EXPECT_EQ(trace.control_factory_id, "raw-udp-arq");
}

// =========================================================================
// Rule 2 — needs_browser_interop=false + needs_high_freq_control=true
// =========================================================================
TEST(CapabilitySelectorRule2, NoBrowserInteropHighFreqDualStacks) {
    CapabilitySelector s;
    TransportRequirements req;
    req.needs_browser_interop = false;
    req.needs_high_freq_control = true;

    auto session = s.select(req);
    EXPECT_EQ(session.media_stack_id, "raw-udp-arq");
    EXPECT_EQ(session.control_stack_id, "raw-udp-arq");

    auto trace = s.last_trace();
    EXPECT_EQ(trace.media_factory_id, "raw-udp-arq");
    EXPECT_EQ(trace.control_factory_id, "raw-udp-arq");
}

// =========================================================================
// Rule 3 — needs_browser_interop=false + prefer_quic=true
// =========================================================================
TEST(CapabilitySelectorRule3, NoBrowserPreferQuic) {
    CapabilitySelector s;
    TransportRequirements req;
    req.needs_browser_interop = false;
    req.needs_high_freq_control = false;
    req.prefer_quic = true;

    auto session = s.select(req);
    EXPECT_EQ(session.media_stack_id, "webrtc-quic");
    EXPECT_EQ(session.control_stack_id, "");

    auto trace = s.last_trace();
    EXPECT_EQ(trace.media_factory_id, "webrtc-quic");
    EXPECT_EQ(trace.control_factory_id, "");
}

// =========================================================================
// Rule 3 documented in §5.3: control_stack is "per needs_high_freq_control".
// In practice, when both high_freq_control and prefer_quic are set without
// browser interop, Rule 2 wins (raw-udp-arq × 2). The dedicated test for
// this precedence is `Precedence.HighFreqControlBeatsPreferQuic` above —
// we do NOT add a contradictory Rule3 + high_freq_control test.
// =========================================================================
// Rule 4 — default (browser interop is the default in TransportRequirements)
// =========================================================================
TEST(CapabilitySelectorRule4, DefaultSelectsWebRtcClassic) {
    CapabilitySelector s;
    TransportRequirements req;  // defaults: needs_browser_interop=true

    auto session = s.select(req);
    EXPECT_EQ(session.media_stack_id, "webrtc-classic");
    EXPECT_EQ(session.control_stack_id, "");

    auto trace = s.last_trace();
    EXPECT_EQ(trace.media_factory_id, "webrtc-classic");
    EXPECT_EQ(trace.control_factory_id, "");
}

// =========================================================================
// Rule precedence: browser interop wins over prefer_quic
// =========================================================================
TEST(CapabilitySelectorPrecedence, BrowserInteropBeatsPreferQuic) {
    CapabilitySelector s;
    TransportRequirements req;
    req.needs_browser_interop = true;
    req.prefer_quic = true;   // should be ignored — Rule 1 wins

    auto session = s.select(req);
    EXPECT_EQ(session.media_stack_id, "webrtc-classic");

    auto trace = s.last_trace();
    EXPECT_EQ(trace.media_factory_id, "webrtc-classic");
}

// =========================================================================
// Rule precedence: high_freq_control beats prefer_quic when both set
// without browser interop (Rule 2 before Rule 3)
// =========================================================================
TEST(CapabilitySelectorPrecedence, HighFreqControlBeatsPreferQuic) {
    CapabilitySelector s;
    TransportRequirements req;
    req.needs_browser_interop = false;
    req.needs_high_freq_control = true;
    req.prefer_quic = true;   // should be ignored — Rule 2 wins

    auto session = s.select(req);
    EXPECT_EQ(session.media_stack_id, "raw-udp-arq");
    EXPECT_EQ(session.control_stack_id, "raw-udp-arq");

    auto trace = s.last_trace();
    EXPECT_EQ(trace.media_factory_id, "raw-udp-arq");
    EXPECT_EQ(trace.control_factory_id, "raw-udp-arq");
}

// =========================================================================
// Default Selector (no resolvers) — stacks are nullptr but ids are correct
// =========================================================================
TEST(CapabilitySelectorDefaultCtor, NoResolversReturnsEmptyStacksWithIds) {
    CapabilitySelector s;  // default ctor: both resolvers null

    TransportRequirements req;
    req.needs_browser_interop = true;
    req.needs_high_freq_control = true;

    auto session = s.select(req);
    EXPECT_EQ(session.media_stack, nullptr);
    EXPECT_EQ(session.control_stack, nullptr);
    EXPECT_EQ(session.media_stack_id, "webrtc-classic");
    EXPECT_EQ(session.control_stack_id, "raw-udp-arq");
}

// =========================================================================
// Resolver callbacks receive the correct factory id strings
// =========================================================================
TEST(CapabilitySelectorResolver, LookupCallbackReceivesExpectedIds) {
    std::vector<std::string> lookup_calls;

    auto factory_lookup =
        [&](std::string_view id) -> const ITransportStackFactory* {
        lookup_calls.push_back(std::string{id});
        return nullptr;  // tell selector "no factory" so no stack is built
    };

    CapabilitySelector s(factory_lookup, nullptr);

    TransportRequirements req;
    req.needs_browser_interop = true;
    req.needs_high_freq_control = true;

    s.select(req);

    // First lookup is for the media stack ("webrtc-classic"), second for
    // the control stack ("raw-udp-arq").
    ASSERT_GE(lookup_calls.size(), 2u);
    EXPECT_EQ(lookup_calls[0], "webrtc-classic");
    EXPECT_EQ(lookup_calls[1], "raw-udp-arq");
}

// =========================================================================
// Resolver returning nullptr → empty stack but id strings still set
// =========================================================================
TEST(CapabilitySelectorResolver, NullFactoryReturnsEmptyStackButKeepsIds) {
    auto factory_lookup =
        [](std::string_view) -> const ITransportStackFactory* {
        return nullptr;
    };

    CapabilitySelector s(factory_lookup, nullptr);

    TransportRequirements req;
    req.needs_browser_interop = true;

    auto session = s.select(req);
    EXPECT_EQ(session.media_stack, nullptr);
    EXPECT_EQ(session.media_stack_id, "webrtc-classic");
}

// =========================================================================
// TransportSession start/close are no-ops on empty session
// =========================================================================
TEST(TransportSessionLifecycle, StartCloseNoOpOnEmptySession) {
    TransportSession s;
    EXPECT_NO_THROW(s.start());
    EXPECT_NO_THROW(s.close());
}

// =========================================================================
// Multiple select() calls refresh last_trace_ independently
// =========================================================================
TEST(CapabilitySelectorMultiCall, LastTraceUpdatesPerCall) {
    CapabilitySelector s;

    TransportRequirements req_a;
    req_a.needs_browser_interop = true;
    req_a.needs_high_freq_control = false;
    s.select(req_a);
    EXPECT_EQ(s.last_trace().media_factory_id, "webrtc-classic");
    EXPECT_EQ(s.last_trace().control_factory_id, "");

    TransportRequirements req_b;
    req_b.needs_browser_interop = false;
    req_b.needs_high_freq_control = true;
    s.select(req_b);
    EXPECT_EQ(s.last_trace().media_factory_id, "raw-udp-arq");
    EXPECT_EQ(s.last_trace().control_factory_id, "raw-udp-arq");

    TransportRequirements req_c;
    req_c.needs_browser_interop = false;
    req_c.prefer_quic = true;
    s.select(req_c);
    EXPECT_EQ(s.last_trace().media_factory_id, "webrtc-quic");
}

// =========================================================================
// StackConfig passes through to factory_create (resolvers see defaults)
// =========================================================================
namespace {

// Minimal mock factory — returns "arq" id, never actually builds a stack.
// We only need it to be non-null so the selector's `try_create_stack`
// invokes our `factory_create` lambda (which captures configs).
class MockStackFactory final : public ITransportStackFactory {
public:
    std::string_view id() const noexcept override { return "arq"; }
    std::string_view display_name() const noexcept override {
        return "MockStackFactory(test)";
    }
    std::unique_ptr<ITransportStack> create(const StackConfig&) const override {
        // Test only cares that factory_create is invoked with the right cfg;
        // we return a null stack to skip the actual stack construction.
        return nullptr;
    }
};

}  // namespace

TEST(CapabilitySelectorConfig, StackConfigRawIdSetForHighFreqControl) {
    std::vector<StackConfig> captured_configs;
    static const MockStackFactory s_mock_factory;

    auto factory_lookup =
        [](std::string_view) -> const ITransportStackFactory* {
        return &s_mock_factory;  // non-null so try_create_stack proceeds
    };
    auto factory_create =
        [&](const ITransportStackFactory*,
            const StackConfig& cfg) -> std::unique_ptr<ITransportStack> {
        captured_configs.push_back(cfg);
        return nullptr;
    };

    CapabilitySelector selector(factory_lookup, factory_create);

    TransportRequirements req;
    req.needs_browser_interop = false;
    req.needs_high_freq_control = true;
    selector.select(req);

    // Both media + control stacks should have raw_id="arq" for Rule 2.
    ASSERT_EQ(captured_configs.size(), 2u);
    EXPECT_EQ(captured_configs[0].raw_id, "arq");
    EXPECT_EQ(captured_configs[1].raw_id, "arq");
}

// =========================================================================
// The engine's StackConfig reaches the factory (v1.2.0, B9 / Q4)
// =========================================================================
//
// Before v1.2.0 the Selector always used a hard-coded default
// `StackConfig`, so the engine's `EngineConfig` (DTLS backend, SCTP
// socket, raw-UDP factory) never reached the stack factory.  The
// Selector now carries the per-stack backend selection.
TEST(CapabilitySelectorConfig, EngineStackConfigReachesTheFactory) {
    std::vector<StackConfig> captured;
    static const MockStackFactory s_mock_factory;

    StackConfig engine_cfg;
    engine_cfg.ice_id  = "my-ice";
    engine_cfg.dtls_id = "my-dtls";
    engine_cfg.sctp_id = "my-sctp";
    engine_cfg.rtp_id  = "my-rtp";
    engine_cfg.raw_id  = "my-raw";

    CapabilitySelector selector(
        [](std::string_view) -> const ITransportStackFactory* {
            return &s_mock_factory;
        },
        [&](const ITransportStackFactory*,
            const StackConfig& cfg) -> std::unique_ptr<ITransportStack> {
            captured.push_back(cfg);
            return nullptr;
        },
        engine_cfg);

    TransportRequirements req;   // browser interop → media only
    selector.select(req);

    ASSERT_EQ(captured.size(), 1u);
    EXPECT_EQ(captured[0].ice_id,  "my-ice");
    EXPECT_EQ(captured[0].dtls_id, "my-dtls");
    EXPECT_EQ(captured[0].sctp_id, "my-sctp");
    EXPECT_EQ(captured[0].rtp_id,  "my-rtp");
    EXPECT_EQ(captured[0].raw_id,  "my-raw");
}

// =========================================================================
// An explicit raw-UDP id is honoured for the ARQ control plane
// =========================================================================
TEST(CapabilitySelectorConfig, ExplicitRawIdIsUsedForTheArqControlPlane) {
    std::vector<StackConfig> captured;
    static const MockStackFactory s_mock_factory;

    StackConfig engine_cfg;
    engine_cfg.raw_id = "my-arq";

    CapabilitySelector selector(
        [](std::string_view) -> const ITransportStackFactory* {
            return &s_mock_factory;
        },
        [&](const ITransportStackFactory*,
            const StackConfig& cfg) -> std::unique_ptr<ITransportStack> {
            captured.push_back(cfg);
            return nullptr;
        },
        engine_cfg);

    TransportRequirements req;
    req.needs_browser_interop   = true;
    req.needs_high_freq_control = true;
    selector.select(req);

    ASSERT_EQ(captured.size(), 2u);
    EXPECT_EQ(captured[1].raw_id, "my-arq")
        << "the control plane must use the engine's raw-UDP factory id, "
           "not the hard-coded \"arq\"";
}

// =========================================================================
// The media-stack override wins over the §5.3 rule
// =========================================================================
TEST(CapabilitySelectorOverride, MediaOverrideBeatsTheRule) {
    CapabilitySelector s;
    s.set_media_factory_override("my-stack");

    TransportRequirements req;   // browser interop → "webrtc-classic"
    auto session = s.select(req);

    EXPECT_EQ(session.media_stack_id, "my-stack");
    EXPECT_EQ(s.last_trace().media_factory_id, "my-stack");
}

TEST(CapabilitySelectorOverride, MediaOverrideDoesNotAffectTheControlPlane) {
    CapabilitySelector s;
    s.set_media_factory_override("my-stack");

    TransportRequirements req;
    req.needs_browser_interop   = true;
    req.needs_high_freq_control = true;
    auto session = s.select(req);

    EXPECT_EQ(session.media_stack_id, "my-stack");
    EXPECT_EQ(session.control_stack_id, "raw-udp-arq")
        << "the control plane is a property of the strategy, not of the "
           "media backend";
}

TEST(CapabilitySelectorOverride, EmptyOverrideLeavesTheRuleInCharge) {
    CapabilitySelector s;

    TransportRequirements req;
    auto session = s.select(req);

    EXPECT_EQ(session.media_stack_id, "webrtc-classic");
}

// =========================================================================
// instantiate() builds from explicit ids (no rule resolution)
// =========================================================================
TEST(CapabilitySelectorInstantiate, ExplicitIdsAreUsedVerbatim) {
    std::vector<std::string> looked_up;
    static const MockStackFactory s_mock_factory;

    CapabilitySelector selector(
        [&](std::string_view id) -> const ITransportStackFactory* {
            looked_up.push_back(std::string{id});
            return &s_mock_factory;
        },
        [](const ITransportStackFactory*,
           const StackConfig&) -> std::unique_ptr<ITransportStack> {
            return nullptr;
        });

    auto session = selector.instantiate("alpha", "beta");

    EXPECT_EQ(session.media_stack_id, "alpha");
    EXPECT_EQ(session.control_stack_id, "beta");
    ASSERT_EQ(looked_up.size(), 2u);
    EXPECT_EQ(looked_up[0], "alpha");
    EXPECT_EQ(looked_up[1], "beta");
}

TEST(CapabilitySelectorInstantiate, EmptyControlIdMeansNoControlStack) {
    std::vector<std::string> looked_up;
    static const MockStackFactory s_mock_factory;

    CapabilitySelector selector(
        [&](std::string_view id) -> const ITransportStackFactory* {
            looked_up.push_back(std::string{id});
            return &s_mock_factory;
        },
        [](const ITransportStackFactory*,
           const StackConfig&) -> std::unique_ptr<ITransportStack> {
            return nullptr;
        });

    auto session = selector.instantiate("alpha", "");

    EXPECT_EQ(session.control_stack_id, "");
    EXPECT_EQ(session.control_stack, nullptr);
    ASSERT_EQ(looked_up.size(), 1u);
}

}  // namespace
}  // namespace nimrtc
