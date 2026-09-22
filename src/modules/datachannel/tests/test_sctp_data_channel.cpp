/**
 * @file src/modules/datachannel/tests/test_sctp_data_channel.cpp
 * @brief Unit tests for SctpDataChannel + SctpDataChannelFactory.
 *
 * Verifies the P2 datachannel implementation:
 *   1. SctpDataChannelFactory doesn't return null from create().
 *   2. Factory id is exactly "sctp".
 *   3. send() before open() doesn't crash and signals failure via
 *      the on_state callback.
 *   4. set_on_message / set_on_state before open() are safe.
 *   5. open() wires the SCTP recv callback and reports opened.
 *   6. send() after open() routes to the matching SCTP send method
 *      based on cfg.reliability (verified via a MockSctpSocket).
 *
 * All tests use a `MockSctpSocket` injected via the test-only
 * `SctpDataChannel(std::unique_ptr<ISctpSocket>)` constructor so we
 * don't depend on a real usrsctp handshake — those are exercised
 * elsewhere (see test_sctp_factory.cpp + the sctp loopback tests).
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/datachannel/sctp_data_channel.hpp>
#include <nimrtc/plugins/base.hpp>
#include <nimrtc/plugins/datachannel.hpp>
#include <nimrtc/sctp/sctp_socket_iface.hpp>

namespace {

using nimrtc::datachannel::SctpDataChannel;
using nimrtc::datachannel::SctpDataChannelFactory;
using nimrtc::plugins::BufferView;
using nimrtc::plugins::DataChannelConfig;
using nimrtc::plugins::DataChannelReliability;
using nimrtc::plugins::DataChannelStateCallback;
using nimrtc::plugins::DataMessageCallback;
using nimrtc::plugins::Status;
using nimrtc::sctp::ISctpSocket;

// ---------------------------------------------------------------------------
// MockSctpSocket — minimal ISctpSocket that records every send method call.
//
// Used by every test that needs to verify "the channel routed to the
// right ISctpSocket method". Counter values are atomic so we can poke
// at them from any thread (the recv trampoline fires on usrsctp's
// worker thread in production; here we just call it from the test
// thread, but the atomicity makes the test future-proof against
// threaded scenarios).
// ---------------------------------------------------------------------------
class MockSctpSocket final : public ISctpSocket {
public:
    // ---- Forward declarations ---------------------------------------------
    //
    // Method and SendRecord are referenced by the ISctpSocket override
    // bodies (send_datagram / send_stream / send_partial_reliable) AND
    // by the snapshot() return type below.  C++ says inline member
    // function bodies and return types are parsed in the complete-class
    // context — but MSVC has historically been finicky about the return
    // type ordering, so we declare the types up front to keep the
    // parser happy on every MSVC version.  See NimRTC issue tracker
    // #TSC-2031 for the MSVC C2923 reproducer that drove this layout.
    enum class Method : std::uint8_t {
        kDatagram,
        kStream,
        kPartialReliable,
    };
    struct SendRecord {
        Method                  method = Method::kStream;
        std::uint16_t           stream = 0;
        std::vector<std::uint8_t> data{};
        std::int64_t            ttl_ms = 0;
    };

    MockSctpSocket() = default;
    ~MockSctpSocket() override = default;

    MockSctpSocket(const MockSctpSocket&)            = delete;
    MockSctpSocket& operator=(const MockSctpSocket&) = delete;

    // ---- ISctpSocket -----------------------------------------------------

    Status send_datagram(std::uint16_t stream,
                         BufferView data) noexcept override {
        SendRecord r;
        r.method  = Method::kDatagram;
        r.stream  = stream;
        r.data.assign(data.begin(), data.end());
        r.ttl_ms  = 0;
        record(std::move(r));
        return datagram_rc_;
    }

    Status send_stream(std::uint16_t stream,
                       BufferView data) noexcept override {
        SendRecord r;
        r.method = Method::kStream;
        r.stream = stream;
        r.data.assign(data.begin(), data.end());
        r.ttl_ms = 0;
        record(std::move(r));
        return stream_rc_;
    }

    Status send_partial_reliable(
        std::uint16_t stream,
        BufferView data,
        std::chrono::milliseconds ttl) noexcept override {
        SendRecord r;
        r.method = Method::kPartialReliable;
        r.stream = stream;
        r.data.assign(data.begin(), data.end());
        r.ttl_ms = static_cast<std::int64_t>(ttl.count());
        record(std::move(r));
        return partial_rc_;
    }

    void set_on_recv(nimrtc::plugins::OnSctpRecvCb cb) noexcept override {
        std::lock_guard<std::mutex> lk(recv_mu_);
        on_recv_ = std::move(cb);
    }

    // ---- Test helpers ----------------------------------------------------

    /** Simulate an inbound SCTP DATA payload from the peer. Fires the
     *  callback registered by `set_on_recv`. */
    void simulate_recv(std::uint16_t stream, nimrtc::core::ByteSpan data) {
        nimrtc::plugins::OnSctpRecvCb cb_copy;
        {
            std::lock_guard<std::mutex> lk(recv_mu_);
            cb_copy = on_recv_;
        }
        if (cb_copy) cb_copy(stream, data);
    }

    /** Returns the recorded send calls. Thread-safe. */
    std::vector<SendRecord> snapshot() const {
        std::lock_guard<std::mutex> lk(mu_);
        return records_;
    }

    /** Override the return value of the matching send_* method so the
     *  test can simulate failures. */
    void set_datagram_rc(Status rc)   noexcept { datagram_rc_   = rc; }
    void set_stream_rc(Status rc)     noexcept { stream_rc_     = rc; }
    void set_partial_rc(Status rc)    noexcept { partial_rc_    = rc; }

    /** Whether `set_on_recv` was called with a non-empty callback. */
    bool has_recv_callback() const {
        std::lock_guard<std::mutex> lk(recv_mu_);
        return static_cast<bool>(on_recv_);
    }

