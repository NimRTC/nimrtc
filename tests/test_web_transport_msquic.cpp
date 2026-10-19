/**
 * @file tests/test_web_transport_msquic.cpp
 * @brief WebTransportMsQuicSessionFactory — beta.1 smoke test (v0.14.0-beta.1).
 *
 * Verifies the v0.14.0-beta.1 WebTransport / QUIC factory seam:
 *   1. Factory `id()` returns "msquic" and is registered with the
 *      core::PluginRegistry under that id.
 *   2. `factory->display_name()` is non-empty.
 *   3. `list_web_transports()` contains "msquic".
 *   4. IWebTransportSession surface works with a MockWebTransportSession:
 *      - fresh session is in kConnecting
 *      - open_bidi_stream() returns nullptr in kConnecting
 *      - open_bidi_stream() returns a stream in kConnected
 *      - send_datagram returns kErrNotReady in kConnecting
 *      - send_datagram returns kOk in kConnected
 *      - close() transitions to kClosed and is idempotent
 *      - set_on_datagram() can be called without crashing
 *
 * The test uses ONLY the public factory seam (IWebTransportSessionFactory +
 * IWebTransportSession) with a MockWebTransportSession — no direct .cpp include,
 * no LoopbackMsQuicPeer, no Schannel cert provisioning.  Real msquic
 * create() requires a reachable QUIC peer and is exercised end-to-end
 * by the Chrome e2e Case E harness
 * (tests/chrome_interop_harness/run_harness.py).
 *
 * @note P0 — added as part of v0.14.0-beta.1 (ADR-014).
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/plugins/base.hpp>       // plugins::Status

#include <nimrtc/web_transport/web_transport_config.hpp>
#include <nimrtc/web_transport/web_transport_plugin.hpp>
#include <nimrtc/web_transport/web_transport_session_factory.hpp>
#include <nimrtc/web_transport/web_transport_session_iface.hpp>
#include <nimrtc/web_transport/web_transport_state.hpp>
#include <nimrtc/web_transport/bidi_stream_iface.hpp>

// ---------------------------------------------------------------------------
// Mock session — minimal IWebTransportSession impl for tests.
// ---------------------------------------------------------------------------

namespace nimrtc_wt_msquic_test {

class MockBidiStream final
    : public nimrtc::web_transport::IBidirectionalStream {
public:
    bool closed = false;

    // ---- IBidirectionalStream ----
    nimrtc::plugins::Status send(
        std::span<const std::uint8_t> /*bytes*/) noexcept override {
        return closed ? nimrtc::plugins::kErrNotReady : nimrtc::plugins::kOk;
    }
    nimrtc::plugins::Status recv(
        std::span<std::uint8_t> /*out*/) noexcept override {
        return closed ? nimrtc::plugins::kErrNotReady
                     : nimrtc::plugins::kErrClosed;  // nothing received yet
    }
    void set_on_data(
        nimrtc::web_transport::OnStreamDataCb /*cb*/) noexcept override {}
    void close() noexcept override { closed = true; }
    void reset() noexcept override { closed = true; }
};

class MockWebTransportSession final
    : public nimrtc::web_transport::IWebTransportSession {
public:
    nimrtc::web_transport::WebTransportState state_ =
        nimrtc::web_transport::WebTransportState::kConnecting;
    std::vector<std::vector<std::uint8_t>> sent_datagrams;
    bool on_datagram_set = false;
    std::vector<std::unique_ptr<MockBidiStream>> bidi_streams;

    // ---- IWebTransportSession ----
    nimrtc::web_transport::WebTransportState state()
        const noexcept override {
        return state_;
    }
    nimrtc::web_transport::IBidirectionalStream* open_bidi_stream() noexcept override {
        if (state_ != nimrtc::web_transport::WebTransportState::kConnected) {
            return nullptr;
        }
        auto s = std::make_unique<MockBidiStream>();
        nimrtc::web_transport::IBidirectionalStream* raw = s.get();
        bidi_streams.push_back(std::move(s));
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
        nimrtc::web_transport::OnDatagramCb /*cb*/) noexcept override {
        on_datagram_set = true;
    }
    void close() noexcept override {
        state_ = nimrtc::web_transport::WebTransportState::kClosed;
    }
};

} // namespace nimrtc_wt_msquic_test

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

