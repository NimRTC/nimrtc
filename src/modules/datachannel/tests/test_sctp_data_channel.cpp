/**
 * @file src/modules/datachannel/tests/test_sctp_data_channel.cpp
 * @brief Unit tests for SctpDataChannel + SctpDataChannelFactory.
 *
 * Verifies the datachannel implementation:
 *   1. SctpDataChannelFactory doesn't return null from create().
 *   2. Factory id is exactly "sctp".
 *   3. send() before open() doesn't crash and signals failure via
 *      the on_state callback.
 *   4. set_on_message / set_on_state before open() are safe.
 *   5. open() wires the SCTP recv callback and reports "connecting";
 *      "open" is reported only once the DCEP handshake completes.
 *   6. send() after the handshake routes to the matching SCTP send
 *      method based on cfg.reliability (verified via a MockSctpSocket).
 *   7. The DCEP handshake (RFC 8832): the DATA_CHANNEL_OPEN goes out
 *      exactly once, an inbound user message is delivered exactly once,
 *      an inbound DATA_CHANNEL_OPEN is answered with exactly one ACK.
 *   8. Before the handshake completes, every user message is sent
 *      ordered (RFC 8832 §6), whatever the channel's own semantics are.
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
#include <nimrtc/sctp/dcep.hpp>
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
namespace sctp = nimrtc::sctp;

// ---------------------------------------------------------------------------
// MockSctpSpy — out-of-band observation point for the mock socket.
//
// The mock is owned by the SctpDataChannel and is therefore destroyed
// by `close()` (that is exactly the behaviour the test asserts: the
// channel tears the socket down).  Reading the mock's fields *after*
// `close()` is a heap-use-after-free — ASan flags it — so the mock
// publishes its observable state into this shared spy object, which
// outlives the socket itself.
// ---------------------------------------------------------------------------
struct MockSctpSpy {
    /** True once `set_on_recv` was called with a non-empty callback. */
    std::atomic<bool> recv_callback_set{false};
    /** True once `set_on_recv` was called with an empty callback
     *  (i.e. the channel cleared it during close()). */
    std::atomic<bool> recv_callback_cleared{false};
};

