/**
 * @file tests/test_datachannel_engine.cpp
 * @brief P2 (TPAL-5) — DataChannel plugin engine-routing regression
 *        suite.
 *
 * What this test verifies (per `docs/plan/v0.11-plan.md` §9 Slice 5
 * tracker and the v0.11.0 datachannel subagent scope):
 *
 *   1. `core::PluginRegistry::instance().get_datachannel(id)` resolves
 *      factories by id — same typed-registry contract used for DTLS /
 *      SCTP / raw_udp / transport-stack in Slice 8.
 *
 *   2. `EngineConfig::datachannel_name = "stub_dc"` (a registered
 *      factory id, dispatched through the legacy `datachannel_name`
 *      selector) makes the engine log a "DataChannel plugin ready"
 *      line in `init_modules_once()` and `create_data_channel(label)`
 *      returns a non-null `IDataChannel*` after `open()`.  This proves
 *      the factory ↔ engine wiring is end-to-end functional.
 *
 *      (v1.2.0: the default `datachannel_name` is now `""`, and the
 *      "sctp" backend was retired from the public selectors — see
 *      `docs/plan/datachannel-capability-plan.md` WP-A / DEC-4 = A1.
 *      These tests drive the legacy `datachannel_name` selector with a
 *      test-registered factory instead of the retired SCTP one.)
 *
 *   3. `EngineConfig::datachannel_name = "nope"` (an unregistered id)
 *      makes `create_data_channel()` return nullptr — the engine does
 *      NOT silently fall back to "sctp".
 *
 *   4. The `on_data_message_` / `on_data_state_` callbacks registered
 *      via `set_on_data_message()` / `set_on_data_state()` are
 *      forwarded to the created IDataChannel (via
 *      `IDataChannel::set_on_message` / `set_on_state` before
 *      `IDataChannel::open()`).  Verified by setting a callback that
 *      captures into an `std::atomic<int>` counter and observing the
 *      counter increment when the StubDataChannel's `set_on_message`
 *      forwards the user-supplied callback.
 *
 *   5. `EngineConfig::datachannel_name = ""` (DataChannel disabled)
 *      makes `create_data_channel()` return nullptr even when the
 *      "sctp" factory is registered.
 *
 * The test uses GoogleTest (`GTEST_*`); links `nimrtc_engine`,
 * `nimrtc_core` and the existing plugin-default registrars the same way
 * `test_engine_plugin_loading` does (see tests/CMakeLists.txt —
 * `nimrtc_add_engine_test`).
 *
 * Threading: nothing concurrent; standard GTest ordering.
 *
 * @note P2 — TPAL-5 datachannel cleanup test added as part of v0.11.0.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/error.hpp>
#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/core/engine_errors.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/datachannel.hpp>

namespace {

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
namespace plugins = nimrtc::plugins;

// =====================================================================
// StubDataChannel — minimal IDataChannel implementation that records
// every callback install so the test can verify the engine forwarded
// the user-supplied callbacks correctly.
//
// This stub does NOT actually transmit data — we don't drive a real
// SCTP association (that's a separate test, test_sctp_usrsctp.cpp).
// The stub's open() succeeds immediately; send() / close() are no-ops;
// set_on_message / set_on_state just stash the callback for later
// assertion.
// =====================================================================
class StubDataChannel final : public plugins::IDataChannel {
public:
    StubDataChannel() = default;
    ~StubDataChannel() override = default;

    // ---- IPlugin ----
    const char* name() const noexcept override {
        return "StubDataChannel";
    }
    plugins::Status open() noexcept override {
        opened_count_.fetch_add(1, std::memory_order_relaxed);
        plugin_open_count_.fetch_add(1, std::memory_order_relaxed);
        return plugins::kOk;
    }
    void close() noexcept override {
        closed_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // ---- IDataChannel ----
    void open(const plugins::DataChannelConfig& cfg) override {
        last_cfg_ = cfg;
        opened_with_cfg_count_.fetch_add(1, std::memory_order_relaxed);
        // NOTE: `opened_count_` is NOT bumped here — the two overloads
        // are counted separately (`plugin_open_count_` tracks the
        // IPlugin lifecycle call) so a test can tell "the engine armed
        // the plugin" from "the engine only stored the config".
    }

    void send(plugins::BufferView /*data*/) override {
        // No-op: the test does not exercise send().  A real
        // SctpDataChannel would queue the payload into usrsctp here.
    }

    void set_on_message(plugins::DataMessageCallback cb) noexcept override {
        // Capture the callback so the test can verify the engine
        // forwarded its own callback to us.
        const bool was_empty = !static_cast<bool>(on_message_);
        on_message_ = std::move(cb);
        on_message_set_count_.fetch_add(1, std::memory_order_relaxed);
        (void)was_empty;
    }

    void set_on_state(plugins::DataChannelStateCallback cb) noexcept override {
        const bool was_empty = !static_cast<bool>(on_state_);
        on_state_ = std::move(cb);
        on_state_set_count_.fetch_add(1, std::memory_order_relaxed);
        (void)was_empty;
    }

    // v1.2.0, WP-C1: the additive identity-carrying callback.  The
    // stub records it so the test can drive an inbound message and
    // observe the identity the engine assigns.
    void set_on_message_ex(
        plugins::DataChannelMessageCallback cb) noexcept override {
        const bool was_empty = !static_cast<bool>(on_message_ex_);
        on_message_ex_ = std::move(cb);
        on_message_ex_set_count_.fetch_add(1, std::memory_order_relaxed);
        (void)was_empty;
    }

    // ---- Test introspection ----
    std::atomic<int> opened_count_{0};
    std::atomic<int> closed_count_{0};
    std::atomic<int> opened_with_cfg_count_{0};
    /** Number of IPlugin::open() calls (the lifecycle arming).  A
     *  DataChannel is unusable without it — `send()` returns
     *  kErrNotReady and no inbound message ever arrives. */
    std::atomic<int> plugin_open_count_{0};
    std::atomic<int> on_message_set_count_{0};
    std::atomic<int> on_state_set_count_{0};
    std::atomic<int> on_message_ex_set_count_{0};
    plugins::DataChannelConfig last_cfg_{};
    plugins::DataMessageCallback on_message_{};
    plugins::DataChannelStateCallback on_state_{};
    plugins::DataChannelMessageCallback on_message_ex_{};
};