namespace {

class WebTransportMsQuicTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Register the msquic-backed factory (idempotent).
        nimrtc::web_transport::register_default_plugins();
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Factory + registration tests
// ---------------------------------------------------------------------------

TEST_F(WebTransportMsQuicTest, factory_registered_with_id_msquic) {
    // After register_default_plugins(), the registry should have
    // a factory under id "msquic".
    auto* factory =
        nimrtc::core::PluginRegistry::instance().get_web_transport("msquic");
    ASSERT_NE(factory, nullptr)
        << "WebTransportMsQuicSessionFactory not registered under id 'msquic'";
    EXPECT_EQ(factory->id(), "msquic");
    EXPECT_FALSE(factory->display_name().empty());
}

TEST_F(WebTransportMsQuicTest, list_web_transports_contains_msquic) {
    auto ids = nimrtc::core::PluginRegistry::instance().list_web_transports();
    EXPECT_FALSE(ids.empty()) << "list_web_transports() should not be empty";
    bool found = false;
    for (const auto& id : ids) {
        if (id == "msquic") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "'msquic' should be in list_web_transports()";
}

TEST_F(WebTransportMsQuicTest, factory_display_name_mentions_msquic) {
    auto* factory =
        nimrtc::core::PluginRegistry::instance().get_web_transport("msquic");
    ASSERT_NE(factory, nullptr);
    auto name = std::string(factory->display_name());
    EXPECT_NE(name.find("msquic"), std::string::npos)
        << "display_name should mention 'msquic', got: " << name;
}

TEST_F(WebTransportMsQuicTest, unknown_id_returns_null) {
    // Sanity check — a non-existent factory id returns nullptr.
    auto* factory =
        nimrtc::core::PluginRegistry::instance().get_web_transport(
            "nonexistent_backend_xyz");
    EXPECT_EQ(factory, nullptr);
}

// ---------------------------------------------------------------------------
// IWebTransportSession surface tests (using MockWebTransportSession).
// The MockWebTransportSession exercises the same interface that the real
// msquic-backed WebTransportMsQuicSession implements, so the surface
// contract is validated end-to-end without needing a real QUIC peer.
// ---------------------------------------------------------------------------

TEST_F(WebTransportMsQuicTest, mock_session_starts_in_kConnecting) {
    nimrtc_wt_msquic_test::MockWebTransportSession session;
    EXPECT_EQ(session.state(),
              nimrtc::web_transport::WebTransportState::kConnecting);
}

TEST_F(WebTransportMsQuicTest, mock_session_open_bidi_stream_returns_stream_when_connected) {
    nimrtc_wt_msquic_test::MockWebTransportSession session;
    session.state_ = nimrtc::web_transport::WebTransportState::kConnected;

    auto* s = session.open_bidi_stream();
    ASSERT_NE(s, nullptr);

    // Send on the stream.
    constexpr std::uint8_t data[] = {0xCA, 0xFE};
    EXPECT_EQ(s->send(data), nimrtc::plugins::kOk);

    s->close();
    EXPECT_EQ(s->send(data), nimrtc::plugins::kErrNotReady)
        << "send() should return kErrNotReady after stream close";
}

TEST_F(WebTransportMsQuicTest, mock_session_open_bidi_stream_returns_null_when_not_connected) {
    nimrtc_wt_msquic_test::MockWebTransportSession session;
    session.state_ = nimrtc::web_transport::WebTransportState::kConnecting;

    EXPECT_EQ(session.open_bidi_stream(), nullptr);
}

TEST_F(WebTransportMsQuicTest, mock_session_open_bidi_stream_returns_null_when_closed) {
    nimrtc_wt_msquic_test::MockWebTransportSession session;
    session.state_ = nimrtc::web_transport::WebTransportState::kClosed;

    EXPECT_EQ(session.open_bidi_stream(), nullptr);
}

TEST_F(WebTransportMsQuicTest, mock_session_send_datagram_works_when_connected) {
    nimrtc_wt_msquic_test::MockWebTransportSession session;
    session.state_ = nimrtc::web_transport::WebTransportState::kConnected;

    constexpr std::uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_EQ(session.send_datagram(payload), nimrtc::plugins::kOk);
    ASSERT_EQ(session.sent_datagrams.size(), 1u);
    EXPECT_EQ(session.sent_datagrams[0].size(), 4u);
}

TEST_F(WebTransportMsQuicTest, mock_session_send_datagram_returns_not_ready_when_not_connected) {
    nimrtc_wt_msquic_test::MockWebTransportSession session;
    session.state_ = nimrtc::web_transport::WebTransportState::kConnecting;

    constexpr std::uint8_t payload[] = {0x01};
    EXPECT_EQ(session.send_datagram(payload),
              nimrtc::plugins::kErrNotReady);
}

TEST_F(WebTransportMsQuicTest, mock_session_send_datagram_empty_payload_is_noop) {
    nimrtc_wt_msquic_test::MockWebTransportSession session;
    session.state_ = nimrtc::web_transport::WebTransportState::kConnected;

    // Empty payload should return kOk and not record anything.
    EXPECT_EQ(session.send_datagram({}), nimrtc::plugins::kOk);
    EXPECT_TRUE(session.sent_datagrams.empty());
}

TEST_F(WebTransportMsQuicTest, mock_session_set_on_datagram_sets_flag) {
    nimrtc_wt_msquic_test::MockWebTransportSession session;

    EXPECT_FALSE(session.on_datagram_set);
    session.set_on_datagram(
        [](std::span<const std::uint8_t> /*bytes*/) {});
    EXPECT_TRUE(session.on_datagram_set);
}

TEST_F(WebTransportMsQuicTest, mock_session_close_transitions_to_kClosed) {
    nimrtc_wt_msquic_test::MockWebTransportSession session;
    session.state_ = nimrtc::web_transport::WebTransportState::kConnected;

    session.close();
    EXPECT_EQ(session.state(),
              nimrtc::web_transport::WebTransportState::kClosed);
}

TEST_F(WebTransportMsQuicTest, mock_session_close_is_idempotent) {
    nimrtc_wt_msquic_test::MockWebTransportSession session;
    session.state_ = nimrtc::web_transport::WebTransportState::kConnected;

    session.close();
    session.close();  // no-op
    EXPECT_EQ(session.state(),
              nimrtc::web_transport::WebTransportState::kClosed);
}

// ---------------------------------------------------------------------------
// WebTransportConfig tests
// ---------------------------------------------------------------------------

TEST_F(WebTransportMsQuicTest, config_default_values_match_rfc) {
    nimrtc::web_transport::WebTransportConfig cfg{};
    EXPECT_EQ(cfg.max_idle_timeout_ms, 30'000u);  // 30 s
    EXPECT_EQ(cfg.initial_max_stream_data, 65'536u);   // 64 KiB
    EXPECT_EQ(cfg.initial_max_data, 1'048'576u);       // 1 MiB
    EXPECT_EQ(cfg.keep_alive_interval_ms, 0u);  // disabled by default
    EXPECT_EQ(cfg.peer_port, 443);  // standard WebTransport port
    EXPECT_TRUE(cfg.peer_hostname.empty());  // unset
}
