/**
 * @file tests/test_engine_datachannel_stats.cpp
 * @brief The data-channel statistics surface (plan Track S / S6).
 *
 * The data-channel plane had **no** statistics before S6: a host that
 * opened a "telemetry" channel and a "cmd" channel could not tell
 * whether either was actually moving data.  `ControlChannelStats` does
 * not help — it describes the engine's *own* control channel, not the
 * user-facing channels the host opens.
 *
 * The surface this test pins is `NimRTCEngine::Stats::data_channels`:
 * one entry per channel the host opened, each with its own identity
 * and its own counters.
 *
 * ## The two-sided model
 *
 * The counters come from two different parties, and that is the whole
 * design:
 *
 *   - the **inbound** half (`messages_received` / `bytes_received`) is
 *     the *engine's* account — the engine installs the channel's
 *     callbacks, so it sees every inbound message;
 *   - the **outbound** half (`messages_sent` / `bytes_sent` /
 *     `messages_dropped` / `messages_retransmitted` /
 *     `messages_pending`) is the *backend's* account, asked for
 *     through the additive `plugins::IDataChannel::stats()` seam.
 *     The engine cannot derive those numbers: it only sees the
 *     `send()` call, and what happens to the message afterwards
 *     (queued, retransmitted, dropped when its lifetime expired)
 *     happens inside the backend.
 *
 * A backend that does not implement the seam reports zeroes — and the
 * engine republishes those zeroes rather than inventing a number.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/datachannel.hpp>

namespace {

namespace np = nimrtc::plugins;

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

// ---------------------------------------------------------------------------
// A stub data channel that keeps the counters a real backend keeps.
// ---------------------------------------------------------------------------

class StubStatsChannel final : public np::IDataChannel {
public:
    // ---- IPlugin ---------------------------------------------------------
    const char* name() const noexcept override { return "stub-stats-dc"; }
    np::Status open() noexcept override {
        plugin_open_count_.fetch_add(1, std::memory_order_relaxed);
        return np::kOk;
    }
    void close() noexcept override {}

    // ---- IDataChannel ----------------------------------------------------
    void open(const np::DataChannelConfig& cfg) override { last_cfg_ = cfg; }
    void send(np::BufferView data) override {
        // The backend's own account of what it handed to the transport.
        sent_.fetch_add(1, std::memory_order_relaxed);
        bytes_sent_.fetch_add(data.size(), std::memory_order_relaxed);
    }

    void set_on_message(np::DataMessageCallback cb) noexcept override {
        on_msg_ = std::move(cb);
    }
    void set_on_message_ex(np::DataChannelMessageCallback cb) noexcept override {
        on_msg_ex_ = std::move(cb);
    }
    void set_on_state(np::DataChannelStateCallback cb) noexcept override {
        on_state_ = std::move(cb);
    }

    np::DataChannelStats stats() const noexcept override {
        np::DataChannelStats out;
        out.messages_sent         = sent_.load(std::memory_order_relaxed);
        out.bytes_sent            = bytes_sent_.load(std::memory_order_relaxed);
        out.messages_dropped      = dropped_.load(std::memory_order_relaxed);
        out.messages_retransmitted = retransmitted_.load(std::memory_order_relaxed);
        out.messages_pending      = pending_.load(std::memory_order_relaxed);
        return out;
    }

    // ---- Test introspection ----------------------------------------------
    /** Drive an inbound message into the engine. */
    void deliver_inbound(const std::vector<std::uint8_t>& payload) {
        if (on_msg_) {
            on_msg_(np::BufferView{payload.data(), payload.size()});
        }
        if (on_msg_ex_) {
            np::DataChannelMessage m;
            m.label = last_cfg_.label;
            m.channel_id = 0;
            m.data = np::BufferView{payload.data(), payload.size()};
            on_msg_ex_(m);
        }
    }

    /** Drive a state transition into the engine. */
    void deliver_state(const char* state) {
        if (on_state_) on_state_(state);
    }

    void set_dropped(std::uint64_t n) { dropped_.store(n, std::memory_order_relaxed); }
    void set_retransmitted(std::uint64_t n) { retransmitted_.store(n, std::memory_order_relaxed); }
    void set_pending(std::uint64_t n) { pending_.store(n, std::memory_order_relaxed); }

    std::atomic<int> plugin_open_count_{0};
    np::DataChannelConfig last_cfg_{};

private:
    std::atomic<std::uint64_t> sent_{0};
    std::atomic<std::uint64_t> bytes_sent_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> retransmitted_{0};
    std::atomic<std::uint64_t> pending_{0};

    np::DataMessageCallback on_msg_;
    np::DataChannelMessageCallback on_msg_ex_;
    np::DataChannelStateCallback on_state_;
};

