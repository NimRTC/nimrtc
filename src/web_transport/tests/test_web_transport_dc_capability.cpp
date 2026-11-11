/**
 * @file test_web_transport_data_channel.cpp
 * @brief Regression tests for the v1.2.0 data-channel capability work
 *        packages (WP-B / WP-C / WP-D / WP-E).
 *
 * The tests drive `WebTransportDataChannel` against an in-process mock
 * `IWebTransportSession` that implements the same inbound contract as
 * the msquic session:
 *
 *   - the *multicast* inbound surface (`subscribe_on_datagram` /
 *     `subscribe_on_peer_stream`) — every subscriber sees the traffic
 *     and picks what belongs to it (WP-C2);
 *   - the *legacy* single-slot surface (`set_on_datagram` /
 *     `set_on_peer_stream`) — used by the raw framing mode;
 *   - the *outbound priority* surface (`register_priority_source` /
 *     `flush_outbound`) — the session drains the pending sends of the
 *     channels in priority order (WP-E2).
 *
 * The mock loops the traffic back: what a channel sends on its stream
 * is delivered to the session's peer-stream subscribers, exactly like a
 * peer's data arriving on a peer-initiated stream.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nimrtc/plugins/base.hpp>
#include <nimrtc/plugins/datachannel.hpp>
#include <nimrtc/web_transport/bidi_stream_iface.hpp>
#include <nimrtc/web_transport/web_transport_data_channel.hpp>
#include <nimrtc/web_transport/web_transport_session_iface.hpp>
#include <nimrtc/web_transport/web_transport_state.hpp>

namespace {

using nimrtc::plugins::BufferView;
using nimrtc::plugins::DataChannelConfig;
using nimrtc::plugins::DataChannelFraming;
using nimrtc::plugins::DataChannelMessage;
using nimrtc::plugins::DataChannelReliability;
using nimrtc::web_transport::IBidirectionalStream;
using nimrtc::web_transport::IWebTransportSession;
using nimrtc::web_transport::OnDatagramCb;
using nimrtc::web_transport::OnPeerStreamCb;
using nimrtc::web_transport::PriorityFlush;
using nimrtc::web_transport::SubscriptionId;
using nimrtc::web_transport::WebTransportDataChannel;
using nimrtc::web_transport::WebTransportState;

// ---------------------------------------------------------------------------
// MockSession — an in-process IWebTransportSession with a loopback wire.
// ---------------------------------------------------------------------------

class MockSession;

class MockStream final : public IBidirectionalStream {
public:
    MockStream(bool peer) noexcept : peer_(peer) {}

    nimrtc::plugins::Status send(
        std::span<const std::uint8_t> bytes) noexcept override {
        if (peer_) return nimrtc::plugins::kOk;
        // The owner refuses the bytes when the test asks it to — the
        // channel must then requeue the message (WP-D).  The hook is
        // installed by the owner (MockSession is incomplete at this
        // point, so the call cannot be made directly).
        if (!send_hook) return nimrtc::plugins::kErrNotReady;
        return send_hook(bytes);
    }

    nimrtc::plugins::Status recv(std::span<std::uint8_t> out) noexcept override {
        (void)out;
        return nimrtc::plugins::kOk;
    }

    void set_on_data(nimrtc::web_transport::OnStreamDataCb cb) noexcept override {
        primary_ = std::move(cb);
    }

    nimrtc::web_transport::StreamSubscriptionId subscribe_on_data(
        nimrtc::web_transport::OnStreamDataCb cb) noexcept override {
        if (!cb) return 0;
        const auto id = next_sub_id_++;
        subs_.push_back(Subscription{id, std::move(cb)});
        return id;
    }

    void unsubscribe_on_data(
        nimrtc::web_transport::StreamSubscriptionId id) noexcept override {
        for (auto it = subs_.begin(); it != subs_.end(); ++it) {
            if (it->id == id) {
                subs_.erase(it);
                return;
            }
        }
    }

    void close() noexcept override {}
    void reset() noexcept override {}

    /** The hook the owner installs to observe (and refuse) the sends. */
    std::function<nimrtc::plugins::Status(
        std::span<const std::uint8_t>)> send_hook;

    /** Deliver bytes to the subscribers of this stream (the peer side). */
    void deliver(std::span<const std::uint8_t> bytes) {
        if (primary_) {
            primary_(std::span<const std::uint8_t>{bytes.data(), bytes.size()});
        }
        for (const auto& s : subs_) {
            if (s.cb) s.cb(std::span<const std::uint8_t>{bytes.data(), bytes.size()});
        }
    }

private:
    struct Subscription {
        nimrtc::web_transport::StreamSubscriptionId id = 0;
        nimrtc::web_transport::OnStreamDataCb cb;
    };

    bool peer_ = false;
    nimrtc::web_transport::OnStreamDataCb primary_;
    std::vector<Subscription> subs_;
    nimrtc::web_transport::StreamSubscriptionId next_sub_id_ = 1;
};

class MockSession final : public IWebTransportSession {
public:
    // ---- IWebTransportSession ----
    WebTransportState state() const noexcept override {
        return WebTransportState::kConnected;
    }