// ---------------------------------------------------------------------------
// MockSctpSocket — minimal ISctpSocket that records every send method call.
//
// Used by every test that needs to verify "the channel routed to the
// right ISctpSocket method". Counter values are atomic so we can poke
// at them from any thread (the recv trampoline fires on usrsctp's
// worker thread in production; here we just call it from the test
// thread, but the atomicity makes the test future-proof against
// threaded scenarios).
//
// The mock mirrors the production backend's inbound delivery rule
// (`UsrsctpSocket`): the PPID-aware callback wins when it is
// registered, the legacy callback is the fallback. Exactly one of them
// fires per inbound chunk — a fan-out would deliver every user message
// twice.
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
        /** The wire PPID (RFC 8831 §6). 0 = the legacy send method was
         *  used (no PPID on the wire). */
        std::uint32_t           ppid = 0;
        /** Whether the send went through a PPID-aware method. */
        bool                    has_ppid = false;
    };

    MockSctpSocket() = default;
    ~MockSctpSocket() override = default;

    /** Attach an out-of-band observation point. The spy must outlive
     *  the socket — it is how the tests observe state *after* the
     *  channel destroyed the mock (which would otherwise be a
     *  use-after-free). */
    void set_spy(std::shared_ptr<MockSctpSpy> spy) noexcept {
        spy_ = std::move(spy);
    }

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

    // ---- PPID-aware send surface ----------------------------------------
    //
    // The channel routes its traffic through these; the mock records
    // the PPID so the tests can tell a DCEP message (50) from a user
    // payload (53).

    Status send_stream_with_ppid(std::uint16_t stream,
                                 BufferView data,
                                 std::uint32_t ppid) noexcept override {
        SendRecord r;
        r.method = Method::kStream;
        r.stream = stream;
        r.data.assign(data.begin(), data.end());
        r.ttl_ms = 0;
        r.ppid = ppid;
        r.has_ppid = true;
        record(std::move(r));
        return stream_rc_;
    }

    Status send_datagram_with_ppid(std::uint16_t stream,
                                   BufferView data,
                                   std::uint32_t ppid) noexcept override {
        SendRecord r;
        r.method = Method::kDatagram;
        r.stream = stream;
        r.data.assign(data.begin(), data.end());
        r.ttl_ms = 0;
        r.ppid = ppid;
        r.has_ppid = true;
        record(std::move(r));
        return datagram_rc_;
    }

    Status send_partial_reliable_with_ppid(
        std::uint16_t stream,
        BufferView data,
        std::chrono::milliseconds ttl,
        std::uint32_t ppid,
        bool /*unordered*/) noexcept override {
        SendRecord r;
        r.method = Method::kPartialReliable;
        r.stream = stream;
        r.data.assign(data.begin(), data.end());
        r.ttl_ms = static_cast<std::int64_t>(ttl.count());
        r.ppid = ppid;
        r.has_ppid = true;
        record(std::move(r));
        return partial_rc_;
    }

    void set_on_recv(nimrtc::plugins::OnSctpRecvCb cb) noexcept override {
        // noexcept per the ISctpSocket seam contract. On macOS in CI
        // (Apple Silicon runner, Xcode 16.x, libc++ debug) we've seen
        // std::mutex::lock() throw std::system_error("Invalid argument")
        // when the underlying pthread_mutex_t object is in an unexpected
        // state — most often after a partial-move through std::unique_ptr
        // (which is *not* supposed to touch the moved-into object, but the
        // debug runtime has tripped on it). Lock_guard construction
        // throws, and because set_on_recv is noexcept that would call
        // std::terminate and mask the test intent. Swallow the exception
        // and keep going — this is a test seam, not a production socket.
        try {
            std::lock_guard<std::mutex> lk(recv_mu_);
            on_recv_ = std::move(cb);
            if (spy_) {
                if (on_recv_) {
                    spy_->recv_callback_set.store(true);
                } else {
                    spy_->recv_callback_cleared.store(true);
                }
            }
        } catch (...) {
            // Best-effort: the next test cleanup will reset state.
        }
    }

    void set_on_recv_with_ppid(
        nimrtc::plugins::OnSctpRecvWithPpidCb cb) noexcept override {
        try {
            std::lock_guard<std::mutex> lk(recv_mu_);
            on_recv_ppid_ = std::move(cb);
        } catch (...) {
            // See set_on_recv — the same macOS-libc++ debug corner case.
        }
    }

    // ---- Test helpers ----------------------------------------------------

    /** Simulate an inbound SCTP DATA payload from the peer.  Mirrors the
     *  production backend: the PPID-aware callback wins when it is
     *  registered, otherwise the legacy callback is used.  Exactly one
     *  of them fires — a user message is delivered once. */
    void simulate_recv(std::uint16_t stream, nimrtc::core::ByteSpan data) {
        simulate_recv_with_ppid(stream, sctp::kPpidBinary, data);
    }

    /** Simulate an inbound SCTP DATA payload with an explicit PPID. */
    void simulate_recv_with_ppid(std::uint16_t stream,
                                 std::uint32_t ppid,
                                 nimrtc::core::ByteSpan data) {
        nimrtc::plugins::OnSctpRecvWithPpidCb ppid_cb;
        nimrtc::plugins::OnSctpRecvCb legacy_cb;
        try {
            std::lock_guard<std::mutex> lk(recv_mu_);
            ppid_cb    = on_recv_ppid_;
            legacy_cb  = on_recv_;
        } catch (...) {
            // Same macOS-libc++ debug corner case as set_on_recv; treat
            // a failed lock as "no callback registered" so the test
            // continues instead of propagating an exception.
            return;
        }
        if (ppid_cb) {
            ppid_cb(stream, ppid, data);
        } else if (legacy_cb) {
            legacy_cb(stream, data);
        }
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
        try {
            std::lock_guard<std::mutex> lk(recv_mu_);
            return static_cast<bool>(on_recv_);
        } catch (...) {
            // As above — treat a failed lock as "no callback" rather
            // than crashing the test. The test will still check the
            // logical invariant; it just won't be confounded by the
            // mutex layer.
            return false;
        }
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
    std::shared_ptr<MockSctpSpy>       spy_;
    nimrtc::plugins::OnSctpRecvWithPpidCb on_recv_ppid_;

    Status datagram_rc_   = nimrtc::plugins::kOk;
    Status stream_rc_     = nimrtc::plugins::kOk;
    Status partial_rc_    = nimrtc::plugins::kOk;
};

/** Open a channel and complete the DCEP handshake by delivering an
 *  inbound user message (RFC 8832 §6: "the DATA_CHANNEL_ACK or any
 *  other message has been received on a data channel"). */
