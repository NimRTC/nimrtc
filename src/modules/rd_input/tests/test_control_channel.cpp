// src/modules/rd_input/tests/test_control_channel.cpp
// ----------------------------------------------------------------------------
// The NimRTC-native control channel (remote-desktop plan G1 / G2).
//
// The channel is the *mechanism* that makes a byte transport
// message-oriented: it frames each message, reassembles the frames that
// arrive across several transport records, and refuses a malformed frame
// without ever allocating for it.
//
// The tests below are deliberately transport-free: the channel talks to a
// `std::function` sink, so a "transport record" here is just a vector of
// bytes the test hands to `feed_inbound()`.  That is what makes the
// framing's contract testable without a socket, a DTLS session or an
// ICE pair.
// ----------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <nimrtc/core/registry.hpp>

#include "nimrtc/rd_input/control_channel.hpp"

namespace nimrtc::rd_input {
namespace {

/// A span over a byte vector.
core::ByteSpan span_of(const std::vector<std::uint8_t>& bytes) {
    return core::ByteSpan{bytes.data(), bytes.size()};
}

/// A byte vector from a string literal.
std::vector<std::uint8_t> bytes_of(std::string_view text) {
    std::vector<std::uint8_t> out;
    out.reserve(text.size());
    for (char c : text) out.push_back(static_cast<std::uint8_t>(c));
    return out;
}

/// The messages a channel delivered, copied out of the callback.
struct Received {
    std::vector<std::vector<std::uint8_t>> messages;