// =====================================================================
// StubDataChannelFactory — typed factory contract that hands out
// StubDataChannel instances.  Counts `create()` invocations so the
// test can assert the factory was the one the engine resolved.
// =====================================================================
class StubDataChannelFactory final : public plugins::IDataChannelFactory {
public:
    StubDataChannelFactory() = default;
    ~StubDataChannelFactory() override = default;

    std::string_view id()          const noexcept override { return "stub_dc"; }
    std::string_view display_name() const noexcept override {
        return "Test stub DataChannel (no real SCTP)";
    }

    plugins::IDataChannel* create() const override {
        // `create()` is const per the IDataChannelFactory seam, but
        // `fetch_add` mutates the counter — declare the field `mutable`
        // so the const method can advance it.  The same pattern is used
        // for `last_instance_` (mutable via the static cast below) and
        // is documented in MSVC C2663 reproducer notes — the standard
        // allows mutable on members of any non-reference, non-const-
        // qualified type.
        create_count_.fetch_add(1, std::memory_order_relaxed);
        // `last_instance_` is a static std::atomic<StubDataChannel*>;
        // static members are accessible from const methods (they're
        // not part of the object's logical state).  The test inspects
        // it after engine.create_data_channel() returns to verify the
        // factory's create() was actually invoked.
        auto* instance = new StubDataChannel();
        last_instance_.store(instance, std::memory_order_relaxed);
        // v1.2.0, WP-C1: keep every instance so a test can address two
        // distinct channels (the engine's per-channel identity).
        created_.push_back(instance);
        return instance;
    }

    // We return the latest instance so the test can inspect its
    // callback-counting fields without keeping a side-table.
    static std::atomic<StubDataChannel*> last_instance_;

    // Every instance handed out, in creation order (v1.2.0, WP-C1).
    static std::vector<StubDataChannel*> created_;

    mutable std::atomic<int> create_count_{0};
};

// Static member definitions.
std::atomic<StubDataChannel*> StubDataChannelFactory::last_instance_ = nullptr;
std::vector<StubDataChannel*> StubDataChannelFactory::created_{};

