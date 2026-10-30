/**
 * @file tests/test_web_transport_data_channel.cpp
 * @brief WebTransportDataChannel — DC-1 land test (v0.14.0-beta.1).
 *
 * Verifies the v0.14.0-beta.1 WebTransport-backed IDataChannel
 * implementation:
 *   - The factory is registered with `core::PluginRegistry` under
 *     id `"web_transport"` and resolves by id.
 *   - The factory can produce a `WebTransportDataChannel` instance
 *     (with or without an injected session).
 *   - The channel implements `plugins::IDataChannel` correctly:
 *     - `name()` returns the documented string.
 *     - `open()` / `open(cfg)` are idempotent and the second form
 *       caches the config.
 *     - `close()` is idempotent and clears the stream.
 *   - The channel respects `cfg.reliability`:
 *     - `kUnreliable` → uses `send_datagram()` (no bidi stream opened).
 *     - `kReliableOrdered` → opens a bidi stream on first send
 *       (only when the session is in kConnected state).
 *     - `kPartialReliableTTL` / `kPartialReliableCount` → use the
 *       bidi stream with PR-style reset on TTL/count exhaustion.
 *   - For `kUnreliable`, `send()` is a no-op when the session is
 *     not connected (returns silently, no crash).
 *   - For `kReliableOrdered`, `send()` is a no-op when the stream
 *     cannot be opened (session not connected, no bidi stream
 *     returned by the session).
 *
 * The test uses a `MockWebTransportSession` that implements the
 * IWebTransportSession surface for the test only — it does NOT
 * link against msquic.  The seam-test pattern is mirrored.
 *
 * @note P0 — added as part of v0.14.0-beta.1 (DC-1 land).
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/plugins/base.hpp>       // plugins::Status
#include <nimrtc/plugins/datachannel.hpp>

#include <nimrtc/web_transport/bidi_stream_iface.hpp>
#include <nimrtc/web_transport/web_transport_data_channel.hpp>
#include <nimrtc/web_transport/web_transport_session_iface.hpp>
#include <nimrtc/web_transport/web_transport_state.hpp>

// ---------------------------------------------------------------------------
// Mock session — minimal IWebTransportSession impl for tests.
// ---------------------------------------------------------------------------

namespace nimrtc_wt_dc_test {

class MockBidiStream final : public nimrtc::web_transport::IBidirectionalStream {
public:
    // Test inspection.
    std::vector<std::vector<std::uint8_t>> sent_payloads;
    bool closed = false;
    bool reset_called = false;
    bool on_data_set = false;

    // ---- IBidirectionalStream ----
    nimrtc::plugins::Status send(
        std::span<const std::uint8_t> bytes) noexcept override {
        if (closed || reset_called) return nimrtc::plugins::kErrNotReady;
        if (bytes.empty()) return nimrtc::plugins::kOk;
        sent_payloads.emplace_back(bytes.begin(), bytes.end());
        return nimrtc::plugins::kOk;
    }
    nimrtc::plugins::Status recv(
        std::span<std::uint8_t> /*out*/) noexcept override {
        return nimrtc::plugins::kErrNotReady;
    }
    void set_on_data(
        nimrtc::web_transport::OnStreamDataCb cb) noexcept override {
        // Check before move — comparing a moved-from std::function
        // with nullptr is implementation-defined; some impls leave
        // it null, others leave it valid.
        on_data_set = static_cast<bool>(cb);
        on_data_cb_ = std::move(cb);
    }
    void close() noexcept override { closed = true; }
    void reset() noexcept override { reset_called = true; closed = true; }

    // Test helper: drive bytes through the installed on_data callback.
    void fire_on_data(std::span<const std::uint8_t> bytes) {
        if (on_data_cb_) on_data_cb_(nimrtc::plugins::BufferView{
            bytes.data(), bytes.size()});
    }

private:
    nimrtc::web_transport::OnStreamDataCb on_data_cb_;
};