    IBidirectionalStream* open_bidi_stream() noexcept override {
        auto s = std::make_unique<MockStream>(/*peer=*/false);
        MockStream* raw = s.get();
        raw->send_hook = [this](std::span<const std::uint8_t> bytes) {
            return on_stream_send(bytes);
        };
        streams_.push_back(std::move(s));
        return raw;
    }

    nimrtc::web_transport::IUnidirectionalStream* open_uni_stream() noexcept override {
        return nullptr;
    }

    nimrtc::plugins::Status send_datagram(
        std::span<const std::uint8_t> bytes) noexcept override {
        if (fail_sends_) return nimrtc::plugins::kErrNotReady;
        datagrams_.emplace_back(bytes.begin(), bytes.end());
        deliver_datagram(bytes);
        return nimrtc::plugins::kOk;
    }

    void set_on_datagram(OnDatagramCb cb) noexcept override {
        legacy_datagram_ = std::move(cb);
    }

    void set_on_peer_stream(OnPeerStreamCb cb) noexcept override {
        legacy_peer_stream_ = std::move(cb);
    }

    SubscriptionId subscribe_on_datagram(OnDatagramCb cb) noexcept override {
        if (!cb) return 0;
        const SubscriptionId id = next_id_++;
        datagram_subs_.push_back({id, std::move(cb)});
        return id;
    }

    void unsubscribe_on_datagram(SubscriptionId id) noexcept override {
        for (auto it = datagram_subs_.begin(); it != datagram_subs_.end(); ++it) {
            if (it->id == id) {
                datagram_subs_.erase(it);
                return;
            }
        }
    }

    SubscriptionId subscribe_on_peer_stream(OnPeerStreamCb cb) noexcept override {
        if (!cb) return 0;
        const SubscriptionId id = next_id_++;
        peer_subs_.push_back({id, std::move(cb)});
        // A subscriber that registers after the peer stream was already
        // accepted must still see it — otherwise a channel created late
        // would never learn about the inbound stream.
        if (peer_stream_ && peer_subs_.back().cb) {
            peer_subs_.back().cb(peer_stream_.get());
        }
        return id;
    }

    void unsubscribe_on_peer_stream(SubscriptionId id) noexcept override {
        for (auto it = peer_subs_.begin(); it != peer_subs_.end(); ++it) {
            if (it->id == id) {
                peer_subs_.erase(it);
                return;
            }
        }
    }

    SubscriptionId register_priority_source(int priority,
                                            PriorityFlush flush) noexcept override {
        if (!flush) return 0;
        const SubscriptionId id = next_id_++;
        priorities_.push_back({id, priority, std::move(flush)});
        priority_values_.push_back(priority);
        return id;
    }

    void unregister_priority_source(SubscriptionId id) noexcept override {
        for (auto it = priorities_.begin(); it != priorities_.end(); ++it) {
            if (it->id == id) {
                priorities_.erase(it);
                break;
            }
        }
        priority_values_.clear();
        for (const auto& p : priorities_) priority_values_.push_back(p.priority);
    }

    void flush_outbound() noexcept override {
        // Lower priority value = offered to the transport first.
        std::stable_sort(priorities_.begin(), priorities_.end(),
                         [](const PrioritySource& a, const PrioritySource& b) {
                             return a.priority < b.priority;
                         });
        for (const auto& p : priorities_) {
            if (!p.flush) continue;
            flush_order_.push_back(p.priority);
            p.flush();
        }
    }

    void close() noexcept override {}

    // ---- Test control ----

    /** Make the next stream sends fail (transport not ready). */
    void set_fail_stream_sends(bool fail) noexcept { fail_sends_ = fail; }

    /** Deliver the bytes to the peer stream in fixed-size chunks. */
    void set_chunk_size(std::size_t n) noexcept { chunk_size_ = n; }

    /** Connect this session to a peer session: the bytes a local
     *  stream offers are delivered to the peer's peer-stream, exactly
     *  like a message that arrives from a remote peer.  Without a peer
     *  the session loops the traffic back to itself. */
    void set_peer(MockSession* peer) noexcept { peer_ = peer; }

    /** Drop the next `n` ARQ data frames — the frames that carry a
     *  sequence number (v1.2.0, WP-D4).  The bytes are accepted by the
     *  transport and then lost, so the sender's retransmission timer
     *  fires. */
    void drop_next_arq_data(int n) noexcept { drop_arq_data_ = n; }

    /** Drop the next `n` acknowledgement frames. */
    void drop_next_arq_ack(int n) noexcept { drop_arq_ack_ = n; }

    /** The chunks that were offered to the transport, in order. */
    const std::vector<std::vector<std::uint8_t>>& sent_chunks() const noexcept {
        return sent_chunks_;
    }

    /** The priorities of the flush callbacks, in the order they ran. */
    const std::vector<int>& flush_order() const noexcept { return flush_order_; }

    /** The registered priority sources, in registration order. */
    const std::vector<int>& priorities() const noexcept { return priority_values_; }

    /** The number of live peer-stream subscriptions. */
    std::size_t peer_subscriber_count() const noexcept { return peer_subs_.size(); }

    /** The number of live datagram subscriptions. */
    std::size_t datagram_subscriber_count() const noexcept {
        return datagram_subs_.size();
    }

