/**
 * @file src/transport/tests/test_raw_udp_arq_stack.cpp
 * @brief Unit tests for RawUdpArqStack + RawUdpArqStackFactory
 *        (transport-selection.md §6.3 — Slice 7.6).
 *
 * Tests cover:
 *   - Factory is registered in core::PluginRegistry under id "raw-udp-arq"
 *   - Factory id() / display_name() / create() return sane values
 *   - create() returns a non-null ITransportStack
 *   - raw_control() returns a non-null IRawUdpDatagram
 *   - ice/dtls/rtp/sctp accessors return references (null instances;
 *     not dereferenced by callers; the Selector's per-stack id gates
 *     access)
 *   - start() / close() lifecycle is idempotent
 *   - Registry idempotency: re-registering does not duplicate
 */
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/plugins/base.hpp>
#include <nimrtc/raw_udp/raw_udp_datagram.hpp>
#include <nimrtc/raw_udp/raw_udp_plugin.hpp>   // raw_udp::register_default_plugins
#include <nimrtc/transport/raw_udp_arq_stack.hpp>
#include <nimrtc/transport/transport_plugin.hpp>
#include <nimrtc/transport/transport_stack.hpp>

namespace nimrtc {
namespace {

// Register the per-module default plugins once per binary. The Slice 7.6
// RawUdpArqStack::start() resolves `cfg.raw_id` (default "arq") through
// `core::PluginRegistry::get_raw_udp_datagram(id)`; the "arq" factory is
// published by nimrtc::raw_udp::register_default_plugins() — which lives
// in nimrtc_raw_udp.lib.  We do NOT call nimrtc::core::register_all_default_plugins()
// here, because that walks PAL Slice 2's kDefaultRegistrars[] table which
// references all 13 module-level register_default_plugins() symbols —
// and the test binary doesn't link them.
//
// This static runs before main() (static init order), so the registry is
// populated by the time any TEST body runs.
struct SuiteSetup {
    SuiteSetup() {
        nimrtc::raw_udp::register_default_plugins();
        nimrtc::transport::register_default_plugins();
    }
};
static const SuiteSetup s_setup;

// ===========================================================================
// Factory registration
// ===========================================================================
TEST(RawUdpArqStackFactory, RegisteredInPluginRegistry) {
    // Trigger default registration (idempotent).
    nimrtc::transport::register_default_plugins();

    const auto* factory =
        core::PluginRegistry::instance().get_transport_stack("raw-udp-arq");
    ASSERT_NE(factory, nullptr)
        << "RawUdpArqStackFactory (id='raw-udp-arq') not registered; "
           "ensure nimrtc::transport::register_default_plugins() was called";
    EXPECT_EQ(factory->id(), "raw-udp-arq");
    EXPECT_FALSE(factory->display_name().empty());
}

TEST(RawUdpArqStackFactory, CoexistsWithWebRtcClassic) {
    nimrtc::transport::register_default_plugins();
    auto stacks = core::PluginRegistry::instance().list_transport_stacks();
    // Expect 3 entries: "default" (shell) + "webrtc-classic" + "raw-udp-arq".
    EXPECT_GE(stacks.size(), 3u)
        << "Expected at least 3 transport-stack entries "
           "(default + webrtc-classic + raw-udp-arq)";
    bool found_wr = false, found_raw = false, found_def = false;
    for (auto id : stacks) {
        if (id == "webrtc-classic") found_wr = true;
        if (id == "raw-udp-arq")    found_raw = true;
        if (id == "default")        found_def = true;
    }
    EXPECT_TRUE(found_wr);
    EXPECT_TRUE(found_raw);
    EXPECT_TRUE(found_def);
}

TEST(RawUdpArqStackFactory, IdempotentRegistration) {
    // Call twice and check that the count doesn't grow.
    nimrtc::transport::register_default_plugins();
    auto stacks1 = core::PluginRegistry::instance().list_transport_stacks();
    nimrtc::transport::register_default_plugins();
    auto stacks2 = core::PluginRegistry::instance().list_transport_stacks();
    EXPECT_EQ(stacks1.size(), stacks2.size());
}

// ===========================================================================
// create() — produces a real ITransportStack
// ===========================================================================
TEST(RawUdpArqStackFactory, CreateReturnsNonNullStack) {
    nimrtc::transport::register_default_plugins();
    const auto* factory =
        core::PluginRegistry::instance().get_transport_stack("raw-udp-arq");
    ASSERT_NE(factory, nullptr);

    StackConfig cfg;
    cfg.raw_id = "arq";   // Slice 6 default ARQ factory
    auto stack = factory->create(cfg);
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->id(), "raw-udp-arq");
}

TEST(RawUdpArqStack, RawControlIsNonNull) {
    nimrtc::transport::register_default_plugins();
    const auto* factory =
        core::PluginRegistry::instance().get_transport_stack("raw-udp-arq");
    ASSERT_NE(factory, nullptr);

    StackConfig cfg;
    cfg.raw_id = "arq";
    auto stack = factory->create(cfg);
    ASSERT_NE(stack, nullptr);

    // Lazy init: raw_control() is null until start() opens the ARQ
    // session. Matches test_engine_plugin_loading.cpp's
    // `slice76_raw_udp_arq_stack_create_and_lifecycle` expectation.
    EXPECT_EQ(stack->raw_control(), nullptr)
        << "raw_control() must be null BEFORE start() (lazy init)";
    EXPECT_NO_THROW(stack->start());
    ASSERT_NE(stack->raw_control(), nullptr)
        << "RawUdpArqStack::raw_control() must be non-null AFTER start() "
           "— the whole point of this stack is to provide a real ARQ session";
    EXPECT_NO_THROW(stack->close());
}

TEST(RawUdpArqStack, IceDtlsRtpSctpAccessorsReturnNullInstances) {
    // The control stack deliberately does NOT own ICE / DTLS / RTP / SCTP.
    // The accessors must return references to null-instance singletons
    // (so the interface is satisfied without a crash). The engine gates
    // every component call on the Selector's per-stack id, so it never
    // derefs these on a raw-udp-arq stack — but if a future caller does
    // deref one, it gets a no-op / kErrNotReady back rather than UB.
    nimrtc::transport::register_default_plugins();
    const auto* factory =
        core::PluginRegistry::instance().get_transport_stack("raw-udp-arq");
    ASSERT_NE(factory, nullptr);
    auto stack = factory->create({});
    ASSERT_NE(stack, nullptr);

    // Just touch the accessors — they must not throw / crash.
    EXPECT_NE(&stack->ice(),  nullptr);
    EXPECT_NE(&stack->dtls(), nullptr);
    EXPECT_NE(&stack->rtp(),  nullptr);
    EXPECT_NE(&stack->sctp(), nullptr);
}

// ===========================================================================
// Lifecycle
// ===========================================================================
TEST(RawUdpArqStackLifecycle, StartThenClose) {
    nimrtc::transport::register_default_plugins();
    const auto* factory =
        core::PluginRegistry::instance().get_transport_stack("raw-udp-arq");
    ASSERT_NE(factory, nullptr);

    StackConfig cfg;
    cfg.raw_id = "arq";
    auto stack = factory->create(cfg);
    ASSERT_NE(stack, nullptr);

    // Lazy init: raw_control() is null before start().
    EXPECT_EQ(stack->raw_control(), nullptr)
        << "raw_control() must be null before start()";
    EXPECT_NO_THROW(stack->start());
    // After start: ARQ open, sockets bound. The control stack is now
    // usable for send() / on_recv() / close().
    ASSERT_NE(stack->raw_control(), nullptr);
    EXPECT_NO_THROW(stack->close());

    // After close: raw_control() is reset to nullptr.
    EXPECT_EQ(stack->raw_control(), nullptr);
}

TEST(RawUdpArqStackLifecycle, CloseWithoutStartIsNoOp) {
    nimrtc::transport::register_default_plugins();
    const auto* factory =
        core::PluginRegistry::instance().get_transport_stack("raw-udp-arq");
    ASSERT_NE(factory, nullptr);
    auto stack = factory->create({});
    EXPECT_NO_THROW(stack->close())
        << "close() on a never-started stack must be a no-op (idempotent)";
}

TEST(RawUdpArqStackLifecycle, DoubleStartIsIdempotent) {
    nimrtc::transport::register_default_plugins();
    const auto* factory =
        core::PluginRegistry::instance().get_transport_stack("raw-udp-arq");
    ASSERT_NE(factory, nullptr);
    StackConfig cfg;
    cfg.raw_id = "arq";
    auto stack = factory->create(cfg);

    EXPECT_NO_THROW(stack->start());
    // Second start() must be a no-op (the impl logs "already started"
    // and returns early).
    EXPECT_NO_THROW(stack->start());
    EXPECT_NO_THROW(stack->close());
}

TEST(RawUdpArqStackLifecycle, DoubleCloseIsIdempotent) {
    nimrtc::transport::register_default_plugins();
    const auto* factory =
        core::PluginRegistry::instance().get_transport_stack("raw-udp-arq");
    ASSERT_NE(factory, nullptr);
    StackConfig cfg;
    cfg.raw_id = "arq";
    auto stack = factory->create(cfg);

    EXPECT_NO_THROW(stack->start());
    EXPECT_NO_THROW(stack->close());
    EXPECT_NO_THROW(stack->close());
    EXPECT_EQ(stack->raw_control(), nullptr);
}

}  // namespace
}  // namespace nimrtc