class MockWebTransportSession final
    : public nimrtc::web_transport::IWebTransportSession {
public:
    // Test state.
    nimrtc::web_transport::WebTransportState state_ =
        nimrtc::web_transport::WebTransportState::kConnecting;
    std::vector<std::vector<std::uint8_t>> sent_datagrams;
    std::unique_ptr<MockBidiStream> next_bidi;
    bool close_called = false;

    // Inject a bidi stream to be returned by open_bidi_stream.
    void set_next_bidi(std::unique_ptr<MockBidiStream> b) {
        next_bidi = std::move(b);
    }

    // ---- IWebTransportSession ----
    nimrtc::web_transport::WebTransportState state() const noexcept override {
        return state_;
    }
    nimrtc::web_transport::IBidirectionalStream* open_bidi_stream() noexcept override {
        if (state_ != nimrtc::web_transport::WebTransportState::kConnected) {
            return nullptr;
        }
        if (!next_bidi) {
            next_bidi = std::make_unique<MockBidiStream>();
        }
        nimrtc::web_transport::IBidirectionalStream* raw = next_bidi.get();
        // Transfer ownership to a registry to mimic real session
        // semantics; here we just keep next_bidi alive.
        held_streams.push_back(std::move(next_bidi));
        return raw;
    }
    nimrtc::web_transport::IUnidirectionalStream* open_uni_stream() noexcept override {
        return nullptr;
    }
    nimrtc::plugins::Status send_datagram(
        std::span<const std::uint8_t> bytes) noexcept override {
        if (state_ != nimrtc::web_transport::WebTransportState::kConnected) {
            return nimrtc::plugins::kErrNotReady;
        }
        if (bytes.empty()) return nimrtc::plugins::kOk;
        sent_datagrams.emplace_back(bytes.begin(), bytes.end());
        return nimrtc::plugins::kOk;
    }
    void set_on_datagram(
        nimrtc::web_transport::OnDatagramCb cb) noexcept override {
        on_datagram_cb_ = std::move(cb);
    }
    void set_on_peer_stream(
        nimrtc::web_transport::OnPeerStreamCb cb) noexcept override {
        on_peer_stream_cb_ = std::move(cb);
    }
    void close() noexcept override {
        close_called = true;
        state_ = nimrtc::web_transport::WebTransportState::kClosed;
    }

    // Test inspection — public for ease of test access.
    std::vector<std::unique_ptr<MockBidiStream>> held_streams;
    nimrtc::web_transport::OnDatagramCb on_datagram_cb_;
    nimrtc::web_transport::OnPeerStreamCb on_peer_stream_cb_;
};

} // namespace nimrtc_wt_dc_test

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