class StubStatsFactory final : public np::IDataChannelFactory {
public:
    std::string_view id() const noexcept override { return "stub-stats-dc"; }
    std::string_view display_name() const noexcept override {
        return "Stub data channel with counters (test)";
    }
    np::IDataChannel* create() const override {
        auto* instance = new StubStatsChannel();
        created_.push_back(instance);
        return instance;
    }

    // Every instance handed out, in creation order.
    static std::vector<StubStatsChannel*> created_;
};

std::vector<StubStatsChannel*> StubStatsFactory::created_{};

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class EngineDataChannelStats : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
        static StubStatsFactory factory;
        PluginRegistry::instance().register_datachannel(factory.id(), &factory);
    }

    void SetUp() override {
        StubStatsFactory::created_.clear();
        // Re-register every test: the registry slot is last-writer-wins
        // and another suite may have parked a factory under the same id.
        static StubStatsFactory factory;
        PluginRegistry::instance().register_datachannel(factory.id(), &factory);
    }

    EngineConfig make_config() {
        EngineConfig cfg{};
        // The legacy dispatch: an empty backend with an explicit name
        // keeps the legacy `datachannel_name` selector authoritative.
        cfg.datachannel_backend = "";
        cfg.datachannel_name    = "stub-stats-dc";
        cfg.bwe_name            = "";
        cfg.scheduler_config.enabled = false;
        return cfg;
    }
};

// ============================================================================
// 1. The surface exists and is empty when the host opened no channel
// ============================================================================

TEST_F(EngineDataChannelStats, TheListIsEmptyWhenNoChannelWasOpened) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto s = engine.stats();
    EXPECT_TRUE(s.data_channels.empty())
        << "a host that never opened a data channel must not see any "
           "data-channel entry";

    engine.close();
}

// ============================================================================
// 2. One entry per channel, with its identity
// ============================================================================

TEST_F(EngineDataChannelStats, AnOpenedChannelAppearsWithItsIdentity) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    auto ch = engine.create_data_channel("telemetry");
    ASSERT_NE(ch, nullptr);

    const auto s = engine.stats();
    ASSERT_EQ(s.data_channels.size(), 1u)
        << "the host opened exactly one channel, so the statistics must "
           "carry exactly one entry";

    const auto& e = s.data_channels[0];
    EXPECT_EQ(e.label, "telemetry")
        << "the entry must name the channel the host opened";
    EXPECT_NE(e.channel_id, 0u)
        << "the engine assigns a non-zero channel id";
    EXPECT_EQ(e.reliability, "reliable-ordered")
        << "the entry must report the mode the channel was created with, "
           "as a name the host can read";

    engine.close();
}

TEST_F(EngineDataChannelStats, TwoChannelsGetTwoEntriesInCreationOrder) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    auto a = engine.create_data_channel("a");
    auto b = engine.create_data_channel("b");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    const auto s = engine.stats();
    ASSERT_EQ(s.data_channels.size(), 2u);
    EXPECT_EQ(s.data_channels[0].label, "a");
    EXPECT_EQ(s.data_channels[1].label, "b");
    EXPECT_NE(s.data_channels[0].channel_id, s.data_channels[1].channel_id)
        << "two channels must carry two distinct ids";

    // The order is the *creation* order and it is stable across
    // readings: a host that diffs two readings must not see the
    // entries swap places.
    const auto s2 = engine.stats();
    ASSERT_EQ(s2.data_channels.size(), 2u);
    EXPECT_EQ(s2.data_channels[0].label, "a");
    EXPECT_EQ(s2.data_channels[1].label, "b");

    engine.close();
}

TEST_F(EngineDataChannelStats, TheReliabilityModeIsPublishedByName) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    auto ch = engine.create_data_channel(
        "telemetry", np::DataChannelReliability::kPartialReliableTTL);
    ASSERT_NE(ch, nullptr);

    const auto s = engine.stats();
    ASSERT_EQ(s.data_channels.size(), 1u);
    EXPECT_EQ(s.data_channels[0].reliability, "partial-reliable-ttl")
        << "a host that wants to know *why* a message was dropped reads "
           "this field: the two partial modes are the only ones that drop";

    engine.close();
}

// ============================================================================
// 3. The inbound half — the engine's own account
// ============================================================================