// =====================================================================
// Helper: register the StubDataChannelFactory under id="stub_dc" and
// return the factory pointer (so tests can inspect the create_count_).
// Idempotent: re-registering under the same id is last-writer-wins per
// TypedRegistry::register_one (matches Slice 8 contract).
// =====================================================================
static StubDataChannelFactory* register_stub_factory_once() {
    static StubDataChannelFactory* s_factory = nullptr;
    if (!s_factory) {
        // Lookup-then-create: don't double-instantiate the static across
        // multiple SetUp() calls.  s_factory is a Meyer's-singleton-like
        // pointer; the factory itself owns its atomic counters.
        s_factory = new StubDataChannelFactory();
    }
    // Always (re-)register: the registry slot is last-writer-wins, and
    // the "ReRegisterSameIdIsLastWriterWins" test deliberately parks a
    // *stack-local* factory under the same id — which would dangle
    // once that test returns.  Re-registering the long-lived stub
    // factory keeps every later test in the suite well-defined.
    PluginRegistry::instance().register_datachannel(s_factory->id(), s_factory);
    return s_factory;
}

// =====================================================================
// Test fixture — registers all default plugins once for the suite.
// =====================================================================
class DataChannelEngine : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        // Same as the engine's internal call in open().  Idempotent.
        nimrtc::core::register_all_default_plugins();
        // Register our stub factory under a /mock/ id (production code
        // never registers such an id).  Done in SetUpTestSuite so every
        // test gets the same baseline.
        register_stub_factory_once();
    }

    void SetUp() override {
        // The registry slot is last-writer-wins and the
        // "ReRegisterSameIdIsLastWriterWins" test parks a *stack-local*
        // factory under the same id.  Re-register the long-lived stub
        // factory before every test so no test observes a dangling
        // factory pointer.
        register_stub_factory_once();
    }

    static EngineConfig make_test_config() {
        EngineConfig cfg;
        cfg.stun_server_host       = "";
        cfg.local_port_range_begin = 52000;
        cfg.local_port_range_end   = 52100;
        // v0.15.0: EngineConfig defaults datachannel_backend to
        // "web_transport" (product route), which ignores datachannel_name.
        // These legacy SCTP-path tests pin the backend to "" so the
        // legacy datachannel_name dispatch stays authoritative
        // (stub_dc / unknown-id / disabled cases below).
        cfg.datachannel_backend = "";

        // Use the "shell" transport strategy (Slice 8 sentinel in
        // engine.hpp) so engine.open() does NOT actually try to bind
        // ICE UDP sockets. Reasons:
        //   1. These tests cover the engine's DataChannel plumbing
        //      (factory lookup, callback forwarding, disabled-id
        //      paths), not the transport stack — they have no need
        //      for a real ICE candidate gathering or DTLS handshake.
        //   2. On Windows runners (GitHub Actions / low-integrity
        //      processes) libjuice's UDP bind fails with WSAEACCES
        //      (errno 10013) for any local port range, including
        //      [52000, 52100] here and any other typical range. With
        //      the real "webrtc-classic" stack this surfaces as a
        //      `transport open failed rc=8191` from libjuice's agent,
        //      and engine.open() returns non-zero — silently
        //      breaking every test in this fixture even though no
        //      DataChannel code path was at fault.
        //   3. The shell stack's ice_t_->open() returns kOk without
        //      touching the network, so the engine reaches the
        //      open() success state and the DataChannel dispatch
        //      logic actually executes — which is the whole point.
        cfg.transport_strategy_name = "shell";

        return cfg;
    }
};

// =====================================================================
// Test 1: `register_datachannel(id, factory*)` resolves via the typed
// registry slot.  Mirrors the Slice 8 contract used by DTLS / SCTP /
// raw_udp / transport-stack.
// =====================================================================
TEST_F(DataChannelEngine, registry_datachannel_slot_resolves_by_id) {
    const auto* factory =
        PluginRegistry::instance().get_datachannel("stub_dc");
    ASSERT_NE(factory, nullptr)
        << "PAL Slice 8-style: datachannel slot must resolve factories "
           "by id (the canonical lookup NimRTCEngine uses)";
    EXPECT_EQ(factory->id(), "stub_dc");
    EXPECT_FALSE(factory->display_name().empty());

    auto ids = PluginRegistry::instance().list_datachannels();
    bool found = false;
    for (auto id : ids) {
        if (id == "stub_dc") { found = true; break; }
    }
    EXPECT_TRUE(found)
        << "list_datachannels() should include the stub_dc factory "
           "(Slot is populated by register_datachannel)";
}