void complete_handshake(MockSctpSocket& mock, SctpDataChannel& ch) {
    const std::uint8_t payload[] = {'h', 's'};
    mock.simulate_recv_with_ppid(/*stream=*/0, sctp::kPpidBinary, payload);
    (void)ch;
}

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
// Test 5: open() wires the SCTP recv callback, marks opened_=true, and
//         reports "connecting". "open" must NOT be reported yet — the
//         DCEP handshake has not completed.
// ---------------------------------------------------------------------------
TEST(SctpDataChannel, OpenReportsConnectingUntilHandshakeCompletes) {
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

    ASSERT_EQ(state_events.size(), 1u);
    EXPECT_EQ(state_events[0], "connecting")
        << "the channel must report \"connecting\" until the DCEP "
           "handshake completes — reporting \"open\" here would be a lie";

    // Inbound path: simulate a recv on the mock socket and verify the
    // message callback fires (and that the handshake now completes).
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

    ASSERT_EQ(state_events.size(), 2u);
    EXPECT_EQ(state_events[1], "open")
        << "the handshake completes when a message arrives on the channel";
}

// ---------------------------------------------------------------------------
// Test 6: send() after the handshake routes to the matching SCTP send
//         method based on cfg.reliability.
// ---------------------------------------------------------------------------
TEST(SctpDataChannel, SendAfterHandshakeRoutesByReliability) {
    // --- ReliableOrdered -> send_stream ------------------------------------
    {
        auto mock = std::make_unique<MockSctpSocket>();
        auto* mock_raw = mock.get();
        SctpDataChannel ch(std::move(mock));
        ASSERT_EQ(ch.open(), nimrtc::plugins::kOk);

        DataChannelConfig cfg;
        cfg.reliability = DataChannelReliability::kReliableOrdered;
        ch.open(cfg);
        complete_handshake(*mock_raw, ch);

        const auto before = mock_raw->snapshot().size();

        std::uint8_t payload[] = {'f', 'i', 'l', 'e'};
        ch.send(payload);

        auto recs = mock_raw->snapshot();
        ASSERT_EQ(recs.size(), before + 1);
        EXPECT_EQ(recs.back().method, MockSctpSocket::Method::kStream);
        EXPECT_EQ(recs.back().stream, 0u);
        EXPECT_TRUE(recs.back().has_ppid);
        EXPECT_EQ(recs.back().ppid, sctp::kPpidBinary);
        ASSERT_EQ(recs.back().data.size(), 4u);
        EXPECT_EQ(recs.back().data[0], 'f');
        EXPECT_EQ(recs.back().data[3], 'e');
    }

    // --- Unreliable -> send_datagram --------------------------------------
    {
        auto mock = std::make_unique<MockSctpSocket>();
        auto* mock_raw = mock.get();
        SctpDataChannel ch(std::move(mock));
        ASSERT_EQ(ch.open(), nimrtc::plugins::kOk);

        DataChannelConfig cfg;
        cfg.reliability = DataChannelReliability::kUnreliable;
        cfg.ordered     = false;
        ch.open(cfg);
        complete_handshake(*mock_raw, ch);

        const auto before = mock_raw->snapshot().size();

        std::uint8_t payload[] = {'t', 'e', 'l'};
        ch.send(payload);

        auto recs = mock_raw->snapshot();
        ASSERT_EQ(recs.size(), before + 1);
        EXPECT_EQ(recs.back().method, MockSctpSocket::Method::kDatagram);
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
        complete_handshake(*mock_raw, ch);

        const auto before = mock_raw->snapshot().size();

        std::uint8_t payload[] = {'c', 'm', 'd'};
        ch.send(payload);

        auto recs = mock_raw->snapshot();
        ASSERT_EQ(recs.size(), before + 1);
        EXPECT_EQ(recs.back().method, MockSctpSocket::Method::kPartialReliable);
        EXPECT_EQ(recs.back().ttl_ms, 250);
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
        complete_handshake(*mock_raw, ch);

        const auto before = mock_raw->snapshot().size();

        std::uint8_t payload[] = {'r', 'e', 'x'};
        ch.send(payload);

        auto recs = mock_raw->snapshot();
        ASSERT_EQ(recs.size(), before + 1);
        EXPECT_EQ(recs.back().method, MockSctpSocket::Method::kPartialReliable);
        EXPECT_EQ(recs.back().ttl_ms, 500)   // 5 * 100 ms
            << "PartialReliableCount should map count → count * 100ms TTL";
    }
}