TEST_F(EngineDataChannelStats, InboundMessagesAreCountedByTheEngine) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    auto ch = engine.create_data_channel("telemetry");
    ASSERT_NE(ch, nullptr);
    ASSERT_EQ(StubStatsFactory::created_.size(), 1u);
    auto* stub = StubStatsFactory::created_[0];

    const std::vector<std::uint8_t> p1(10, 0xAA);
    const std::vector<std::uint8_t> p2(20, 0xBB);
    stub->deliver_inbound(p1);
    stub->deliver_inbound(p2);

    const auto s = engine.stats();
    ASSERT_EQ(s.data_channels.size(), 1u);
    EXPECT_EQ(s.data_channels[0].messages_received, 2u);
    EXPECT_EQ(s.data_channels[0].bytes_received, 30u)
        << "the byte counter must add up the payload sizes";

    engine.close();
}

TEST_F(EngineDataChannelStats, TheInboundCountersGrowMonotonically) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    auto ch = engine.create_data_channel("telemetry");
    ASSERT_NE(ch, nullptr);
    auto* stub = StubStatsFactory::created_[0];

    const std::vector<std::uint8_t> p(8, 0x01);
    stub->deliver_inbound(p);
    const auto first = engine.stats();
    stub->deliver_inbound(p);
    const auto second = engine.stats();

    ASSERT_EQ(first.data_channels.size(), 1u);
    ASSERT_EQ(second.data_channels.size(), 1u);
    EXPECT_EQ(first.data_channels[0].messages_received, 1u);
    EXPECT_EQ(second.data_channels[0].messages_received, 2u);

    engine.close();
}

TEST_F(EngineDataChannelStats, TheInboundCountersAreCountedPerChannel) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    auto a = engine.create_data_channel("a");
    auto b = engine.create_data_channel("b");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_EQ(StubStatsFactory::created_.size(), 2u);

    const std::vector<std::uint8_t> p(4, 0x02);
    StubStatsFactory::created_[0]->deliver_inbound(p);
    StubStatsFactory::created_[0]->deliver_inbound(p);
    StubStatsFactory::created_[1]->deliver_inbound(p);

    const auto s = engine.stats();
    ASSERT_EQ(s.data_channels.size(), 2u);
    EXPECT_EQ(s.data_channels[0].messages_received, 2u)
        << "the counters must not bleed from one channel into the next";
    EXPECT_EQ(s.data_channels[1].messages_received, 1u);

    engine.close();
}

// ============================================================================
// 4. The outbound half — the backend's own account
// ============================================================================

TEST_F(EngineDataChannelStats, TheBackendCountersAreRepublished) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    auto ch = engine.create_data_channel("telemetry");
    ASSERT_NE(ch, nullptr);

    // The backend is the only party that knows what it did with the
    // messages, so the engine asks it through the additive
    // `plugins::IDataChannel::stats()` seam.
    auto* stub = StubStatsFactory::created_[0];
    stub->set_dropped(3);
    stub->set_retransmitted(2);
    stub->set_pending(5);

    const std::vector<std::uint8_t> payload(64, 0x03);
    ch->send(np::BufferView{payload.data(), payload.size()});
    ch->send(np::BufferView{payload.data(), payload.size()});

    const auto s = engine.stats();
    ASSERT_EQ(s.data_channels.size(), 1u);
    const auto& e = s.data_channels[0];
    EXPECT_EQ(e.messages_sent, 2u);
    EXPECT_EQ(e.bytes_sent, 128u);
    EXPECT_EQ(e.messages_dropped, 3u)
        << "the drop count is the backend's account — the engine cannot "
           "derive it";
    EXPECT_EQ(e.messages_retransmitted, 2u);
    EXPECT_EQ(e.messages_pending, 5u);

    engine.close();
}

// ============================================================================
// 5. The channel's state
// ============================================================================

TEST_F(EngineDataChannelStats, TheLastReportedStateIsPublished) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    auto ch = engine.create_data_channel("telemetry");
    ASSERT_NE(ch, nullptr);

    {
        const auto s = engine.stats();
        ASSERT_EQ(s.data_channels.size(), 1u);
        EXPECT_TRUE(s.data_channels[0].state.empty())
            << "the backend has not reported a state yet, so the entry "
               "must not invent one";
    }

    StubStatsFactory::created_[0]->deliver_state("open");

    {
        const auto s = engine.stats();
        ASSERT_EQ(s.data_channels.size(), 1u);
        EXPECT_EQ(s.data_channels[0].state, "open");
    }

    StubStatsFactory::created_[0]->deliver_state("closed");

    {
        const auto s = engine.stats();
        ASSERT_EQ(s.data_channels.size(), 1u);
        EXPECT_EQ(s.data_channels[0].state, "closed")
            << "the entry must carry the *last* state the backend "
               "reported, not the first one";
    }

    engine.close();
}

// ============================================================================
// 6. The host's own callbacks still reach the host
// ============================================================================