// =====================================================================
// Test 2: `cfg.datachannel_name = "nope"` (un-registered id) makes
// `create_data_channel()` return nullptr.  The engine does NOT
// silently fall back to "sctp" (that would mask integrator typos
// the same way the DTLS seam doesn't silently fall back to "wolfssl").
// =====================================================================
TEST_F(DataChannelEngine, EngineReturnsNullForUnknownDatachannelId) {
    EngineConfig cfg = make_test_config();
    cfg.datachannel_name = "nope_no_such_factory";

    NimRTCEngine engine(cfg);

    // Capture the error callback so we can assert the engine surfaces
    // a diagnostic — kEngineInternal matches the pre-Slice-8 contract
    // for "factory id not found".
    std::atomic<std::uint32_t> err_code{0};
    std::atomic<bool>          err_fired{false};
    engine.set_on_error(
        [&](std::uint32_t err, std::string_view /*msg*/) {
            err_code   = err;
            err_fired  = true;
        });

    // Engine must open successfully (datachannel_name is just a name
    // lookup; missing factories are non-fatal at open() time).
    ASSERT_EQ(engine.open(), 0u)
        << "engine.open() should succeed even when datachannel_name is "
           "unknown (the failure surfaces at create_data_channel())";

    // create_data_channel() returns nullptr for the unknown id.
    auto ch = engine.create_data_channel("test");
    EXPECT_EQ(ch, nullptr)
        << "create_data_channel() must return nullptr when the factory "
           "id is not registered";

    // The error callback should have surfaced the diagnostic.
    EXPECT_TRUE(err_fired.load())
        << "engine.on_error_ should fire when create_data_channel() "
           "cannot resolve the factory id";
    EXPECT_EQ(err_code.load(), nimrtc::core::kEngineInternal)
        << "kEngineInternal is the code the engine uses for "
           "datachannel-plugin-not-found (matches the DTLS seam "
           "contract from test_dtls_seam)";

    engine.close();
}

// =====================================================================
// Test 3: with the registered stub_dc factory, set_on_data_message
// and set_on_data_state callbacks are forwarded to the IDataChannel
// instance created by `create_data_channel()` BEFORE its open() call.
//
// We verify the forwarding by:
//   (a) Setting atomic-int callbacks on the engine via set_on_data_*
//   (b) Creating a channel
//   (c) Inspecting the StubDataChannel's on_message_set_count_ /
//       on_state_set_count_ — they should be 1 (the engine called
//       set_on_message / set_on_state exactly once before open())
//   (d) Verifying that the engine's own callback was the one
//       forwarded: we set a callback that bumps a counter, then
//       invoke the captured callback (via the StubDataChannel's
//       stored on_message_ / on_state_) and confirm the counter
//       advances.  This is the strongest assertion that the engine's
//       callback, not a default-constructed std::function, was
//       installed.
// =====================================================================
TEST_F(DataChannelEngine, EngineSurfacesDataChannelCallbacks) {
    EngineConfig cfg = make_test_config();
    cfg.datachannel_name = "stub_dc";   // the test-registered factory

    // Counters that the user-supplied callbacks bump.
    std::atomic<int> msg_counter{0};
    std::atomic<int> state_counter{0};

    NimRTCEngine engine(cfg);
    engine.set_on_data_message(
        [&msg_counter](plugins::BufferView /*msg*/) {
            msg_counter.fetch_add(1, std::memory_order_relaxed);
        });
    engine.set_on_data_state(
        [&state_counter](const char* /*state*/) {
            state_counter.fetch_add(1, std::memory_order_relaxed);
        });

    ASSERT_EQ(engine.open(), 0u);

    auto ch = engine.create_data_channel("test-channel");
    ASSERT_NE(ch, nullptr)
        << "create_data_channel() must return a non-null IDataChannel "
           "when cfg.datachannel_name resolves to a registered factory";

    // The engine stored a unique_ptr<IDataChannel> on its side and
    // returned nullptr; we can't directly inspect engine-side state
    // (the IDataChannel lives behind a unique_ptr that's moved out of
    // the factory).  But we CAN inspect the last-created instance via
    // the factory's static last_instance_ pointer — set in
    // StubDataChannelFactory::create().
    auto* stub = StubDataChannelFactory::last_instance_.load();
    ASSERT_NE(stub, nullptr);
    EXPECT_GE(stub->opened_with_cfg_count_.load(), 1)
        << "StubDataChannel::open(cfg) should fire when "
           "engine.create_data_channel() runs";
    EXPECT_EQ(stub->plugin_open_count_.load(), 1)
        << "the engine must call IPlugin::open() on the channel: without "
           "it the channel is a shell — every send() fails with "
           "kErrNotReady and no inbound message ever reaches the "
           "application";
    EXPECT_EQ(stub->on_message_set_count_.load(), 1)
        << "engine must call set_on_message exactly once before "
           "open(cfg) (per IDataChannel contract)";
    EXPECT_EQ(stub->on_state_set_count_.load(), 1)
        << "engine must call set_on_state exactly once before open(cfg)";

    // Confirm the forwarded callback IS the engine-supplied one (not a
    // default-constructed std::function).  Invoke the captured
    // callback directly and check the counter advances.
    EXPECT_TRUE(static_cast<bool>(stub->on_message_))
        << "StubDataChannel::on_message_ must be set";
    EXPECT_TRUE(static_cast<bool>(stub->on_state_))
        << "StubDataChannel::on_state_ must be set";

    // Simulate an inbound message — invokes the engine's callback.
    const std::uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    plugins::BufferView bv{payload, sizeof(payload)};
    stub->on_message_(bv);
    EXPECT_EQ(msg_counter.load(), 1)
        << "Invoking the captured callback must bump the engine's "
           "counter — proves the engine's set_on_data_message cb "
           "(not a default-constructed one) was forwarded";

    // Simulate a state transition.
    stub->on_state_("open");
    EXPECT_EQ(state_counter.load(), 1)
        << "Invoking the captured callback must bump the engine's "
           "counter — proves the engine's set_on_data_state cb was "
           "forwarded";

    // Sanity: last_cfg_ has the label we passed in.
    EXPECT_EQ(stub->last_cfg_.label, "test-channel")
        << "StubDataChannel::open(cfg) must receive the label the "
           "caller passed to create_data_channel()";

    // The engine's own state should still be open.  close() must
    // cleanly tear down the channel-side bookkeeping (no crash, no
    // UB on the StubDataChannel's std::function moves).
    engine.close();
}