    /** Simulate a peer-initiated stream carrying `bytes`. */
    void deliver_peer_stream(const std::vector<std::uint8_t>& bytes) {
        if (!peer_stream_) {
            peer_stream_ = std::make_unique<MockStream>(/*peer=*/true);
            MockStream* raw = peer_stream_.get();
            if (legacy_peer_stream_) legacy_peer_stream_(raw);
            for (const auto& sub : peer_subs_) {
                if (sub.cb) sub.cb(raw);
            }
        }
        const std::size_t chunk = (chunk_size_ == 0) ? bytes.size() : chunk_size_;
        for (std::size_t off = 0; off < bytes.size(); off += chunk) {
            const std::size_t n = std::min(chunk, bytes.size() - off);
            peer_stream_->deliver(
                std::span<const std::uint8_t>{bytes.data() + off, n});
        }
    }

    /** Called by a local stream when it is offered bytes.  Returns
     *  `kErrNotReady` when the test asked the transport to refuse. */
    nimrtc::plugins::Status on_stream_send(
        std::span<const std::uint8_t> bytes) {
        sent_chunks_.emplace_back(bytes.begin(), bytes.end());
        if (fail_sends_) return nimrtc::plugins::kErrNotReady;

        // The loss simulation reads the frame header (v1.2.0, WP-D4):
        // the kind is byte 0 (1 = data, 2 = ack) and the sequence
        // number is bytes 4..7.
        if (bytes.size() >= 8) {
            const std::uint8_t kind = bytes[0];
            const std::uint32_t seq =
                (static_cast<std::uint32_t>(bytes[4]) << 24) |
                (static_cast<std::uint32_t>(bytes[5]) << 16) |
                (static_cast<std::uint32_t>(bytes[6]) << 8) |
                static_cast<std::uint32_t>(bytes[7]);
            if (kind == 1 && seq != 0 && drop_arq_data_ > 0) {
                --drop_arq_data_;
                return nimrtc::plugins::kOk;  // accepted, then lost
            }
            if (kind == 2 && drop_arq_ack_ > 0) {
                --drop_arq_ack_;
                return nimrtc::plugins::kOk;  // accepted, then lost
            }
        }

        const std::vector<std::uint8_t> copy(bytes.begin(), bytes.end());
        if (peer_) {
            peer_->deliver_peer_stream(copy);
            return nimrtc::plugins::kOk;
        }
        deliver_peer_stream(copy);
        return nimrtc::plugins::kOk;
    }

private:
    struct DatagramSub {
        SubscriptionId id = 0;
        OnDatagramCb cb;
    };
    struct PeerSub {
        SubscriptionId id = 0;
        OnPeerStreamCb cb;
    };
    struct PrioritySource {
        SubscriptionId id = 0;
        int priority = 0;
        PriorityFlush flush;
    };

    void deliver_datagram(std::span<const std::uint8_t> bytes) {
        if (legacy_datagram_) legacy_datagram_(bytes);
        for (const auto& sub : datagram_subs_) {
            if (sub.cb) sub.cb(bytes);
        }
    }

    SubscriptionId next_id_ = 1;
    std::vector<std::unique_ptr<MockStream>> streams_;
    std::unique_ptr<MockStream> peer_stream_;
    std::vector<DatagramSub> datagram_subs_;
    std::vector<PeerSub> peer_subs_;
    std::vector<PrioritySource> priorities_;
    /** The priorities of the live sources (mirrors `priorities_`). */
    std::vector<int> priority_values_;
    std::vector<int> flush_order_;
    std::vector<std::vector<std::uint8_t>> sent_chunks_;
    std::vector<std::vector<std::uint8_t>> datagrams_;
    OnDatagramCb legacy_datagram_;
    OnPeerStreamCb legacy_peer_stream_;
    MockSession* peer_ = nullptr;
    int drop_arq_data_ = 0;
    int drop_arq_ack_ = 0;
    bool fail_sends_ = false;
    std::size_t chunk_size_ = 0;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

DataChannelConfig make_cfg(std::string label,
                           DataChannelReliability rel =
                               DataChannelReliability::kReliableOrdered) {
    DataChannelConfig cfg;
    cfg.label = std::move(label);
    cfg.reliability = rel;
    cfg.framing = DataChannelFraming::kFramed;
    return cfg;
}

/** Collects the messages a channel receives. */
struct Collector {
    std::vector<std::vector<std::uint8_t>> payloads;
    std::vector<std::string> labels;
    std::vector<std::string> states;