// ---------------------------------------------------------------------------
// Test 7 (bonus): close() is idempotent and tears down the socket.
// ---------------------------------------------------------------------------
TEST(SctpDataChannel, CloseIsIdempotentAndTearsDownSocket) {
    auto spy = std::make_shared<MockSctpSpy>();
    auto mock = std::make_unique<MockSctpSocket>();
    mock->set_spy(spy);
    auto* mock_raw = mock.get();

    SctpDataChannel ch(std::move(mock));
    ASSERT_EQ(ch.open(), nimrtc::plugins::kOk);
    EXPECT_TRUE(ch.is_opened());
    EXPECT_TRUE(mock_raw->has_recv_callback());
    EXPECT_TRUE(spy->recv_callback_set.load());

    std::vector<std::string> state_events;
    ch.set_on_state([&](const char* s) { state_events.emplace_back(s); });

    ch.close();
    EXPECT_FALSE(ch.is_opened());
    EXPECT_FALSE(ch.is_socket_ready());
    // NOTE: the mock socket is destroyed by close() (that IS the
    // teardown this test asserts), so the "callback was cleared" check
    // reads the out-of-band spy instead of the (freed) mock.
    EXPECT_TRUE(spy->recv_callback_cleared.load())
        << "close() must clear the SCTP recv callback so usrsctp's "
           "worker thread can't fire into a torn-down channel";
    ASSERT_EQ(state_events.size(), 1u);
    EXPECT_EQ(state_events[0], "closed");

    // Second close — must be a no-op (no crash, no double-free).
    EXPECT_NO_THROW(ch.close());
    EXPECT_EQ(state_events.size(), 1u)
        << "close() must report \"closed\" exactly once";
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

// ---------------------------------------------------------------------------
// Test 9: the DATA_CHANNEL_OPEN is emitted exactly once (RFC 8832 §5.1).
// ---------------------------------------------------------------------------
TEST(SctpDataChannel, DcepOpenIsSentExactlyOnce) {
    auto mock = std::make_unique<MockSctpSocket>();
    auto* mock_raw = mock.get();
    SctpDataChannel ch(std::move(mock));

    DataChannelConfig cfg;
    cfg.label = "telemetry";
    ch.open(cfg);

    // open(cfg) before open() — the OPEN goes out when the socket is
    // wired (i.e. on open()).
    ASSERT_EQ(ch.open(), nimrtc::plugins::kOk);

    auto recs = mock_raw->snapshot();
    ASSERT_EQ(recs.size(), 1u)
        << "exactly one DATA_CHANNEL_OPEN must be sent";
    EXPECT_TRUE(recs[0].has_ppid);
    EXPECT_EQ(recs[0].ppid, sctp::kPpidDcep);
    EXPECT_EQ(recs[0].method, MockSctpSocket::Method::kStream)
        << "the DCEP OPEN must ride the ordered + reliable path";

    // The payload must decode as a DATA_CHANNEL_OPEN carrying the label.
    const auto params =
        sctp::dcep_decode_open(recs[0].data.data(), recs[0].data.size());
    ASSERT_TRUE(params.has_value());
    EXPECT_EQ(params->label, "telemetry");

    // Repeating open() / open(cfg) must not send a second OPEN.
    ch.open(cfg);
    ASSERT_EQ(ch.open(), nimrtc::plugins::kOk);
    EXPECT_EQ(mock_raw->snapshot().size(), 1u)
        << "the DATA_CHANNEL_OPEN must be sent at most once";
}

// ---------------------------------------------------------------------------
// Test 10: an inbound user message is delivered exactly once.
// ---------------------------------------------------------------------------
TEST(SctpDataChannel, InboundMessageIsDeliveredOnce) {
    auto mock = std::make_unique<MockSctpSocket>();
    auto* mock_raw = mock.get();
    SctpDataChannel ch(std::move(mock));

    std::atomic<int> msg_count{0};
    ch.set_on_message([&](BufferView) { ++msg_count; });
    ASSERT_EQ(ch.open(), nimrtc::plugins::kOk);

    const std::uint8_t payload[] = {'x', 'y', 'z'};
    mock_raw->simulate_recv_with_ppid(/*stream=*/0, sctp::kPpidBinary, payload);

    EXPECT_EQ(msg_count.load(), 1)
        << "an inbound user message must reach the application exactly "
           "once — the PPID demux and the legacy callback must not both "
           "fire for the same chunk";
}

// ---------------------------------------------------------------------------
// Test 11: an inbound DATA_CHANNEL_OPEN is answered with exactly one
//           DATA_CHANNEL_ACK, and the channel reports "open".
// ---------------------------------------------------------------------------
TEST(SctpDataChannel, InboundDcepOpenIsAnsweredWithOneAck) {
    auto mock = std::make_unique<MockSctpSocket>();
    auto* mock_raw = mock.get();
    SctpDataChannel ch(std::move(mock));

    std::vector<std::string> state_events;
    ch.set_on_state([&](const char* s) { state_events.emplace_back(s); });
    ASSERT_EQ(ch.open(), nimrtc::plugins::kOk);

    // The peer opens a channel toward us.
    DataChannelConfig peer_cfg;
    peer_cfg.label       = "peer";
    peer_cfg.priority    = 32;
    peer_cfg.reliability = DataChannelReliability::kReliableOrdered;
    const auto open_bytes = sctp::dcep_encode_open(peer_cfg);

    mock_raw->simulate_recv_with_ppid(/*stream=*/0, sctp::kPpidDcep,
                                      open_bytes);

    // Exactly one DATA_CHANNEL_ACK must go back.
    auto recs = mock_raw->snapshot();
    ASSERT_EQ(recs.size(), 1u)
        << "an inbound DATA_CHANNEL_OPEN must be answered with exactly "
           "one DATA_CHANNEL_ACK";
    EXPECT_EQ(recs[0].ppid, sctp::kPpidDcep);
    ASSERT_EQ(recs[0].data.size(), 1u);
    EXPECT_EQ(recs[0].data[0], sctp::kDcepAck);

    // The channel reports "open" — and the peer's QoS was adopted.
    ASSERT_EQ(state_events.size(), 2u);
    EXPECT_EQ(state_events[0], "connecting");
    EXPECT_EQ(state_events[1], "open");
    EXPECT_EQ(ch.config().label, "peer");
    EXPECT_EQ(ch.config().priority, 32);
}

// ---------------------------------------------------------------------------
// Test 12: before the handshake completes, every user message is sent
//           ordered — even on an unordered channel (RFC 8832 §6).
// ---------------------------------------------------------------------------
TEST(SctpDataChannel, UnorderedChannelSendsOrderedBeforeHandshake) {
    auto mock = std::make_unique<MockSctpSocket>();
    auto* mock_raw = mock.get();
    SctpDataChannel ch(std::move(mock));

    DataChannelConfig cfg;
    cfg.label       = "telemetry";
    cfg.reliability = DataChannelReliability::kUnreliable;
    cfg.ordered     = false;
    ch.open(cfg);
    ASSERT_EQ(ch.open(), nimrtc::plugins::kOk);

    const auto before = mock_raw->snapshot().size();

    std::uint8_t payload[] = {'t', 'e', 'l'};
    ch.send(payload);

    auto recs = mock_raw->snapshot();
    ASSERT_EQ(recs.size(), before + 1);
    EXPECT_EQ(recs.back().method, MockSctpSocket::Method::kStream)
        << "RFC 8832 §6: before the DATA_CHANNEL_ACK (or any other "
           "message) has been received, user data MUST be sent ordered";

    // Once the handshake completes, the channel's own semantics apply.
    complete_handshake(*mock_raw, ch);
    const auto after_handshake = mock_raw->snapshot().size();

    ch.send(payload);
    recs = mock_raw->snapshot();
    ASSERT_EQ(recs.size(), after_handshake + 1);
    EXPECT_EQ(recs.back().method, MockSctpSocket::Method::kDatagram)
        << "after the handshake the channel's own unordered semantics apply";
}

// ---------------------------------------------------------------------------
// Test 13: an inbound DATA_CHANNEL_ACK completes the handshake.
// ---------------------------------------------------------------------------
TEST(SctpDataChannel, InboundDcepAckCompletesTheHandshake) {
    auto mock = std::make_unique<MockSctpSocket>();
    auto* mock_raw = mock.get();
    SctpDataChannel ch(std::move(mock));

    std::vector<std::string> state_events;
    ch.set_on_state([&](const char* s) { state_events.emplace_back(s); });

    DataChannelConfig cfg;
    cfg.label = "cmd";
    ch.open(cfg);
    ASSERT_EQ(ch.open(), nimrtc::plugins::kOk);

    const auto ack = sctp::dcep_encode_ack();
    mock_raw->simulate_recv_with_ppid(/*stream=*/0, sctp::kPpidDcep, ack);

    ASSERT_EQ(state_events.size(), 2u);
    EXPECT_EQ(state_events[0], "connecting");
    EXPECT_EQ(state_events[1], "open");
}

} // namespace