// =====================================================================
// Test 4: `cfg.datachannel_backend = ""` (DataChannel disabled) makes
// `create_data_channel()` return nullptr even when the "sctp" (or any
// other) factory is registered.  This is the explicit-disable path —
// mirrors how `cfg.bwe_name = ""` disables BWE.
//
// v1.2.0 contract (engine_config.md §2.6.3): `datachannel_backend = ""`
// disables the data plane.  The legacy `datachannel_name = ""` spelling
// disables it too, but only when `datachannel_backend` is empty as well
// — in the default `"web_transport"` family `datachannel_name` is
// ignored (see the `backend == "web_transport"` branch in
// `init_datachannel_registry()`, engine_transport.cpp).
//
// This test clears BOTH fields so the assertion holds regardless of
// which one the backend dispatch consults; clearing `datachannel_name`
// alone is sufficient, and clearing `datachannel_backend` alone is
// also sufficient (the empty-backend branch resolves to an empty
// factory id and skips resolution entirely).  Setting both is
// belt-and-suspenders and documents the intended caller-facing
// spelling of "disable".
// =====================================================================
TEST_F(DataChannelEngine, DisabledDatachannelNameReturnsNull) {
    EngineConfig cfg = make_test_config();
    cfg.datachannel_backend = "";  // explicit disable (rc.1 API)
    cfg.datachannel_name    = "";  // also clear (belt-and-suspenders)

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u)
        << "engine.open() should succeed when DataChannel is disabled "
           "(datachannel_backend is a per-channel knob, not a hard "
           "dependency)";

    auto ch = engine.create_data_channel("test");
    EXPECT_EQ(ch, nullptr)
        << "create_data_channel() must return nullptr when "
           "cfg.datachannel_name (and cfg.datachannel_backend) are "
           "empty (DataChannel disabled per v0.14.0-rc.1 contract in "
           "engine_config.md §2.6.3)";

    engine.close();
}

// =====================================================================
// Test 5: re-registering under the SAME factory id is last-writer-wins
// (TypedRegistry::register_one contract from Slice 8).  Verifies the
// datachannel slot follows the same contract as DTLS / SCTP / etc.
// =====================================================================
TEST_F(DataChannelEngine, ReRegisterSameIdIsLastWriterWins) {
    auto* first = register_stub_factory_once();

    // Register a second factory under the SAME id ("stp
    // TypedRegistry semantics).  We use a fresh factory (different
    // atomic counters) to prove the second one overwrites the first.
    StubDataChannelFactory second_factory;
    PluginRegistry::instance().register_datachannel(
        first->id(), &second_factory);

    const auto* resolved =
        PluginRegistry::instance().get_datachannel(first->id());
    ASSERT_NE(resolved, nullptr);
    EXPECT_EQ(resolved, &second_factory)
        << "register_datachannel() must overwrite previous factory on "
           "the same id (last-writer-wins, matches Slice 8 contract)";
}