    void attach(WebTransportDataChannel& ch) {
        ch.set_on_message([this](BufferView data) {
            payloads.emplace_back(
                reinterpret_cast<const std::uint8_t*>(data.data()),
                reinterpret_cast<const std::uint8_t*>(data.data()) + data.size());
        });
        ch.set_on_message_ex([this](const DataChannelMessage& m) {
            labels.emplace_back(m.label);
        });
        ch.set_on_state([this](const char* s) { states.emplace_back(s); });
    }
};

// ===========================================================================
// WP-B — message boundary
// ===========================================================================

TEST(WebTransportDataChannelFraming, TwoSendsSurfaceAsTwoMessages) {
    MockSession session;
    WebTransportDataChannel channel(&session);
    Collector c;
    c.attach(channel);

    DataChannelConfig cfg = make_cfg("a");
    channel.open(cfg);
    ASSERT_EQ(channel.open(), nimrtc::plugins::kOk);

    const std::vector<std::uint8_t> a(10, 0xA1);
    const std::vector<std::uint8_t> b(10, 0xB2);
    channel.send(BufferView{a.data(), a.size()});
    channel.send(BufferView{b.data(), b.size()});

    ASSERT_EQ(c.payloads.size(), 2u);
    EXPECT_EQ(c.payloads[0].size(), 10u);
    EXPECT_EQ(c.payloads[1].size(), 10u);
    EXPECT_EQ(c.payloads[0][0], 0xA1);
    EXPECT_EQ(c.payloads[1][0], 0xB2);
}

TEST(WebTransportDataChannelFraming, OneBigSendSurfacesAsOneMessage) {
    MockSession session;
    WebTransportDataChannel channel(&session);
    Collector c;
    c.attach(channel);

    channel.open(make_cfg("a"));
    ASSERT_EQ(channel.open(), nimrtc::plugins::kOk);

    const std::vector<std::uint8_t> big(10000, 0x5A);
    channel.send(BufferView{big.data(), big.size()});

    ASSERT_EQ(c.payloads.size(), 1u);
    EXPECT_EQ(c.payloads[0].size(), 10000u);
}

TEST(WebTransportDataChannelFraming, MessageSplitAcrossChunksIsReassembled) {
    MockSession session;
    session.set_chunk_size(1);  // one byte per callback
    WebTransportDataChannel channel(&session);
    Collector c;
    c.attach(channel);

    channel.open(make_cfg("a"));
    ASSERT_EQ(channel.open(), nimrtc::plugins::kOk);

    const std::vector<std::uint8_t> payload(64, 0x7E);
    channel.send(BufferView{payload.data(), payload.size()});

    ASSERT_EQ(c.payloads.size(), 1u);
    EXPECT_EQ(c.payloads[0].size(), 64u);
    EXPECT_EQ(c.payloads[0], payload);
}

TEST(WebTransportDataChannelFraming, OversizedFrameDrivesTheChannelToError) {
    MockSession session;
    WebTransportDataChannel channel(&session);
    Collector c;
    c.attach(channel);

    channel.open(make_cfg("a"));
    ASSERT_EQ(channel.open(), nimrtc::plugins::kOk);

    // A frame whose payload length prefix exceeds the negotiated maximum
    // is a protocol error: the channel must not allocate the buffer.
    //
    // The frame header (v1.2.0, WP-D4): kind | flags | reserved |
    // seq | label_len | payload_len.
    const std::vector<std::uint8_t> bogus = {
        0x01, 0x00, 0x00, 0x00,        // kind = data, flags, reserved
        0x00, 0x00, 0x00, 0x00,        // seq
        0x00, 0x00, 0x00, 0x01,        // label_len = 1
        0xFF, 0xFF, 0xFF, 0xFF,        // payload_len = 0xFFFFFFFF
        'a'};
    session.deliver_peer_stream(bogus);

    EXPECT_TRUE(c.payloads.empty());
    const bool saw_error =
        std::find(c.states.begin(), c.states.end(), "error") != c.states.end();
    EXPECT_TRUE(saw_error);
}

// ===========================================================================
// WebTransport stream-type header (the inbound stream header stripping)
// ===========================================================================

TEST(WebTransportDataChannelHeader, SingleByteSignalHeaderIsStripped) {
    MockSession session;
    WebTransportDataChannel channel(&session);
    Collector c;
    c.attach(channel);

    DataChannelConfig cfg = make_cfg("a");
    cfg.framing = DataChannelFraming::kRaw;
    channel.open(cfg);
    ASSERT_EQ(channel.open(), nimrtc::plugins::kOk);

    // The signal 0x41 followed by the session-id varint 0, then the
    // payload: the header must be stripped and the payload delivered.
    session.deliver_peer_stream({0x41, 0x00, 'h', 'i'});

    ASSERT_EQ(c.payloads.size(), 1u);
    EXPECT_EQ(c.payloads[0], (std::vector<std::uint8_t>{'h', 'i'}));
}

TEST(WebTransportDataChannelHeader, CanonicalVarintSignalHeaderIsStripped) {
    MockSession session;
    WebTransportDataChannel channel(&session);
    Collector c;
    c.attach(channel);

    DataChannelConfig cfg = make_cfg("a");
    cfg.framing = DataChannelFraming::kRaw;
    channel.open(cfg);
    ASSERT_EQ(channel.open(), nimrtc::plugins::kOk);

    // The canonical QUIC varint encoding of 0x41 is the two bytes
    // 0x40 0x41 (0x41 > 63, so a one-byte varint cannot carry it),
    // followed by the session-id varint 0.
    session.deliver_peer_stream({0x40, 0x41, 0x00, 'h', 'i'});

    ASSERT_EQ(c.payloads.size(), 1u);
    EXPECT_EQ(c.payloads[0], (std::vector<std::uint8_t>{'h', 'i'}));
}

TEST(WebTransportDataChannelHeader, HeaderSplitAcrossChunksIsStripped) {
    MockSession session;
    session.set_chunk_size(1);  // one byte per callback
    WebTransportDataChannel channel(&session);
    Collector c;
    c.attach(channel);

    DataChannelConfig cfg = make_cfg("a");
    cfg.framing = DataChannelFraming::kRaw;
    channel.open(cfg);
    ASSERT_EQ(channel.open(), nimrtc::plugins::kOk);

    // The header arrives one byte at a time: the probe must wait for
    // the whole header instead of delivering the header bytes.  Raw
    // mode has no message boundary, so the payload surfaces as two
    // single-byte messages — what matters is that none of the header
    // bytes leaked into the payload.
    session.deliver_peer_stream({0x40, 0x41, 0x00, 'h', 'i'});

    std::vector<std::uint8_t> received;
    for (const auto& p : c.payloads) {
        received.insert(received.end(), p.begin(), p.end());
    }
    EXPECT_EQ(received, (std::vector<std::uint8_t>{'h', 'i'}));
}

TEST(WebTransportDataChannelHeader, StreamWithoutHeaderIsDeliveredAsIs) {
    MockSession session;
    WebTransportDataChannel channel(&session);
    Collector c;
    c.attach(channel);

    DataChannelConfig cfg = make_cfg("a");
    cfg.framing = DataChannelFraming::kRaw;
    channel.open(cfg);
    ASSERT_EQ(channel.open(), nimrtc::plugins::kOk);

    // A NimRTC peer in raw mode sends the payload verbatim — there is
    // no WebTransport header to strip.
    session.deliver_peer_stream({'h', 'i'});

    ASSERT_EQ(c.payloads.size(), 1u);
    EXPECT_EQ(c.payloads[0], (std::vector<std::uint8_t>{'h', 'i'}));
}

// ===========================================================================
// WP-C — multi-channel isolation
// ===========================================================================

TEST(WebTransportDataChannelIsolation, MessageGoesToItsOwnChannelOnly) {
    MockSession session;
    WebTransportDataChannel a(&session);
    WebTransportDataChannel b(&session);
    Collector ca;
    Collector cb;
    ca.attach(a);
    cb.attach(b);

    a.open(make_cfg("a"));
    b.open(make_cfg("b"));
    ASSERT_EQ(a.open(), nimrtc::plugins::kOk);
    ASSERT_EQ(b.open(), nimrtc::plugins::kOk);

    const std::vector<std::uint8_t> payload(8, 0x11);
    a.send(BufferView{payload.data(), payload.size()});

    EXPECT_EQ(ca.payloads.size(), 1u);
    EXPECT_EQ(cb.payloads.size(), 0u);
    EXPECT_EQ(ca.labels.size(), 1u);
    EXPECT_EQ(ca.labels[0], "a");
}

TEST(WebTransportDataChannelIsolation, ClosingOneChannelLeavesTheOtherAlive) {
    MockSession session;
    WebTransportDataChannel a(&session);
    WebTransportDataChannel b(&session);
    Collector ca;
    Collector cb;
    ca.attach(a);
    cb.attach(b);

    a.open(make_cfg("a"));
    b.open(make_cfg("b"));
    ASSERT_EQ(a.open(), nimrtc::plugins::kOk);
    ASSERT_EQ(b.open(), nimrtc::plugins::kOk);

    a.close();

    const std::vector<std::uint8_t> payload(8, 0x22);
    b.send(BufferView{payload.data(), payload.size()});

    EXPECT_EQ(cb.payloads.size(), 1u);
    EXPECT_EQ(ca.payloads.size(), 0u);
    // The sibling's subscription survived the close (WP-C4).
    EXPECT_EQ(session.peer_subscriber_count(), 1u);
}

TEST(WebTransportDataChannelIsolation, TwoPeerStreamsReachTheirOwnChannel) {
    MockSession session;
    WebTransportDataChannel a(&session);
    WebTransportDataChannel b(&session);
    Collector ca;
    Collector cb;
    ca.attach(a);
    cb.attach(b);

    a.open(make_cfg("a"));
    b.open(make_cfg("b"));
    ASSERT_EQ(a.open(), nimrtc::plugins::kOk);
    ASSERT_EQ(b.open(), nimrtc::plugins::kOk);

    // Two peer streams, each carrying one frame addressed to one
    // channel.  The frames are built by the channels themselves (the
    // encoder is internal), so the test asks the channels to produce
    // them and replays the wire bytes.
    const std::vector<std::uint8_t> payload(4, 0x33);
    a.send(BufferView{payload.data(), payload.size()});
    const std::vector<std::uint8_t> wire_a = session.sent_chunks().back();

    b.send(BufferView{payload.data(), payload.size()});
    const std::vector<std::uint8_t> wire_b = session.sent_chunks().back();

    // Replay both wires on a *fresh* pair of channels (the loopback of
    // the first pair already delivered them).
    session.set_fail_stream_sends(true);  // stop the loopback
    WebTransportDataChannel a2(&session);
    WebTransportDataChannel b2(&session);
    Collector ca2;
    Collector cb2;
    ca2.attach(a2);
    cb2.attach(b2);
    a2.open(make_cfg("a"));
    b2.open(make_cfg("b"));
    ASSERT_EQ(a2.open(), nimrtc::plugins::kOk);
    ASSERT_EQ(b2.open(), nimrtc::plugins::kOk);

    session.deliver_peer_stream(wire_a);
    session.deliver_peer_stream(wire_b);

    EXPECT_EQ(ca2.payloads.size(), 1u);
    EXPECT_EQ(cb2.payloads.size(), 1u);
    EXPECT_EQ(ca2.labels[0], "a");
    EXPECT_EQ(cb2.labels[0], "b");
}

TEST(WebTransportDataChannelIsolation, DatagramGoesToItsOwnChannelOnly) {
    MockSession session;
    WebTransportDataChannel a(&session);
    WebTransportDataChannel b(&session);
    Collector ca;
    Collector cb;
    ca.attach(a);
    cb.attach(b);

    a.open(make_cfg("a", DataChannelReliability::kUnreliable));
    b.open(make_cfg("b", DataChannelReliability::kUnreliable));
    ASSERT_EQ(a.open(), nimrtc::plugins::kOk);
    ASSERT_EQ(b.open(), nimrtc::plugins::kOk);

    const std::vector<std::uint8_t> payload(8, 0x44);
    a.send(BufferView{payload.data(), payload.size()});

    EXPECT_EQ(ca.payloads.size(), 1u);
    EXPECT_EQ(cb.payloads.size(), 0u);
}

// ===========================================================================
// WP-D — partial reliability
// ===========================================================================

TEST(WebTransportDataChannelReliability, ExpiredMessageIsDroppedAndTheChannelStaysOpen) {
    MockSession session;
    session.set_fail_stream_sends(true);  // the transport refuses the bytes
    WebTransportDataChannel channel(&session);
    Collector c;
    c.attach(channel);

    DataChannelConfig cfg = make_cfg("a", DataChannelReliability::kPartialReliableTTL);
    cfg.max_packet_lifetime_ms = 1;
    channel.open(cfg);
    ASSERT_EQ(channel.open(), nimrtc::plugins::kOk);

    const std::vector<std::uint8_t> payload(8, 0x55);
    channel.send(BufferView{payload.data(), payload.size()});
    ASSERT_EQ(channel.pending_sends(), 1u);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    session.flush_outbound();

    // The expired message is dropped — the channel is NOT killed.
    EXPECT_EQ(channel.dropped_messages(), 1u);
    EXPECT_EQ(channel.pending_sends(), 0u);
    EXPECT_TRUE(channel.is_open());
    const bool closed =
        std::find(c.states.begin(), c.states.end(), "closed") != c.states.end();
    EXPECT_FALSE(closed);
}

TEST(WebTransportDataChannelReliability, UnlimitedRetransmitsStillDeliver) {
    MockSession session;
    WebTransportDataChannel channel(&session);
    Collector c;
    c.attach(channel);

    DataChannelConfig cfg =
        make_cfg("a", DataChannelReliability::kPartialReliableCount);
    cfg.max_retransmits = 0;  // unlimited
    channel.open(cfg);
    ASSERT_EQ(channel.open(), nimrtc::plugins::kOk);

    const std::vector<std::uint8_t> payload(8, 0x66);
    channel.send(BufferView{payload.data(), payload.size()});

    // Regression against the v1.1.0 behaviour: `max_retransmits == 0`
    // means "unlimited", so the message goes out and the channel does
    // not die.
    EXPECT_EQ(channel.dropped_messages(), 0u);
    EXPECT_EQ(c.payloads.size(), 1u);
    EXPECT_TRUE(channel.is_open());
}

TEST(WebTransportDataChannelReliability, RetransmitBudgetIsPerMessage) {
    MockSession session;
    session.set_fail_stream_sends(true);
    WebTransportDataChannel channel(&session);
    Collector c;
    c.attach(channel);

    DataChannelConfig cfg =
        make_cfg("a", DataChannelReliability::kPartialReliableCount);
    cfg.max_retransmits = 2;  // two retransmissions after the first try
    channel.open(cfg);
    ASSERT_EQ(channel.open(), nimrtc::plugins::kOk);

    const std::vector<std::uint8_t> payload(8, 0x77);
    channel.send(BufferView{payload.data(), payload.size()});

    // First attempt + two retransmissions, then the message is dropped.
    session.flush_outbound();
    session.flush_outbound();
    session.flush_outbound();

    EXPECT_EQ(channel.dropped_messages(), 1u);
    EXPECT_EQ(channel.pending_sends(), 0u);
    EXPECT_TRUE(channel.is_open());
}

// ===========================================================================
// WP-E — priority
// ===========================================================================

TEST(WebTransportDataChannelPriority, PriorityReachesTheSession) {
    MockSession session;
    WebTransportDataChannel channel(&session);
    channel.open(make_cfg("a"));
    ASSERT_EQ(channel.open(), nimrtc::plugins::kOk);

    ASSERT_EQ(session.priorities().size(), 1u);
    EXPECT_EQ(session.priorities()[0], 128);
}

TEST(WebTransportDataChannelPriority, HigherPriorityIsOfferedFirst) {
    MockSession session;
    session.set_fail_stream_sends(true);  // the sends stay queued

    WebTransportDataChannel low(&session);
    WebTransportDataChannel high(&session);
    low.open(make_cfg("low"));
    high.open(make_cfg("high"));
    ASSERT_EQ(low.open(), nimrtc::plugins::kOk);
    ASSERT_EQ(high.open(), nimrtc::plugins::kOk);

    // The priorities are configured per channel.
    auto set_priority = [](WebTransportDataChannel& ch, const char* label,
                           std::uint8_t prio) {
        DataChannelConfig cfg = make_cfg(label);
        cfg.priority = prio;
        ch.open(cfg);
    };
    set_priority(low, "low", 240);
    set_priority(high, "high", 16);

    const std::vector<std::uint8_t> payload(8, 0x88);
    low.send(BufferView{payload.data(), payload.size()});
    high.send(BufferView{payload.data(), payload.size()});

    // The session drains the pending sends in priority order.
    ASSERT_FALSE(session.flush_order().empty());
    EXPECT_EQ(session.flush_order().front(), 16);
    EXPECT_LT(session.flush_order().front(), 240);
}

// ===========================================================================
// WP-D4 — the application-level ARQ (sequence number + acknowledgement)
// ===========================================================================
//
// A QUIC stream gives the application no per-message retry hook, so
// the partial-reliability modes run their own retransmission protocol
// on top of the frame: every message carries a sequence number, the
// receiver acknowledges it, and the sender retransmits the messages
// that were not acknowledged in time.
//
// The tests below run the two ends against each other (two mock
// sessions, one link) and simulate the loss on the link.

namespace {

/** Wait long enough for the ARQ retransmission timer (250 ms) to
 *  expire. */
void wait_for_retransmit_timer() {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
}

/** The configuration of a partial-reliability channel on both ends of
 *  the link. */
DataChannelConfig make_arq_cfg(std::uint16_t max_retransmits) {
    DataChannelConfig cfg =
        make_cfg("a", DataChannelReliability::kPartialReliableCount);
    cfg.max_retransmits = max_retransmits;
    return cfg;
}

} // anonymous namespace

TEST(WebTransportDataChannelArq, MessageIsDeliveredAndAcknowledged) {
    MockSession sa;
    MockSession sb;
    sa.set_peer(&sb);
    sb.set_peer(&sa);

    WebTransportDataChannel a(&sa);
    WebTransportDataChannel b(&sb);
    Collector ca;
    Collector cb;
    ca.attach(a);
    cb.attach(b);

    a.open(make_arq_cfg(2));
    b.open(make_arq_cfg(2));
    ASSERT_EQ(a.open(), nimrtc::plugins::kOk);
    ASSERT_EQ(b.open(), nimrtc::plugins::kOk);

    const std::vector<std::uint8_t> payload(8, 0xA1);
    a.send(BufferView{payload.data(), payload.size()});

    // The peer received the message exactly once ...
    ASSERT_EQ(cb.payloads.size(), 1u);
    EXPECT_EQ(cb.payloads[0], payload);
    EXPECT_EQ(cb.labels.size(), 1u);
    EXPECT_EQ(cb.labels[0], "a");
    // ... and the sender's in-flight window is empty again: the
    // message was acknowledged.
    EXPECT_EQ(a.in_flight_messages(), 0u);
    EXPECT_EQ(a.retransmitted_messages(), 0u);
    EXPECT_EQ(a.dropped_messages(), 0u);
}

TEST(WebTransportDataChannelArq, LostMessageIsRetransmittedAndDeliveredOnce) {
    MockSession sa;
    MockSession sb;
    sa.set_peer(&sb);
    sb.set_peer(&sa);

    WebTransportDataChannel a(&sa);
    WebTransportDataChannel b(&sb);
    Collector cb;
    cb.attach(b);

    a.open(make_arq_cfg(2));
    b.open(make_arq_cfg(2));
    ASSERT_EQ(a.open(), nimrtc::plugins::kOk);
    ASSERT_EQ(b.open(), nimrtc::plugins::kOk);

    // The first attempt is lost on the link.
    sa.drop_next_arq_data(1);

    const std::vector<std::uint8_t> payload(8, 0xB2);
    a.send(BufferView{payload.data(), payload.size()});

    EXPECT_EQ(cb.payloads.size(), 0u) << "the frame was lost on the link";
    EXPECT_EQ(a.in_flight_messages(), 1u) << "the message waits for its ack";

    // The retransmission timer fires: the engine's tick calls
    // flush_outbound(), which runs the ARQ sweep.
    wait_for_retransmit_timer();
    sa.flush_outbound();

    EXPECT_EQ(a.retransmitted_messages(), 1u);
    ASSERT_EQ(cb.payloads.size(), 1u) << "the retransmission is delivered";
    EXPECT_EQ(cb.payloads[0], payload);
    EXPECT_EQ(a.in_flight_messages(), 0u) << "the retransmission was acked";
}

TEST(WebTransportDataChannelArq, ExhaustedBudgetDropsTheMessageAndKeepsTheChannelOpen) {
    MockSession sa;
    MockSession sb;
    sa.set_peer(&sb);
    sb.set_peer(&sa);

    WebTransportDataChannel a(&sa);
    WebTransportDataChannel b(&sb);
    Collector cb;
    cb.attach(b);

    a.open(make_arq_cfg(2));
    b.open(make_arq_cfg(2));
    ASSERT_EQ(a.open(), nimrtc::plugins::kOk);
    ASSERT_EQ(b.open(), nimrtc::plugins::kOk);

    // Every attempt is lost on the link.
    sa.drop_next_arq_data(16);

    const std::vector<std::uint8_t> payload(8, 0xC3);
    a.send(BufferView{payload.data(), payload.size()});
    EXPECT_EQ(a.in_flight_messages(), 1u);

    // The budget is two retransmissions: the first attempt plus two
    // retries, then the message is dropped.
    for (int i = 0; i < 3; ++i) {
        wait_for_retransmit_timer();
        sa.flush_outbound();
    }

    EXPECT_EQ(a.retransmitted_messages(), 2u)
        << "the budget is two retransmissions, not two attempts";
    EXPECT_EQ(a.dropped_messages(), 1u);
    EXPECT_EQ(a.in_flight_messages(), 0u);
    EXPECT_TRUE(a.is_open()) << "an exhausted budget must not kill the channel";
    EXPECT_EQ(cb.payloads.size(), 0u);
}

TEST(WebTransportDataChannelArq, DuplicateIsDeliveredOnlyOnce) {
    MockSession sa;
    MockSession sb;
    sa.set_peer(&sb);
    sb.set_peer(&sa);

    WebTransportDataChannel a(&sa);
    WebTransportDataChannel b(&sb);
    Collector cb;
    cb.attach(b);

    a.open(make_arq_cfg(2));
    b.open(make_arq_cfg(2));
    ASSERT_EQ(a.open(), nimrtc::plugins::kOk);
    ASSERT_EQ(b.open(), nimrtc::plugins::kOk);

    // The acknowledgement is lost, so the sender retransmits.
    sb.drop_next_arq_ack(1);

    const std::vector<std::uint8_t> payload(8, 0xD4);
    a.send(BufferView{payload.data(), payload.size()});

    ASSERT_EQ(cb.payloads.size(), 1u);
    EXPECT_EQ(a.in_flight_messages(), 1u) << "the ack was lost";

    wait_for_retransmit_timer();
    sa.flush_outbound();

    // The retransmission arrives, but the message is not delivered a
    // second time — the de-duplication window saw the sequence number.
    EXPECT_EQ(a.retransmitted_messages(), 1u);
    EXPECT_EQ(cb.payloads.size(), 1u) << "a retransmission is not a new message";
    // The receiver re-acknowledged the duplicate, so the in-flight
    // window closes.
    EXPECT_EQ(a.in_flight_messages(), 0u);
}

TEST(WebTransportDataChannelArq, TtlExpiresWhileInFlightAndKeepsTheChannelOpen) {
    MockSession sa;
    MockSession sb;
    sa.set_peer(&sb);
    sb.set_peer(&sa);

    WebTransportDataChannel a(&sa);
    WebTransportDataChannel b(&sb);
    Collector cb;
    cb.attach(b);

    DataChannelConfig cfg =
        make_cfg("a", DataChannelReliability::kPartialReliableTTL);
    cfg.max_packet_lifetime_ms = 50;
    a.open(cfg);
    b.open(cfg);
    ASSERT_EQ(a.open(), nimrtc::plugins::kOk);
    ASSERT_EQ(b.open(), nimrtc::plugins::kOk);

    // Every attempt is lost on the link: the message stays in flight
    // until its lifetime passes.
    sa.drop_next_arq_data(16);

    const std::vector<std::uint8_t> payload(8, 0xE5);
    a.send(BufferView{payload.data(), payload.size()});
    EXPECT_EQ(a.in_flight_messages(), 1u);

    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    sa.flush_outbound();

    EXPECT_EQ(a.dropped_messages(), 1u);
    EXPECT_EQ(a.in_flight_messages(), 0u);
    EXPECT_TRUE(a.is_open());
    EXPECT_EQ(cb.payloads.size(), 0u);
}

TEST(WebTransportDataChannelArq, ReliableModeDoesNotUseTheArq) {
    MockSession sa;
    MockSession sb;
    sa.set_peer(&sb);
    sb.set_peer(&sa);

    WebTransportDataChannel a(&sa);
    WebTransportDataChannel b(&sb);
    Collector cb;
    cb.attach(b);

    // The reliable mode rides QUIC's own retransmission: no sequence
    // number, no acknowledgement, no in-flight window.
    a.open(make_cfg("a"));
    b.open(make_cfg("a"));
    ASSERT_EQ(a.open(), nimrtc::plugins::kOk);
    ASSERT_EQ(b.open(), nimrtc::plugins::kOk);

    const std::vector<std::uint8_t> payload(8, 0xF6);
    a.send(BufferView{payload.data(), payload.size()});

    ASSERT_EQ(cb.payloads.size(), 1u);
    EXPECT_EQ(a.in_flight_messages(), 0u);
    EXPECT_EQ(a.retransmitted_messages(), 0u);
}

}  // namespace