TEST_F(EngineDataChannelStats, TheHostCallbackStillFires) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    std::atomic<int> messages{0};
    engine.set_on_data_message(
        [&messages](np::BufferView) {
            messages.fetch_add(1, std::memory_order_relaxed);
        });

    auto ch = engine.create_data_channel("telemetry");
    ASSERT_NE(ch, nullptr);

    const std::vector<std::uint8_t> p(4, 0x04);
    StubStatsFactory::created_[0]->deliver_inbound(p);

    EXPECT_EQ(messages.load(), 1)
        << "the proxy must forward the inbound message to the host's "
           "callback — the statistics must not swallow the traffic";

    const auto s = engine.stats();
    ASSERT_EQ(s.data_channels.size(), 1u);
    EXPECT_EQ(s.data_channels[0].messages_received, 1u)
        << "and the engine must still count it";

    engine.close();
}

TEST_F(EngineDataChannelStats, TheHostIdentityCallbackStillFires) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    std::vector<std::uint32_t> ids;
    engine.set_on_data_message_ex(
        [&ids](const np::DataChannelMessage& msg) {
            ids.push_back(msg.channel_id);
        });

    auto ch = engine.create_data_channel("telemetry");
    ASSERT_NE(ch, nullptr);

    const std::vector<std::uint8_t> p(4, 0x05);
    StubStatsFactory::created_[0]->deliver_inbound(p);

    ASSERT_EQ(ids.size(), 1u);
    EXPECT_NE(ids[0], 0u)
        << "the engine must still assign the channel id";

    const auto s = engine.stats();
    ASSERT_EQ(s.data_channels.size(), 1u);
    EXPECT_EQ(s.data_channels[0].channel_id, ids[0])
        << "the statistics entry and the identity callback must name the "
           "same channel";

    engine.close();
}

// ============================================================================
// 7. The default seam is additive — a backend that does not implement it
//    keeps compiling and reports zeroes
// ============================================================================

namespace {

/** A backend that does not implement the statistics seam at all. */
class MinimalChannel final : public np::IDataChannel {
public:
    const char* name() const noexcept override { return "minimal-dc"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}
    void open(const np::DataChannelConfig&) override {}
    void send(np::BufferView) override {}
    void set_on_message(np::DataMessageCallback cb) noexcept override {
        on_msg_ = std::move(cb);
    }
    void set_on_message_ex(np::DataChannelMessageCallback cb) noexcept override {
        on_msg_ex_ = std::move(cb);
    }
    void set_on_state(np::DataChannelStateCallback cb) noexcept override {
        on_state_ = std::move(cb);
    }
    // NOTE: `stats()` is *not* overridden — the seam's default
    // implementation returns an all-zero struct.

    void deliver(const std::vector<std::uint8_t>& payload) {
        if (on_msg_) {
            on_msg_(np::BufferView{payload.data(), payload.size()});
        }
    }

private:
    np::DataMessageCallback on_msg_;
    np::DataChannelMessageCallback on_msg_ex_;
    np::DataChannelStateCallback on_state_;
};

class MinimalFactory final : public np::IDataChannelFactory {
public:
    std::string_view id() const noexcept override { return "minimal-dc"; }
    std::string_view display_name() const noexcept override {
        return "Minimal data channel (no statistics seam)";
    }
    np::IDataChannel* create() const override {
        auto* instance = new MinimalChannel();
        created_.push_back(instance);
        return instance;
    }
    static std::vector<MinimalChannel*> created_;
};

std::vector<MinimalChannel*> MinimalFactory::created_{};

}  // namespace

TEST_F(EngineDataChannelStats, ABackendWithoutTheSeamReportsZeroes) {
    static MinimalFactory factory;
    PluginRegistry::instance().register_datachannel(factory.id(), &factory);

    EngineConfig cfg = make_config();
    cfg.datachannel_name = "minimal-dc";
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    auto ch = engine.create_data_channel("plain");
    ASSERT_NE(ch, nullptr);

    const std::vector<std::uint8_t> p(7, 0x06);
    MinimalFactory::created_.back()->deliver(p);

    const auto s = engine.stats();
    ASSERT_EQ(s.data_channels.size(), 1u);
    const auto& e = s.data_channels[0];

    // The inbound half still works — it is the engine's own account.
    EXPECT_EQ(e.messages_received, 1u);
    EXPECT_EQ(e.bytes_received, 7u);

    // The outbound half is zero: the backend does not track it, and
    // the engine republishes the zero rather than inventing a number.
    EXPECT_EQ(e.messages_sent, 0u);
    EXPECT_EQ(e.bytes_sent, 0u);
    EXPECT_EQ(e.messages_dropped, 0u);
    EXPECT_EQ(e.messages_retransmitted, 0u);
    EXPECT_EQ(e.messages_pending, 0u);

    engine.close();
}

}  // namespace