    void clear() { messages.clear(); }
    std::size_t count() const { return messages.size(); }
};

// ---------------------------------------------------------------------------
// The framing round trip
// ---------------------------------------------------------------------------

TEST(ControlChannel, AMessageRoundTrips) {
    ControlChannel tx;
    ControlChannel rx;

    std::vector<std::uint8_t> wire;
    tx.set_outbound_sink([&wire](core::ByteSpan bytes) {
        wire.insert(wire.end(), bytes.begin(), bytes.end());
        return plugins::kOk;
    });

    Received received;
    rx.set_on_message([&received](core::ByteSpan message) {
        received.messages.emplace_back(message.begin(), message.end());
    });

    const auto payload = bytes_of("hello control plane");
    ASSERT_EQ(tx.send(span_of(payload)), plugins::kOk);
    ASSERT_EQ(rx.feed_inbound(span_of(wire)), plugins::kOk);

    ASSERT_EQ(received.count(), 1u);
    EXPECT_EQ(received.messages[0], payload);

    EXPECT_EQ(tx.messages_sent(), 1u);
    EXPECT_EQ(tx.bytes_sent(), payload.size());
    EXPECT_EQ(rx.messages_received(), 1u);
    EXPECT_EQ(rx.bytes_received(), payload.size());
    EXPECT_EQ(rx.frames_rejected(), 0u);
}

TEST(ControlChannel, TheFrameCarriesTheMagicAndTheVersion) {
    std::vector<std::uint8_t> framed;
    const auto payload = bytes_of("x");
    ASSERT_EQ(ControlChannel::frame_message(span_of(payload), 1024, framed),
              plugins::kOk);

    ASSERT_GE(framed.size(), 3u);
    EXPECT_EQ(framed[0], kControlFrameMagic);
    EXPECT_EQ(framed[1], kControlFrameVersion);
    // The varint length of a one-byte payload is one byte.
    EXPECT_EQ(framed[2], 1u);
    EXPECT_EQ(framed[3], payload[0]);
}

TEST(ControlChannel, AnEmptyMessageIsRefused) {
    ControlChannel tx;
    EXPECT_EQ(tx.send(core::ByteSpan{}), plugins::kErrInvalidParam);
    EXPECT_EQ(tx.messages_sent(), 0u);
}

TEST(ControlChannel, AnOverLongMessageIsRefusedAndNeverTruncated) {
    ControlChannel tx{8};
    const auto payload = bytes_of("0123456789");
    EXPECT_EQ(tx.send(span_of(payload)), plugins::kErrInvalidParam);
    EXPECT_EQ(tx.messages_sent(), 0u);
    EXPECT_EQ(tx.queued(), 0u);
}

// ---------------------------------------------------------------------------
// The reassembly
// ---------------------------------------------------------------------------

TEST(ControlChannel, AMessageSplitAcrossRecordsIsReassembled) {
    ControlChannel tx;
    ControlChannel rx;

    // Frame the message by hand so the test can cut it at an arbitrary
    // point — the point of the exercise.
    std::vector<std::uint8_t> framed;
    const auto payload = bytes_of("a message that will be split across records");
    ASSERT_EQ(ControlChannel::frame_message(span_of(payload), 1024, framed),
              plugins::kOk);
    ASSERT_GT(framed.size(), 4u);

    Received received;
    rx.set_on_message([&received](core::ByteSpan message) {
        received.messages.emplace_back(message.begin(), message.end());
    });

    // Feed the frame one byte at a time: every intermediate state is a
    // partial message, and none of them may produce a delivery.
    for (std::size_t i = 0; i + 1 < framed.size(); ++i) {
        const std::uint8_t byte = framed[i];
        ASSERT_EQ(rx.feed_inbound(core::ByteSpan{&byte, 1}), plugins::kOk)
            << "byte " << i << " must not be reported as an error";
        EXPECT_EQ(received.count(), 0u)
            << "a partial message must not be delivered (byte " << i << ")";
    }

    const std::uint8_t last = framed.back();
    ASSERT_EQ(rx.feed_inbound(core::ByteSpan{&last, 1}), plugins::kOk);
    ASSERT_EQ(received.count(), 1u);
    EXPECT_EQ(received.messages[0], payload);
    EXPECT_EQ(rx.messages_received(), 1u);
}

TEST(ControlChannel, SeveralMessagesInOneRecordAreAllDelivered) {
    ControlChannel tx;
    ControlChannel rx;

    // Two framed messages concatenated into one transport record.
    std::vector<std::uint8_t> wire;
    const auto first  = bytes_of("first");
    const auto second = bytes_of("second");
    std::vector<std::uint8_t> frame;
    ASSERT_EQ(ControlChannel::frame_message(span_of(first), 1024, frame),
              plugins::kOk);
    wire.insert(wire.end(), frame.begin(), frame.end());
    ASSERT_EQ(ControlChannel::frame_message(span_of(second), 1024, frame),
              plugins::kOk);
    wire.insert(wire.end(), frame.begin(), frame.end());

    Received received;
    rx.set_on_message([&received](core::ByteSpan message) {
        received.messages.emplace_back(message.begin(), message.end());
    });

    ASSERT_EQ(rx.feed_inbound(span_of(wire)), plugins::kOk);
    ASSERT_EQ(received.count(), 2u);
    EXPECT_EQ(received.messages[0], first);
    EXPECT_EQ(received.messages[1], second);
    EXPECT_EQ(rx.messages_received(), 2u);
}

// ---------------------------------------------------------------------------
// The malformed frames
// ---------------------------------------------------------------------------

TEST(ControlChannel, ABadMagicByteIsRejected) {
    ControlChannel rx;
    Received received;
    rx.set_on_message([&received](core::ByteSpan message) {
        received.messages.emplace_back(message.begin(), message.end());
    });

    const auto bad = bytes_of("XX\x01\x01");
    EXPECT_EQ(rx.feed_inbound(span_of(bad)), plugins::kErrCorrupt);
    EXPECT_EQ(rx.frames_rejected(), 1u);
    EXPECT_EQ(rx.messages_received(), 0u);
    EXPECT_EQ(received.count(), 0u);
}

TEST(ControlChannel, AnAbsurdLengthIsRejectedWithoutAHugeAllocation) {
    ControlChannel rx{1024};

    // magic + version + a varint that claims 2^40 bytes.
    std::vector<std::uint8_t> bad{
        kControlFrameMagic, kControlFrameVersion,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01};
    EXPECT_EQ(rx.feed_inbound(span_of(bad)), plugins::kErrCorrupt);
    EXPECT_EQ(rx.frames_rejected(), 1u);
    EXPECT_EQ(rx.messages_received(), 0u);
}

TEST(ControlChannel, ALengthOverTheLimitIsRejected) {
    ControlChannel rx{16};
    // A frame that claims 100 bytes when the limit is 16.
    std::vector<std::uint8_t> bad{
        kControlFrameMagic, kControlFrameVersion, 100};
    EXPECT_EQ(rx.feed_inbound(span_of(bad)), plugins::kErrCorrupt);
    EXPECT_EQ(rx.frames_rejected(), 1u);
}

TEST(ControlChannel, AnUnknownVersionIsRejected) {
    ControlChannel rx;
    std::vector<std::uint8_t> bad{
        kControlFrameMagic, static_cast<std::uint8_t>(kControlFrameVersion + 1),
        1, 'a'};
    EXPECT_EQ(rx.feed_inbound(span_of(bad)), plugins::kErrCorrupt);
    EXPECT_EQ(rx.frames_rejected(), 1u);
    EXPECT_EQ(rx.messages_received(), 0u);
}

TEST(ControlChannel, ANonTerminatingVarintIsRejected) {
    ControlChannel rx;
    // Eleven continuation bytes: a varint cannot occupy more than ten
    // bytes, so this cannot be a value.
    std::vector<std::uint8_t> bad{
        kControlFrameMagic, kControlFrameVersion,
        0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
    EXPECT_EQ(rx.feed_inbound(span_of(bad)), plugins::kErrCorrupt);
    EXPECT_EQ(rx.frames_rejected(), 1u);
}

TEST(ControlChannel, TheChannelStaysUsableAfterARejection) {
    ControlChannel tx;
    ControlChannel rx;

    std::vector<std::uint8_t> wire;
    tx.set_outbound_sink([&wire](core::ByteSpan bytes) {
        wire.insert(wire.end(), bytes.begin(), bytes.end());
        return plugins::kOk;
    });

    Received received;
    rx.set_on_message([&received](core::ByteSpan message) {
        received.messages.emplace_back(message.begin(), message.end());
    });

    // A foreign datagram first...
    const auto junk = bytes_of("not a control frame");
    EXPECT_EQ(rx.feed_inbound(span_of(junk)), plugins::kErrCorrupt);

    // ...then a good one.  The channel recovers: a malformed frame
    // must not take the control plane down.
    const auto payload = bytes_of("still here");
    ASSERT_EQ(tx.send(span_of(payload)), plugins::kOk);
    ASSERT_EQ(rx.feed_inbound(span_of(wire)), plugins::kOk);
    ASSERT_EQ(received.count(), 1u);
    EXPECT_EQ(received.messages[0], payload);
}

// ---------------------------------------------------------------------------
// The outbound queue
// ---------------------------------------------------------------------------

TEST(ControlChannel, ARefusedSinkKeepsTheMessageQueued) {
    ControlChannel tx;

    bool refuse = true;
    tx.set_outbound_sink([&refuse](core::ByteSpan) {
        return refuse ? plugins::kErrNotReady : plugins::kOk;
    });

    const auto payload = bytes_of("queued");
    ASSERT_EQ(tx.send(span_of(payload)), plugins::kOk);
    // The sink refused the bytes, so the message stays queued and is
    // not counted as sent.
    EXPECT_EQ(tx.messages_sent(), 0u);
    EXPECT_EQ(tx.queued(), 1u);

    // Once the transport takes the bytes, the flush puts it out.
    refuse = false;
    ASSERT_EQ(tx.flush(), plugins::kOk);
    EXPECT_EQ(tx.messages_sent(), 1u);
    EXPECT_EQ(tx.queued(), 0u);
    EXPECT_EQ(tx.bytes_sent(), payload.size());
}

TEST(ControlChannel, WithoutASinkTheMessageStaysQueued) {
    ControlChannel tx;
    const auto payload = bytes_of("no transport yet");
    ASSERT_EQ(tx.send(span_of(payload)), plugins::kOk);
    EXPECT_EQ(tx.queued(), 1u);
    EXPECT_EQ(tx.messages_sent(), 0u);
    EXPECT_EQ(tx.flush(), plugins::kErrNotReady);
    EXPECT_EQ(tx.queued(), 1u);
}

TEST(ControlChannel, TheQueueIsBounded) {
    ControlChannel tx;
    // No sink at all, so every message stays queued.
    const auto payload = bytes_of("flood");
    plugins::Status last = plugins::kOk;
    for (int i = 0; i < 256; ++i) {
        last = tx.send(span_of(payload));
        if (last != plugins::kOk) break;
    }
    EXPECT_EQ(last, plugins::kErrBufferTooSmall);
    EXPECT_GT(tx.messages_dropped(), 0u);
    EXPECT_LE(tx.queued(), 64u);
}

// ---------------------------------------------------------------------------
// The plugin adapter + the factory + the registry
// ---------------------------------------------------------------------------

TEST(ControlChannelPlugin, TheAdapterForwardsTheSeam) {
    plugins::ControlChannelBinding binding;
    binding.transport = plugins::ControlChannelTransport::kDtlsAppData;

    std::vector<std::uint8_t> wire;
    binding.sink = [&wire](core::ByteSpan bytes) {
        wire.insert(wire.end(), bytes.begin(), bytes.end());
        return plugins::kOk;
    };

    rd_input::ControlChannelFactory factory;
    auto channel = factory.create(binding);
    ASSERT_NE(channel, nullptr);
    ASSERT_EQ(channel->open(), plugins::kOk);

    std::vector<std::vector<std::uint8_t>> received;
    channel->set_on_control_message([&received](core::ByteSpan message) {
        received.emplace_back(message.begin(), message.end());
    });

    const auto payload = bytes_of("through the seam");
    ASSERT_EQ(channel->send_control_message(span_of(payload)), plugins::kOk);
    ASSERT_EQ(channel->on_transport_bytes(span_of(wire)), plugins::kOk);

    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0], payload);
    EXPECT_EQ(channel->messages_sent(), 1u);
    EXPECT_EQ(channel->messages_received(), 1u);
    EXPECT_EQ(channel->frames_rejected(), 0u);

    channel->close();
}

TEST(ControlChannelPlugin, TheFactoryIsRegisteredUnderItsId) {
    rd_input::register_default_plugins();
    // Idempotent.
    rd_input::register_default_plugins();

    const auto* factory =
        core::PluginRegistry::instance().get_control_channel("control-channel");
    ASSERT_NE(factory, nullptr);
    EXPECT_EQ(factory->id(), "control-channel");

    const auto ids = core::PluginRegistry::instance().list_control_channels();
    EXPECT_NE(std::find(ids.begin(), ids.end(), std::string("control-channel")),
              ids.end());
}

} // namespace
} // namespace nimrtc::rd_input