// =====================================================================
// Test 6 (v1.2.0, WP-C1): the engine assigns a distinct channel
// identity to every channel it creates.
//
// The plain `set_on_data_message()` callback carries no channel
// identity — a host that opens two channels cannot tell which one a
// message arrived on.  The additive `set_on_data_message_ex()`
// callback carries the label and the engine-assigned `channel_id`.
//
// We create two channels ("a" and "b"), then drive an inbound
// message on each stub channel and assert that the identity the
// engine's callback receives names the right channel.
// =====================================================================
TEST_F(DataChannelEngine, ChannelIdentityIsReportedPerChannel) {
    EngineConfig cfg = make_test_config();
    cfg.datachannel_name = "stub_dc";

    NimRTCEngine engine(cfg);

    // The identities the engine reports, in arrival order.
    std::vector<std::string> labels;
    std::vector<std::uint32_t> ids;
    engine.set_on_data_message_ex(
        [&](const plugins::DataChannelMessage& msg) {
            labels.emplace_back(msg.label);
            ids.push_back(msg.channel_id);
        });

    ASSERT_EQ(engine.open(), 0u);

    const std::size_t before = StubDataChannelFactory::created_.size();
    auto ch_a = engine.create_data_channel("a");
    auto ch_b = engine.create_data_channel("b");
    ASSERT_NE(ch_a, nullptr);
    ASSERT_NE(ch_b, nullptr);
    ASSERT_EQ(StubDataChannelFactory::created_.size(), before + 2)
        << "each create_data_channel() must produce its own channel "
           "instance";

    auto* stub_a = StubDataChannelFactory::created_[before];
    auto* stub_b = StubDataChannelFactory::created_[before + 1];
    ASSERT_NE(stub_a, nullptr);
    ASSERT_NE(stub_b, nullptr);
    ASSERT_EQ(stub_a->on_message_ex_set_count_.load(), 1)
        << "the engine must install the identity-carrying callback on "
           "every channel it creates";
    ASSERT_EQ(stub_b->on_message_ex_set_count_.load(), 1);

    // Drive an inbound message on channel "b" only.
    const std::uint8_t payload[] = {0xAA};
    plugins::DataChannelMessage inbound;
    inbound.label = "b";
    inbound.channel_id = 0;   // the backend does not know the id
    inbound.data = plugins::BufferView{payload, sizeof(payload)};
    stub_b->on_message_ex_(inbound);

    ASSERT_EQ(labels.size(), 1u);
    EXPECT_EQ(labels[0], "b")
        << "the identity must name the channel the message arrived on";
    EXPECT_NE(ids[0], 0u)
        << "the engine must assign a non-zero channel id";

    // Drive an inbound message on channel "a".
    plugins::DataChannelMessage inbound_a;
    inbound_a.label = "a";
    inbound_a.channel_id = 0;
    inbound_a.data = plugins::BufferView{payload, sizeof(payload)};
    stub_a->on_message_ex_(inbound_a);

    ASSERT_EQ(labels.size(), 2u);
    EXPECT_EQ(labels[1], "a");
    EXPECT_NE(ids[0], ids[1])
        << "two channels must carry two distinct channel ids";

    engine.close();
}

// =====================================================================
// Test 7 (v1.2.0, WP-E1 / WP-B): `EngineConfig::datachannel_priority`
// and `EngineConfig::datachannel_framing` reach the backend's
// `DataChannelConfig`.
//
// Before v1.2.0 the engine hard-coded `priority = 128` and offered no
// way to change it (the "priority field zero references" gap), and the
// framing mode did not exist at all.
// =====================================================================
TEST_F(DataChannelEngine, PriorityAndFramingReachTheBackend) {
    EngineConfig cfg = make_test_config();
    cfg.datachannel_name  = "stub_dc";
    cfg.datachannel_priority = 17;
    cfg.datachannel_framing  = plugins::DataChannelFraming::kFramed;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    auto ch = engine.create_data_channel("ctl");
    ASSERT_NE(ch, nullptr);

    auto* stub = StubDataChannelFactory::last_instance_.load();
    ASSERT_NE(stub, nullptr);
    EXPECT_EQ(stub->last_cfg_.priority, 17)
        << "the engine must forward the host's priority to the backend "
           "(v1.2.0, WP-E1) — it used to hard-code 128";
    EXPECT_EQ(stub->last_cfg_.framing, plugins::DataChannelFraming::kFramed)
        << "the engine must forward the host's framing mode to the "
           "backend (v1.2.0, WP-B)";

    engine.close();
}