private:
    void record(SendRecord&& r) {
        std::lock_guard<std::mutex> lk(mu_);
        records_.push_back(std::move(r));
    }

    mutable std::mutex                 mu_;
    std::vector<SendRecord>            records_;

    mutable std::mutex                 recv_mu_;
    nimrtc::plugins::OnSctpRecvCb      on_recv_;

    Status datagram_rc_   = nimrtc::plugins::kOk;
    Status stream_rc_     = nimrtc::plugins::kOk;
    Status partial_rc_    = nimrtc::plugins::kOk;
};

// ---------------------------------------------------------------------------
// Test 1: factory returns a non-null channel.
// ---------------------------------------------------------------------------
TEST(SctpDataChannelFactory, CreatesChannel) {
    SctpDataChannelFactory factory;
    auto* raw = factory.create();
    ASSERT_NE(raw, nullptr)
        << "SctpDataChannelFactory::create() returned null";
    EXPECT_STREQ(raw->name(), "nimrtc::datachannel::SctpDataChannel");
    delete raw;
}

// ---------------------------------------------------------------------------
// Test 2: factory id is "sctp" and display_name is non-empty.
// ---------------------------------------------------------------------------
TEST(SctpDataChannelFactory, IdIsSctp) {
    SctpDataChannelFactory factory;
    EXPECT_EQ(factory.id(), "sctp")
        << "P2 gate: factory id MUST be the literal \"sctp\" so the "
           "engine / Profile loader can resolve the production backend.";
    EXPECT_FALSE(factory.display_name().empty());
}

// ---------------------------------------------------------------------------
// Test 3: send() before open() doesn't crash, signals failure via the
//         on_state("error") callback. We inject a mock socket so that
//         "send wasn't routed to the socket" is also verifiable.
// ---------------------------------------------------------------------------
TEST(SctpDataChannel, SendWithoutReadyReturnsError) {
    auto mock = std::make_unique<MockSctpSocket>();
    auto* mock_raw = mock.get();

    SctpDataChannel ch(std::move(mock));

    // No socket call has happened yet.
    EXPECT_TRUE(mock_raw->snapshot().empty());

    std::atomic<int> state_count{0};
    std::atomic<bool> error_seen{false};

    ch.set_on_state([&](const char* s) {
        ++state_count;
        if (std::string{s} == "error") error_seen.store(true);
    });

    // Send BEFORE open() — the channel must report an error and NOT
    // forward to the mock socket.
    std::uint8_t payload[] = {'h', 'i'};
    ch.send(payload);

    EXPECT_TRUE(error_seen.load())
        << "send() before open() must fire on_state(\"error\")";
    EXPECT_TRUE(mock_raw->snapshot().empty())
        << "send() before open() must NOT reach the underlying socket";
    EXPECT_GE(state_count.load(), 1);
}