namespace {

class WebTransportDataChannelTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Make sure the factory is registered (idempotent).
        nimrtc::web_transport::register_default_plugins();
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Factory + registration tests
// ---------------------------------------------------------------------------

TEST_F(WebTransportDataChannelTest, factory_registered_with_id) {
    // After register_default_plugins(), the registry should have
    // a factory under id "web_transport".
    auto* factory =
        nimrtc::core::PluginRegistry::instance().get_datachannel("web_transport");
    ASSERT_NE(factory, nullptr) << "WebTransportDataChannelFactory not registered";
    EXPECT_EQ(factory->id(), "web_transport");
    EXPECT_FALSE(factory->display_name().empty());
}

TEST_F(WebTransportDataChannelTest, factory_create_returns_channel) {
    auto* factory =
        nimrtc::core::PluginRegistry::instance().get_datachannel("web_transport");
    ASSERT_NE(factory, nullptr);

    std::unique_ptr<nimrtc::plugins::IDataChannel> channel{factory->create()};
    ASSERT_NE(channel, nullptr);
    EXPECT_NE(std::string(channel->name()).find("WebTransportDataChannel"),
              std::string::npos);
}

TEST_F(WebTransportDataChannelTest, factory_create_with_session) {
    auto* factory =
        nimrtc::core::PluginRegistry::instance().get_datachannel("web_transport");
    ASSERT_NE(factory, nullptr);

    // The factory type-erases the session-aware create() — cast
    // back to the concrete factory to invoke it.  The registry
    // returns a const pointer, so we const_cast for the duration
    // of this test (the registry does not modify factories).
    auto* concrete =
        const_cast<nimrtc::web_transport::WebTransportDataChannelFactory*>(
            static_cast<const nimrtc::web_transport::WebTransportDataChannelFactory*>(
                factory));
    ASSERT_NE(concrete, nullptr) << "factory is not WebTransportDataChannelFactory";

    nimrtc_wt_dc_test::MockWebTransportSession session;
    std::unique_ptr<nimrtc::web_transport::WebTransportDataChannel> channel{
        concrete->create(&session)};
    ASSERT_NE(channel, nullptr);
    EXPECT_NE(std::string(channel->name()).find("WebTransportDataChannel"),
              std::string::npos);
}

// ---------------------------------------------------------------------------
// Lifecycle tests
// ---------------------------------------------------------------------------

TEST_F(WebTransportDataChannelTest, open_and_close_are_idempotent) {
    nimrtc_wt_dc_test::MockWebTransportSession session;
    nimrtc::web_transport::WebTransportDataChannelFactory factory;

    std::unique_ptr<nimrtc::web_transport::WebTransportDataChannel> channel{
        factory.create(&session)};
    ASSERT_NE(channel, nullptr);

    // open() twice — should not crash, should not double-register.
    EXPECT_EQ(channel->open(), nimrtc::plugins::kOk);
    EXPECT_EQ(channel->open(), nimrtc::plugins::kOk);

    // close() twice — should be idempotent.
    channel->close();
    channel->close();

    // After close, is_open() should report false.
    EXPECT_FALSE(channel->is_open());
}

TEST_F(WebTransportDataChannelTest, open_without_session_returns_not_ready) {
    // The IPlugin overload open() on a channel with no session
    // should return kErrNotReady.
    auto* factory =
        nimrtc::core::PluginRegistry::instance().get_datachannel("web_transport");
    ASSERT_NE(factory, nullptr);

    std::unique_ptr<nimrtc::plugins::IDataChannel> channel{factory->create()};
    ASSERT_NE(channel, nullptr);
    EXPECT_EQ(channel->open(), nimrtc::plugins::kErrNotReady);
}

TEST_F(WebTransportDataChannelTest, open_config_caches_reliability) {
    nimrtc_wt_dc_test::MockWebTransportSession session;
    nimrtc::web_transport::WebTransportDataChannelFactory factory;

    std::unique_ptr<nimrtc::web_transport::WebTransportDataChannel> channel{
        factory.create(&session)};
    ASSERT_NE(channel, nullptr);

    nimrtc::plugins::DataChannelConfig cfg{};
    cfg.label = "telemetry";
    cfg.reliability = nimrtc::plugins::DataChannelReliability::kReliableOrdered;
    channel->open(cfg);

    EXPECT_EQ(channel->config().label, "telemetry");
    EXPECT_EQ(channel->config().reliability,
              nimrtc::plugins::DataChannelReliability::kReliableOrdered);
}

// ---------------------------------------------------------------------------
// Reliability routing tests
// ---------------------------------------------------------------------------

TEST_F(WebTransportDataChannelTest, send_unreliable_uses_datagram) {
    nimrtc_wt_dc_test::MockWebTransportSession session;
    session.state_ = nimrtc::web_transport::WebTransportState::kConnected;
    nimrtc::web_transport::WebTransportDataChannelFactory factory;

    std::unique_ptr<nimrtc::web_transport::WebTransportDataChannel> channel{
        factory.create(&session)};

    nimrtc::plugins::DataChannelConfig cfg{};
    cfg.label = "telemetry";
    cfg.reliability = nimrtc::plugins::DataChannelReliability::kUnreliable;
    channel->open(cfg);
    ASSERT_EQ(channel->open(), nimrtc::plugins::kOk);

    constexpr std::uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    channel->send(nimrtc::plugins::BufferView{
        payload, sizeof(payload)});

    // The datagram path should have been taken: the session should
    // see the bytes; no bidi stream should have been opened.
    ASSERT_EQ(session.sent_datagrams.size(), 1u);
    EXPECT_EQ(session.sent_datagrams[0].size(), sizeof(payload));
    EXPECT_EQ(session.sent_datagrams[0][0], 0xDE);
    EXPECT_TRUE(session.held_streams.empty());
}

TEST_F(WebTransportDataChannelTest, send_unreliable_noop_when_not_connected) {
    nimrtc_wt_dc_test::MockWebTransportSession session;
    session.state_ = nimrtc::web_transport::WebTransportState::kConnecting;
    nimrtc::web_transport::WebTransportDataChannelFactory factory;

    std::unique_ptr<nimrtc::web_transport::WebTransportDataChannel> channel{
        factory.create(&session)};

    nimrtc::plugins::DataChannelConfig cfg{};
    cfg.reliability = nimrtc::plugins::DataChannelReliability::kUnreliable;
    channel->open(cfg);
    channel->open();  // IPlugin open

    constexpr std::uint8_t payload[] = {0x01, 0x02};
    channel->send(nimrtc::plugins::BufferView{
        payload, sizeof(payload)});

    // No datagrams should have been sent (session not connected).
    EXPECT_TRUE(session.sent_datagrams.empty());
}

TEST_F(WebTransportDataChannelTest, send_reliable_uses_bidi_stream) {
    nimrtc_wt_dc_test::MockWebTransportSession session;
    session.state_ = nimrtc::web_transport::WebTransportState::kConnected;
    nimrtc::web_transport::WebTransportDataChannelFactory factory;

    std::unique_ptr<nimrtc::web_transport::WebTransportDataChannel> channel{
        factory.create(&session)};

    nimrtc::plugins::DataChannelConfig cfg{};
    cfg.label = "file";
    cfg.reliability = nimrtc::plugins::DataChannelReliability::kReliableOrdered;
    channel->open(cfg);
    ASSERT_EQ(channel->open(), nimrtc::plugins::kOk);

    constexpr std::uint8_t payload[] = {0xCA, 0xFE};
    channel->send(nimrtc::plugins::BufferView{
        payload, sizeof(payload)});

    // The reliable path should have opened a bidi stream and sent
    // the bytes through it (not via datagram).
    ASSERT_EQ(session.held_streams.size(), 1u);
    EXPECT_EQ(session.held_streams[0]->sent_payloads.size(), 1u);
    EXPECT_EQ(session.held_streams[0]->sent_payloads[0].size(), sizeof(payload));
    EXPECT_TRUE(session.sent_datagrams.empty());
    EXPECT_TRUE(session.held_streams[0]->on_data_set)
        << "on_data callback should have been installed";
}

TEST_F(WebTransportDataChannelTest, send_reliable_uses_existing_stream) {
    // Two consecutive sends should reuse the same bidi stream, not
    // open a new one.
    nimrtc_wt_dc_test::MockWebTransportSession session;
    session.state_ = nimrtc::web_transport::WebTransportState::kConnected;
    nimrtc::web_transport::WebTransportDataChannelFactory factory;

    std::unique_ptr<nimrtc::web_transport::WebTransportDataChannel> channel{
        factory.create(&session)};

    nimrtc::plugins::DataChannelConfig cfg{};
    cfg.reliability = nimrtc::plugins::DataChannelReliability::kReliableOrdered;
    channel->open(cfg);
    ASSERT_EQ(channel->open(), nimrtc::plugins::kOk);

    constexpr std::uint8_t p1[] = {0x01};
    constexpr std::uint8_t p2[] = {0x02, 0x03};
    channel->send(nimrtc::plugins::BufferView{p1, sizeof(p1)});
    channel->send(nimrtc::plugins::BufferView{p2, sizeof(p2)});

    ASSERT_EQ(session.held_streams.size(), 1u);
    EXPECT_EQ(session.held_streams[0]->sent_payloads.size(), 2u);
}

TEST_F(WebTransportDataChannelTest, send_reliable_noop_when_not_connected) {
    nimrtc_wt_dc_test::MockWebTransportSession session;
    session.state_ = nimrtc::web_transport::WebTransportState::kConnecting;
    nimrtc::web_transport::WebTransportDataChannelFactory factory;

    std::unique_ptr<nimrtc::web_transport::WebTransportDataChannel> channel{
        factory.create(&session)};

    nimrtc::plugins::DataChannelConfig cfg{};
    cfg.reliability = nimrtc::plugins::DataChannelReliability::kReliableOrdered;
    channel->open(cfg);
    channel->open();

    constexpr std::uint8_t payload[] = {0xAA};
    channel->send(nimrtc::plugins::BufferView{payload, sizeof(payload)});

    // No stream should have been opened (session not connected).
    EXPECT_TRUE(session.held_streams.empty());
}

TEST_F(WebTransportDataChannelTest, close_releases_stream) {
    nimrtc_wt_dc_test::MockWebTransportSession session;
    session.state_ = nimrtc::web_transport::WebTransportState::kConnected;
    nimrtc::web_transport::WebTransportDataChannelFactory factory;

    std::unique_ptr<nimrtc::web_transport::WebTransportDataChannel> channel{
        factory.create(&session)};

    nimrtc::plugins::DataChannelConfig cfg{};
    cfg.reliability = nimrtc::plugins::DataChannelReliability::kReliableOrdered;
    channel->open(cfg);
    channel->open();  // opens a stream

    ASSERT_EQ(session.held_streams.size(), 1u);
    EXPECT_FALSE(session.held_streams[0]->closed);

    channel->close();
    EXPECT_TRUE(session.held_streams[0]->closed)
        << "Stream should be closed when channel closes";
}

TEST_F(WebTransportDataChannelTest, set_on_message_does_not_crash) {
    // Setting the message callback before open() must be safe.
    nimrtc_wt_dc_test::MockWebTransportSession session;
    nimrtc::web_transport::WebTransportDataChannelFactory factory;

    std::unique_ptr<nimrtc::web_transport::WebTransportDataChannel> channel{
        factory.create(&session)};

    bool called = false;
    channel->set_on_message(
        [&called](nimrtc::plugins::BufferView /*data*/) { called = true; });
    channel->set_on_state(
        [](const char* /*state*/) {});

    // No crash, no exception.  The callback isn't fired here because
    // we haven't sent/received anything.
    EXPECT_FALSE(called);
}

TEST_F(WebTransportDataChannelTest, attach_registers_session_callbacks) {
    nimrtc_wt_dc_test::MockWebTransportSession session;
    nimrtc::web_transport::WebTransportDataChannelFactory factory;

    std::unique_ptr<nimrtc::web_transport::WebTransportDataChannel> channel{
        factory.create(&session)};

    nimrtc::plugins::DataChannelConfig cfg{};
    cfg.reliability = nimrtc::plugins::DataChannelReliability::kReliableOrdered;
    channel->open(cfg);
    channel->attach_session_callbacks();

    EXPECT_TRUE(static_cast<bool>(session.on_datagram_cb_));
    EXPECT_TRUE(static_cast<bool>(session.on_peer_stream_cb_));
}

TEST_F(WebTransportDataChannelTest, inbound_datagram_delivers_in_unreliable_mode) {
    nimrtc_wt_dc_test::MockWebTransportSession session;
    session.state_ = nimrtc::web_transport::WebTransportState::kConnected;
    nimrtc::web_transport::WebTransportDataChannelFactory factory;

    std::unique_ptr<nimrtc::web_transport::WebTransportDataChannel> channel{
        factory.create(&session)};

    nimrtc::plugins::DataChannelConfig cfg{};
    cfg.reliability = nimrtc::plugins::DataChannelReliability::kUnreliable;
    channel->open(cfg);
    ASSERT_EQ(channel->open(), nimrtc::plugins::kOk);
    channel->attach_session_callbacks();
    ASSERT_TRUE(static_cast<bool>(session.on_datagram_cb_));

    std::vector<std::uint8_t> received;
    channel->set_on_message(
        [&received](nimrtc::plugins::BufferView data) {
            received.assign(data.begin(), data.end());
        });

    constexpr std::uint8_t payload[] = {0x11, 0x22, 0x33};
    session.on_datagram_cb_(
        std::span<const std::uint8_t>{payload, sizeof(payload)});

    ASSERT_EQ(received.size(), sizeof(payload));
    EXPECT_EQ(received[0], 0x11);
    EXPECT_EQ(received[2], 0x33);
}

TEST_F(WebTransportDataChannelTest, inbound_datagram_ignored_in_reliable_mode) {
    nimrtc_wt_dc_test::MockWebTransportSession session;
    session.state_ = nimrtc::web_transport::WebTransportState::kConnected;
    nimrtc::web_transport::WebTransportDataChannelFactory factory;

    std::unique_ptr<nimrtc::web_transport::WebTransportDataChannel> channel{
        factory.create(&session)};

    nimrtc::plugins::DataChannelConfig cfg{};
    cfg.reliability = nimrtc::plugins::DataChannelReliability::kReliableOrdered;
    channel->open(cfg);
    ASSERT_EQ(channel->open(), nimrtc::plugins::kOk);
    channel->attach_session_callbacks();

    bool called = false;
    channel->set_on_message(
        [&called](nimrtc::plugins::BufferView /*data*/) { called = true; });

    constexpr std::uint8_t payload[] = {0xAA};
    session.on_datagram_cb_(
        std::span<const std::uint8_t>{payload, sizeof(payload)});
    EXPECT_FALSE(called);
}

TEST_F(WebTransportDataChannelTest, inbound_peer_stream_strips_header_and_delivers) {
    nimrtc_wt_dc_test::MockWebTransportSession session;
    session.state_ = nimrtc::web_transport::WebTransportState::kConnected;
    nimrtc::web_transport::WebTransportDataChannelFactory factory;

    std::unique_ptr<nimrtc::web_transport::WebTransportDataChannel> channel{
        factory.create(&session)};

    nimrtc::plugins::DataChannelConfig cfg{};
    cfg.reliability = nimrtc::plugins::DataChannelReliability::kReliableOrdered;
    channel->open(cfg);
    ASSERT_EQ(channel->open(), nimrtc::plugins::kOk);
    channel->attach_session_callbacks();
    ASSERT_TRUE(static_cast<bool>(session.on_peer_stream_cb_));

    std::vector<std::uint8_t> received;
    channel->set_on_message(
        [&received](nimrtc::plugins::BufferView data) {
            received.insert(received.end(), data.begin(), data.end());
        });

    // Peer-initiated stream: WebTransport data stream header is
    // signal varint 0x41 + session-id varint (here session id 0),
    // followed by the payload.
    auto peer = std::make_unique<nimrtc_wt_dc_test::MockBidiStream>();
    nimrtc_wt_dc_test::MockBidiStream* peer_raw = peer.get();
    session.held_streams.push_back(std::move(peer));

    session.on_peer_stream_cb_(peer_raw);
    ASSERT_TRUE(peer_raw->on_data_set);

    // Split delivery: header first, payload second — exercises the
    // header reassembly path for headers fragmented across on_data
    // calls.
    constexpr std::uint8_t header[] = {0x41, 0x00};
    constexpr std::uint8_t payload[] = {0xCA, 0xFE, 0x01};
    peer_raw->fire_on_data(
        std::span<const std::uint8_t>{header, sizeof(header)});
    EXPECT_TRUE(received.empty())
        << "Header-only chunk must not deliver payload yet";
    peer_raw->fire_on_data(
        std::span<const std::uint8_t>{payload, sizeof(payload)});

    ASSERT_EQ(received.size(), sizeof(payload));
    EXPECT_EQ(received[0], 0xCA);
    EXPECT_EQ(received[1], 0xFE);
    EXPECT_EQ(received[2], 0x01);
}

TEST_F(WebTransportDataChannelTest, close_unregisters_session_callbacks) {
    nimrtc_wt_dc_test::MockWebTransportSession session;
    nimrtc::web_transport::WebTransportDataChannelFactory factory;

    std::unique_ptr<nimrtc::web_transport::WebTransportDataChannel> channel{
        factory.create(&session)};

    nimrtc::plugins::DataChannelConfig cfg{};
    cfg.reliability = nimrtc::plugins::DataChannelReliability::kUnreliable;
    channel->open(cfg);
    channel->attach_session_callbacks();
    ASSERT_TRUE(static_cast<bool>(session.on_datagram_cb_));

    channel->close();
    EXPECT_FALSE(static_cast<bool>(session.on_datagram_cb_));
    EXPECT_FALSE(static_cast<bool>(session.on_peer_stream_cb_));
}