// =====================================================================
// Test 8 (v1.2.0, WP-B): the default framing mode is `kRaw` — the
// byte-stream behaviour every existing deployment relies on.  The
// framed mode is opt-in.
// =====================================================================
TEST_F(DataChannelEngine, DefaultFramingIsRaw) {
    EngineConfig cfg = make_test_config();
    cfg.datachannel_name = "stub_dc";

    // The engine-level default.
    EXPECT_EQ(cfg.datachannel_framing, plugins::DataChannelFraming::kRaw);
    EXPECT_EQ(plugins::DataChannelConfig{}.framing,
              plugins::DataChannelFraming::kRaw);

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    auto ch = engine.create_data_channel("legacy");
    ASSERT_NE(ch, nullptr);

    auto* stub = StubDataChannelFactory::last_instance_.load();
    ASSERT_NE(stub, nullptr);
    EXPECT_EQ(stub->last_cfg_.framing, plugins::DataChannelFraming::kRaw)
        << "the default must preserve the pre-v1.2.0 byte-stream "
           "behaviour (the framed mode is opt-in)";

    engine.close();
}

// Test 6: the engine arms the channel with IPlugin::open() (DC-1).
//
// The `IDataChannel::open(cfg)` overload only caches the config —
// without the `IPlugin::open()` call the returned channel is a shell:
// every `send()` fails with kErrNotReady and no inbound message ever
// reaches the application.
//
// Test 3 already asserts that `IPlugin::open()` was called (via the
// stub's `plugin_open_count_`).  This test pins the *failure* half:
// when the plugin refuses to open, the engine must report the error
// and hand back nullptr instead of a channel that silently fails.
// =====================================================================
namespace {

/** A DataChannel whose IPlugin::open() always fails. */
class FailingOpenDataChannel final : public plugins::IDataChannel {
public:
    plugins::Status open() noexcept override {
        return plugins::kErrNotReady;
    }
    const char* name() const noexcept override {
        return "FailingOpenDataChannel";
    }
    void close() noexcept override {}
    void open(const plugins::DataChannelConfig&) override {}
    void send(plugins::BufferView) override {}
    void set_on_message(plugins::DataMessageCallback) noexcept override {}
    void set_on_state(plugins::DataChannelStateCallback) noexcept override {}
};

class FailingOpenFactory final : public plugins::IDataChannelFactory {
public:
    std::string_view id() const noexcept override {
        return "failing_open_dc";
    }
    std::string_view display_name() const noexcept override {
        return "Test DataChannel whose IPlugin::open() fails";
    }
    plugins::IDataChannel* create() const override {
        return new FailingOpenDataChannel();
    }
};

} // namespace

TEST_F(DataChannelEngine, EngineReportsAFailedPluginOpen) {
    static FailingOpenFactory factory;
    PluginRegistry::instance().register_datachannel(factory.id(), &factory);

    EngineConfig cfg = make_test_config();
    cfg.datachannel_name = "failing_open_dc";

    NimRTCEngine engine(cfg);

    std::atomic<std::uint32_t> err_code{0};
    std::atomic<bool>          err_fired{false};
    engine.set_on_error(
        [&](std::uint32_t err, std::string_view /*msg*/) {
            err_code  = err;
            err_fired = true;
        });

    ASSERT_EQ(engine.open(), 0u);

    auto ch = engine.create_data_channel("test-channel");
    EXPECT_EQ(ch, nullptr)
        << "create_data_channel() must return nullptr when the plugin's "
           "IPlugin::open() fails — a channel that can never send is "
           "worse than no channel at all";
    EXPECT_TRUE(err_fired.load())
        << "the engine must surface the failed open through on_error_";
    EXPECT_EQ(err_code.load(), nimrtc::core::kEngineInternal);

    engine.close();
}

} // anonymous namespace