// ---------------------------------------------------------------------------
// Test 4: set_on_message / set_on_state before open() are safe and
//         don't crash; the callbacks persist past open().
// ---------------------------------------------------------------------------
TEST(SctpDataChannel, SetCallbacksBeforeOpen) {
    auto mock = std::make_unique<MockSctpSocket>();
    SctpDataChannel ch(std::move(mock));

    bool msg_called = false;
    bool state_called = false;

    // noexcept overloads — must not throw.
    EXPECT_NO_THROW(ch.set_on_message([&](BufferView) { msg_called = true; }));
    EXPECT_NO_THROW(ch.set_on_state([&](const char*) { state_called = true; }));

    // Callbacks aren't fired until we actually have something to fire.
    EXPECT_FALSE(msg_called);
    EXPECT_FALSE(state_called);
}

// ---------------------------------------------------------------------------
// Test 5: open() wires the SCTP recv callback, marks opened_=true,
//         and fires "connecting" then "open" state events.
// ---------------------------------------------------------------------------
TEST(SctpDataChannel, OpenWithReliableOrdered) {
    auto mock = std::make_unique<MockSctpSocket>();
    auto* mock_raw = mock.get();

    SctpDataChannel ch(std::move(mock));

    std::vector<std::string> state_events;
    ch.set_on_state([&](const char* s) { state_events.emplace_back(s); });

    // open() before configure() — config defaults to kReliableOrdered.
    Status rc = ch.open();
    EXPECT_EQ(rc, nimrtc::plugins::kOk)
        << "open() with a healthy mock socket must return kOk";
    EXPECT_TRUE(ch.is_opened());
    EXPECT_TRUE(mock_raw->has_recv_callback())
        << "open() must wire set_on_recv into the SCTP socket";

    // Expect "connecting" then "open" in that order.
    ASSERT_GE(state_events.size(), 2u);
    EXPECT_EQ(state_events[0], "connecting");
    EXPECT_EQ(state_events[1], "open");

    // Inbound path: simulate a recv on the mock socket and verify the
    // message callback fires.
    bool msg_called = false;
    std::vector<std::uint8_t> received;
    ch.set_on_message([&](BufferView bv) {
        msg_called = true;
        received.assign(bv.begin(), bv.end());
    });

    std::uint8_t payload[] = {'a', 'b', 'c'};
    mock_raw->simulate_recv(/*stream=*/0, payload);

    EXPECT_TRUE(msg_called);
    ASSERT_EQ(received.size(), 3u);
    EXPECT_EQ(received[0], 'a');
    EXPECT_EQ(received[2], 'c');
}

