/**
 * @file src/modules/datachannel/tests/test_datachannel.cpp
 * @brief Unit tests for IDataChannel interface and SimpleDataChannelFactory.
 *
 * Verifies:
 *   1. IDataChannel interface can be implemented (stub concrete type).
 *   2. SimpleDataChannelFactory<T> correctly reports id / display_name.
 *   3. SimpleDataChannelFactory<T>::create() returns a new T instance.
 *   4. DataChannelConfig fields are all accessible.
 *   5. Callbacks can be set on a channel instance.
 *   6. DataChannelReliability enum has all expected variants.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nimrtc/plugins/datachannel.hpp>

namespace {

// ---------------------------------------------------------------------------
// Stub concrete IDataChannel for testing (not part of the production API).
// Lives only in this test translation unit.
// ---------------------------------------------------------------------------

class StubDataChannel final : public nimrtc::plugins::IDataChannel {
public:
    explicit StubDataChannel() = default;

    // IPlugin
    const char* name() const noexcept override { return "StubDataChannel"; }

    nimrtc::plugins::Status open() noexcept override {
        if (state_ != kClosed) return nimrtc::plugins::kErrNotReady;
        state_ = kOpen;
        return nimrtc::plugins::kOk;
    }

    void close() noexcept override { state_ = kClosed; }

    // IDataChannel
    void open(const nimrtc::plugins::DataChannelConfig& cfg) override {
        cfg_ = cfg;
        state_ = kOpen;
    }

    void send(nimrtc::plugins::BufferView data) override {
        if (state_ != kOpen) return;
        sent_.insert(sent_.end(), data.begin(), data.end());
    }

    void set_on_message(nimrtc::plugins::DataMessageCallback cb) noexcept override {
        on_message_ = std::move(cb);
    }

    void set_on_state(nimrtc::plugins::DataChannelStateCallback cb) noexcept override {
        on_state_ = std::move(cb);
    }

    // Test helpers
    void simulate_message(nimrtc::plugins::BufferView msg) {
        if (on_message_) on_message_(msg);
    }

    void simulate_state(const char* s) {
        if (on_state_) on_state_(s);
    }

    const nimrtc::plugins::DataChannelConfig& cfg() const { return cfg_; }
    std::size_t sent_bytes() const { return sent_.size(); }

private:
    enum State { kClosed, kOpen };
    State state_ = kClosed;
    nimrtc::plugins::DataChannelConfig cfg_;
    std::vector<std::uint8_t> sent_;
    nimrtc::plugins::DataMessageCallback on_message_;
    nimrtc::plugins::DataChannelStateCallback on_state_;
};

} // namespace

// -----------------------------------------------------------------------------
// DataChannelReliability enum
// -----------------------------------------------------------------------------

TEST(DataChannelReliability, HasExpectedVariants) {
    // All four QoS variants must be present.
    EXPECT_EQ(static_cast<int>(nimrtc::plugins::DataChannelReliability::kReliableOrdered),
              0);
    EXPECT_EQ(static_cast<int>(nimrtc::plugins::DataChannelReliability::kUnreliable),
              1);
    EXPECT_EQ(static_cast<int>(nimrtc::plugins::DataChannelReliability::kPartialReliableTTL),
              2);
    EXPECT_EQ(static_cast<int>(nimrtc::plugins::DataChannelReliability::kPartialReliableCount),
              3);
}

// -----------------------------------------------------------------------------
// DataChannelConfig
// -----------------------------------------------------------------------------

TEST(DataChannelConfig, DefaultValues) {
    nimrtc::plugins::DataChannelConfig cfg;
    EXPECT_EQ(cfg.label, std::string{});
    EXPECT_EQ(cfg.reliability,
              nimrtc::plugins::DataChannelReliability::kReliableOrdered);
    EXPECT_EQ(cfg.max_retransmits, 0u);
    EXPECT_EQ(cfg.max_packet_lifetime_ms, 0u);
    EXPECT_EQ(cfg.priority, 128u);
    EXPECT_TRUE(cfg.ordered);
}

TEST(DataChannelConfig, AssignAllFields) {
    nimrtc::plugins::DataChannelConfig cfg;
    cfg.label = "telemetry";
    cfg.reliability = nimrtc::plugins::DataChannelReliability::kUnreliable;
    cfg.max_retransmits = 10;
    cfg.max_packet_lifetime_ms = 500;
    cfg.priority = 240;
    cfg.ordered = false;

    EXPECT_EQ(cfg.label, "telemetry");
    EXPECT_EQ(cfg.reliability,
              nimrtc::plugins::DataChannelReliability::kUnreliable);
    EXPECT_EQ(cfg.max_retransmits, 10u);
    EXPECT_EQ(cfg.max_packet_lifetime_ms, 500u);
    EXPECT_EQ(cfg.priority, 240u);
    EXPECT_FALSE(cfg.ordered);
}

// -----------------------------------------------------------------------------
// IPlugin overrides on StubDataChannel
// -----------------------------------------------------------------------------

TEST(StubDataChannel, NameIsCorrect) {
    StubDataChannel ch;
    EXPECT_STREQ(ch.name(), "StubDataChannel");
}

TEST(StubDataChannel, OpenCloseLifecycle) {
    StubDataChannel ch;
    EXPECT_EQ(ch.open(), nimrtc::plugins::kOk);
    ch.close();
    // Closed — no error
}

TEST(StubDataChannel, OpenTwiceReturnsError) {
    StubDataChannel ch;
    EXPECT_EQ(ch.open(), nimrtc::plugins::kOk);
    EXPECT_EQ(ch.open(), nimrtc::plugins::kErrNotReady);  // already open
}

// -----------------------------------------------------------------------------
// SimpleDataChannelFactory
// -----------------------------------------------------------------------------

TEST(SimpleDataChannelFactory, ReportsCorrectId) {
    nimrtc::plugins::SimpleDataChannelFactory<StubDataChannel> factory{
        "stub", "Stub data channel"};
    EXPECT_EQ(factory.id(), "stub");
}

TEST(SimpleDataChannelFactory, ReportsCorrectDisplayName) {
    nimrtc::plugins::SimpleDataChannelFactory<StubDataChannel> factory{
        "stub", "Stub data channel"};
    EXPECT_EQ(factory.display_name(), "Stub data channel");
}

TEST(SimpleDataChannelFactory, CreateReturnsNewInstance) {
    nimrtc::plugins::SimpleDataChannelFactory<StubDataChannel> factory{
        "stub", "Stub data channel"};

    auto* ch1 = factory.create();
    auto* ch2 = factory.create();

    ASSERT_NE(ch1, nullptr);
    ASSERT_NE(ch2, nullptr);
    EXPECT_NE(ch1, ch2);  // different heap objects

    // Each instance is independently usable
    EXPECT_STREQ(ch1->name(), "StubDataChannel");
    EXPECT_STREQ(ch2->name(), "StubDataChannel");

    delete ch1;
    delete ch2;
}

// -----------------------------------------------------------------------------
// Callbacks
// -----------------------------------------------------------------------------

TEST(StubDataChannel, MessageCallbackInvoked) {
    StubDataChannel ch;

    bool called = false;
    std::vector<std::uint8_t> received;
    ch.set_on_message([&](nimrtc::plugins::BufferView msg) {
        called = true;
        received.assign(msg.begin(), msg.end());
    });

    std::uint8_t buf[] = {'h', 'e', 'l', 'l', 'o'};
    ch.simulate_message(buf);

    EXPECT_TRUE(called);
    ASSERT_EQ(received.size(), 5u);
    EXPECT_EQ(received[0], 'h');
    EXPECT_EQ(received[4], 'o');
}

TEST(StubDataChannel, StateCallbackInvoked) {
    StubDataChannel ch;

    const char* received_state = nullptr;
    ch.set_on_state([&](const char* s) { received_state = s; });

    ch.simulate_state("open");
    EXPECT_STREQ(received_state, "open");

    ch.simulate_state("closed");
    EXPECT_STREQ(received_state, "closed");
}

// -----------------------------------------------------------------------------
// send() queues data
// -----------------------------------------------------------------------------

TEST(StubDataChannel, SendAccumulatesBytes) {
    StubDataChannel ch;
    ch.open(nimrtc::plugins::DataChannelConfig{});

    std::uint8_t a[] = {'a', 'b'};
    std::uint8_t c[] = {'c', 'd', 'e'};

    ch.send(a);
    EXPECT_EQ(ch.sent_bytes(), 2u);

    ch.send(c);
    EXPECT_EQ(ch.sent_bytes(), 5u);
}

TEST(StubDataChannel, SendIgnoredWhenClosed) {
    StubDataChannel ch;  // not open
    std::uint8_t buf[] = {'x'};
    ch.send(buf);
    EXPECT_EQ(ch.sent_bytes(), 0u);
}

// -----------------------------------------------------------------------------
// open(cfg) stores config
// -----------------------------------------------------------------------------

TEST(StubDataChannel, OpenWithConfigStoresConfig) {
    StubDataChannel ch;
    nimrtc::plugins::DataChannelConfig cfg;
    cfg.label = "test-channel";
    cfg.reliability = nimrtc::plugins::DataChannelReliability::kPartialReliableTTL;
    cfg.max_packet_lifetime_ms = 1000;
    cfg.priority = 16;
    cfg.ordered = false;

    ch.open(cfg);

    EXPECT_EQ(ch.cfg().label, "test-channel");
    EXPECT_EQ(ch.cfg().reliability,
              nimrtc::plugins::DataChannelReliability::kPartialReliableTTL);
    EXPECT_EQ(ch.cfg().max_packet_lifetime_ms, 1000u);
    EXPECT_EQ(ch.cfg().priority, 16u);
    EXPECT_FALSE(ch.cfg().ordered);
}

// -----------------------------------------------------------------------------
// IPluginFactory base class
// -----------------------------------------------------------------------------

TEST(IDataChannelFactory, IsConstructableFromSimpleFactory) {
    // Verify that SimpleDataChannelFactory is a concrete IDataChannelFactory.
    // The factory id must be non-empty for valid registration.
    nimrtc::plugins::SimpleDataChannelFactory<StubDataChannel> factory{
        "sctp", "SCTP (usrsctp)"};

    const nimrtc::plugins::IPluginFactory* base = &factory;
    EXPECT_EQ(base->id(), "sctp");
    EXPECT_EQ(base->display_name(), "SCTP (usrsctp)");
}