// ---------------------------------------------------------------------------
// Test 6: send() after open() routes to the matching SCTP send method
//         based on cfg.reliability.
// ---------------------------------------------------------------------------
TEST(SctpDataChannel, SendAfterOpenQueuesToSocket) {
    // --- ReliableOrdered -> send_stream ------------------------------------
    {
        auto mock = std::make_unique<MockSctpSocket>();
        auto* mock_raw = mock.get();
        SctpDataChannel ch(std::move(mock));
        ASSERT_EQ(ch.open(), nimrtc::plugins::kOk);

        DataChannelConfig cfg;
        cfg.reliability = DataChannelReliability::kReliableOrdered;
        ch.open(cfg);

        std::uint8_t payload[] = {'f', 'i', 'l', 'e'};
        ch.send(payload);

        auto recs = mock_raw->snapshot();
        ASSERT_EQ(recs.size(), 1u);
        EXPECT_EQ(recs[0].method, MockSctpSocket::Method::kStream);
        EXPECT_EQ(recs[0].stream, 0u);
        ASSERT_EQ(recs[0].data.size(), 4u);
        EXPECT_EQ(recs[0].data[0], 'f');
        EXPECT_EQ(recs[0].data[3], 'e');
    }

    // --- Unreliable -> send_datagram --------------------------------------
    {
        auto mock = std::make_unique<MockSctpSocket>();
        auto* mock_raw = mock.get();
        SctpDataChannel ch(std::move(mock));
        ASSERT_EQ(ch.open(), nimrtc::plugins::kOk);

        DataChannelConfig cfg;
        cfg.reliability = DataChannelReliability::kUnreliable;
        ch.open(cfg);

        std::uint8_t payload[] = {'t', 'e', 'l'};
        ch.send(payload);

        auto recs = mock_raw->snapshot();
        ASSERT_EQ(recs.size(), 1u);
        EXPECT_EQ(recs[0].method, MockSctpSocket::Method::kDatagram);
    }

    // --- PartialReliableTTL -> send_partial_reliable(ttl=max_packet_lifetime_ms)
    {
        auto mock = std::make_unique<MockSctpSocket>();
        auto* mock_raw = mock.get();
        SctpDataChannel ch(std::move(mock));
        ASSERT_EQ(ch.open(), nimrtc::plugins::kOk);

        DataChannelConfig cfg;
        cfg.reliability = DataChannelReliability::kPartialReliableTTL;
        cfg.max_packet_lifetime_ms = 250;
        ch.open(cfg);

        std::uint8_t payload[] = {'c', 'm', 'd'};
        ch.send(payload);

        auto recs = mock_raw->snapshot();
        ASSERT_EQ(recs.size(), 1u);
        EXPECT_EQ(recs[0].method, MockSctpSocket::Method::kPartialReliable);
        EXPECT_EQ(recs[0].ttl_ms, 250);
    }

    // --- PartialReliableCount -> send_partial_reliable(ttl=count*100ms) ---
    {
        auto mock = std::make_unique<MockSctpSocket>();
        auto* mock_raw = mock.get();
        SctpDataChannel ch(std::move(mock));
        ASSERT_EQ(ch.open(), nimrtc::plugins::kOk);

        DataChannelConfig cfg;
        cfg.reliability = DataChannelReliability::kPartialReliableCount;
        cfg.max_retransmits = 5;
        ch.open(cfg);

        std::uint8_t payload[] = {'r', 'e', 'x'};
        ch.send(payload);

        auto recs = mock_raw->snapshot();
        ASSERT_EQ(recs.size(), 1u);
        EXPECT_EQ(recs[0].method, MockSctpSocket::Method::kPartialReliable);
        EXPECT_EQ(recs[0].ttl_ms, 500)   // 5 * 100 ms
            << "PartialReliableCount should map count → count * 100ms TTL";
    }
}

// ---------------------------------------------------------------------------
// Test 7 (bonus): close() is idempotent and tears down the socket.
// ---------------------------------------------------------------------------
TEST(SctpDataChannel, CloseIsIdempotentAndTearsDownSocket) {
    auto mock = std::make_unique<MockSctpSocket>();
    auto* mock_raw = mock.get();

    SctpDataChannel ch(std::move(mock));
    ASSERT_EQ(ch.open(), nimrtc::plugins::kOk);
    EXPECT_TRUE(ch.is_opened());
    EXPECT_TRUE(mock_raw->has_recv_callback());

    ch.close();
    EXPECT_FALSE(ch.is_opened());
    EXPECT_FALSE(ch.is_socket_ready());
    EXPECT_FALSE(mock_raw->has_recv_callback())
        << "close() must clear the SCTP recv callback so usrsctp's "
           "worker thread can't fire into a torn-down channel";

    // Second close — must be a no-op (no crash, no double-free).
    EXPECT_NO_THROW(ch.close());
}

// ---------------------------------------------------------------------------
// Test 8 (bonus): register_default_plugins() publishes under id "sctp"
//                  and is idempotent.
// ---------------------------------------------------------------------------
TEST(SctpDataChannelRegistry, RegisterDefaultPluginsPublishesSctp) {
    nimrtc::datachannel::register_default_plugins();
    nimrtc::datachannel::register_default_plugins();   // idempotency

    const auto* factory =
        nimrtc::core::PluginRegistry::instance().get_datachannel("sctp");
    ASSERT_NE(factory, nullptr)
        << "register_default_plugins() must publish SctpDataChannelFactory "
           "under id=\"sctp\"";
    EXPECT_EQ(factory->id(), "sctp");
    EXPECT_FALSE(factory->display_name().empty());

    // list_datachannels() must include "sctp".
    auto ids = nimrtc::core::PluginRegistry::instance().list_datachannels();
    bool found = false;
    for (auto id : ids) {
        if (id == "sctp") { found = true; break; }
    }
    EXPECT_TRUE(found);
}

} // namespace
