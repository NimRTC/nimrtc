/**
 * @file tests/test_transport_cc.cpp
 * @brief Tests for WebRTC abs-send-time and transport-wide CC feedback.
 *
 * Covers:
 *   T1  - abs-send-time round-trip (build -> parse -> equal)
 *   T2  - abs-send-time 24-bit wrap-around
 *   T3  - transport-cc parse: run length chunk, "not received"
 *   T4  - transport-cc parse: run length chunk, "received" with small deltas
 *   T5  - transport-cc parse: run length chunk, symbol 3 ("received, no delta")
 *   T6  - transport-cc parse: one-bit status vector chunk
 *   T7  - transport-cc parse: two-bit status vector chunk (draft example 2)
 *   T8  - transport-cc parse: large / negative (16-bit signed) delta
 *   T9  - transport-cc parse: several chunks, deltas follow all of them
 *   T10 - transport-cc parse: header fields (reference time, fb pkt count)
 *   T11 - transport-cc parse: the arrival-time series is reconstructible
 *   T12 - transport-cc parse: rejects a wrong FMT
 *   T13 - transport-cc parse: rejects truncated receive deltas
 *   T14 - transport-cc parse: rejects truncated packet chunks
 *   T15 - transport-cc parse: rejects an empty packet status
 *
 * Wire format: draft-holmer-rmcat-transport-wide-cc-extensions-01
 * (NOT RFC 8888, which is the Common Control Feedback Format, FMT = 11).
 * See ADR-018 for the normative statement of the time units.
 */

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/rtp/packet.hpp>

using nimrtc::core::ByteBuffer;
using nimrtc::core::ByteSpan;
using namespace nimrtc::rtp;

// ---------------------------------------------------------------------------
// T1: abs-send-time round-trip -- a fresh value emits the RFC 5285 one-byte
// form extension (profile 0xBEDE, id=1, 3-byte timestamp) and parses back
// equal.
// ---------------------------------------------------------------------------
TEST(AbsSendTimeExtension, RoundTrip) {
    constexpr std::uint32_t kTimestamp24MHz = 0x00ABCDEFu;
    AbsSendTime t{kTimestamp24MHz};

    PacketBuilder pb;
    pb.set_seq(0x1234)
      .set_timestamp(0)
      .set_ssrc(0xDEADBEEFu)
      .set_payload_type(96)
      .set_abs_send_time(t);

    auto buf = pb.build();
    ASSERT_GE(buf.size(), 12u + 4u /*ext header*/ + 4u /*ext data*/);

    // Verify the on-wire layout of the extension header:
    //   byte 12 (after the fixed header) = 0xBE, byte 13 = 0xDE,
    //   byte 14 = 0x00, byte 15 = 0x01
    EXPECT_EQ(buf[12], 0xBE);
    EXPECT_EQ(buf[13], 0xDE);
    EXPECT_EQ(buf[14], 0x00);
    EXPECT_EQ(buf[15], 0x01);

    Parser parser;
    auto pv = parser.parse(buf);
    ASSERT_TRUE(pv.ok()) << pv.error().message();
    ASSERT_TRUE(pv.value().extension.has_value());

    auto ext_data = pv.value().extension->data;
    AbsSendTime parsed{};
    auto r = parser.parse_abs_send_time_extension(ext_data, parsed);
    ASSERT_TRUE(r.ok()) << r.error().message();
    EXPECT_EQ(parsed.send_time_24mhz, kTimestamp24MHz);
}

// ---------------------------------------------------------------------------
// T2: 24-bit wrap -- the field is masked to 24 bits at build time, so setting
// a 32-bit value > 0xFFFFFF must round-trip the low 24 bits.
// ---------------------------------------------------------------------------
TEST(AbsSendTimeExtension, WrapsAt24Bits) {
    AbsSendTime t{0xFFEEDDCCu};                  // high byte set
    PacketBuilder pb;
    pb.set_seq(1).set_ssrc(1).set_payload_type(96).set_abs_send_time(t);

    auto buf = pb.build();
    Parser parser;
    auto pv = parser.parse(buf);
    ASSERT_TRUE(pv.ok());
    ASSERT_TRUE(pv.value().extension.has_value());

    AbsSendTime parsed{};
    auto r = parser.parse_abs_send_time_extension(pv.value().extension->data, parsed);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(parsed.send_time_24mhz, 0x00EEDDCCu);   // high byte masked out
}

// ===========================================================================
// transport-wide CC feedback
// ===========================================================================

namespace {

// ---------------------------------------------------------------------------
// Helpers to build the wire format.
//
//   0                   1                   2                   3
//   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |V=2|P|  FMT=15 |    PT=205     |           length              |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |                     SSRC of packet sender                     |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |                      SSRC of media source                     |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |      base sequence number     |      packet status count      |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |                 reference time                | fb pkt. count |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |          packet chunk         |         packet chunk          |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |          recv delta           |          recv delta           |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// `body` holds the packet chunks followed by the whole receive-delta list.
// ---------------------------------------------------------------------------
ByteBuffer make_transport_cc_packet(std::uint32_t sender_ssrc,
                                     std::uint32_t media_ssrc,
                                     std::uint16_t base_seq,
                                     std::uint16_t packet_status_count,
                                     std::uint32_t reference_time,
                                     std::uint8_t fb_pkt_count,
                                     ByteSpan body) {
    // 4 B RTCP header + 16 B feedback header + body, padded to a 32-bit word.
    const std::size_t total = 20u + body.size();
    const std::size_t pad = (4u - (total % 4u)) % 4u;
    const std::size_t padded = total + pad;

    ByteBuffer buf(padded, 0);

    buf[0] = static_cast<std::uint8_t>(0x80u | (pad ? 0x20u : 0x00u) | 15u);
    buf[1] = 205u;                                        // PT = RTPFB
    const std::uint16_t length_words =
        static_cast<std::uint16_t>((padded / 4u) - 1u);
    buf[2] = static_cast<std::uint8_t>((length_words >> 8) & 0xFFu);
    buf[3] = static_cast<std::uint8_t>( length_words       & 0xFFu);

    auto w32 = [&](std::size_t off, std::uint32_t v) {
        buf[off    ] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
        buf[off + 1] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
        buf[off + 2] = static_cast<std::uint8_t>((v >>  8) & 0xFFu);
        buf[off + 3] = static_cast<std::uint8_t>( v        & 0xFFu);
    };
    auto w16 = [&](std::size_t off, std::uint16_t v) {
        buf[off    ] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
        buf[off    + 1] = static_cast<std::uint8_t>(v & 0xFFu);
    };

    w32(4, sender_ssrc);
    w32(8, media_ssrc);
    w16(12, base_seq);
    w16(14, packet_status_count);
    buf[16] = static_cast<std::uint8_t>((reference_time >> 16) & 0xFFu);
    buf[17] = static_cast<std::uint8_t>((reference_time >>  8) & 0xFFu);
    buf[18] = static_cast<std::uint8_t>( reference_time        & 0xFFu);
    buf[19] = fb_pkt_count;

    if (!body.empty()) {
        std::memcpy(buf.data() + 20u, body.data(), body.size());
    }
    if (pad) {
        // The last padding octet carries the count of padding octets.
        buf[padded - 1u] = static_cast<std::uint8_t>(pad);
    }
    return buf;
}

// Run length chunk: 0 | symbol (2 bit) | run length (13 bit).
std::uint16_t run_length_chunk(std::uint8_t symbol, std::uint16_t run) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(symbol & 0x3u) << 13) | (run & 0x1FFFu));
}

// One-bit status vector chunk: 1 | 0 | 14 symbol bits (symbol 0 at bit 13).
// `received[i]` is the status of the i-th packet: false = not received,
// true = received with a small (8-bit) delta.
std::uint16_t one_bit_chunk(const bool (&received)[14]) {
    std::uint16_t w = 0x8000u;
    for (int i = 0; i < 14; ++i) {
        if (received[i]) {
            w |= static_cast<std::uint16_t>(1u << (13 - i));
        }
    }
    return w;
}

// Two-bit status vector chunk: 1 | 1 | 7 symbols of 2 bits (symbol 0 at
// bits 13..12).
std::uint16_t two_bit_chunk(const std::uint8_t (&symbols)[7]) {
    std::uint16_t w = 0xC000u;
    for (int i = 0; i < 7; ++i) {
        w |= static_cast<std::uint16_t>((symbols[i] & 0x3u) << (12 - 2 * i));
    }
    return w;
}

void append_u16(ByteBuffer& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFFu));
    v.push_back(static_cast<std::uint8_t>(x & 0xFFu));
}

void append_u8(ByteBuffer& v, std::uint8_t x) { v.push_back(x); }

} // namespace

// ---------------------------------------------------------------------------
// T3: run length chunk -- T=0, symbol "not received", run of 5.
// ---------------------------------------------------------------------------
TEST(TransportCcFeedback, RunLengthChunkNotReceived) {
    ByteBuffer body;
    append_u16(body, run_length_chunk(0, 5));

    auto pkt = make_transport_cc_packet(0x10000001u, 0x20000002u, 1000, 5,
                                         0x00123456u, 0, ByteSpan{body});
    Parser parser;
    auto fb = parser.parse_transport_cc(pkt);
    ASSERT_TRUE(fb.ok()) << fb.error().message();
    EXPECT_EQ(fb.value().sender_ssrc, 0x10000001u);
    EXPECT_EQ(fb.value().media_ssrc, 0x20000002u);
    EXPECT_EQ(fb.value().base_seq, 1000);
    EXPECT_EQ(fb.value().packet_status_count, 5);
    EXPECT_EQ(fb.value().reference_time_24mhz, 0x00123456u);
    ASSERT_EQ(fb.value().packets.size(), 5u);
    for (const auto& ps : fb.value().packets) {
        EXPECT_EQ(ps.status, PacketStatus::NotReceived);
        EXPECT_EQ(ps.delta_250us, 0);
    }
}

// ---------------------------------------------------------------------------
// T4: run length chunk of "received, small delta" -- every packet of the run
// carries its own 8-bit receive delta, and the deltas follow the chunks.
// ---------------------------------------------------------------------------
TEST(TransportCcFeedback, RunLengthChunkReceivedWithSmallDeltas) {
    ByteBuffer body;
    append_u16(body, run_length_chunk(1, 3));
    append_u8(body, 0x10);   // 16 ticks
    append_u8(body, 0x20);   // 32 ticks
    append_u8(body, 0x30);   // 48 ticks

    auto pkt = make_transport_cc_packet(1, 2, 10, 3, 0, 0, ByteSpan{body});
    Parser parser;
    auto fb = parser.parse_transport_cc(pkt);
    ASSERT_TRUE(fb.ok()) << fb.error().message();
    ASSERT_EQ(fb.value().packets.size(), 3u);
    EXPECT_EQ(fb.value().packets[0].status, PacketStatus::Received);
    EXPECT_EQ(fb.value().packets[0].delta_250us, 0x10);
    EXPECT_EQ(fb.value().packets[1].status, PacketStatus::Received);
    EXPECT_EQ(fb.value().packets[1].delta_250us, 0x20);
    EXPECT_EQ(fb.value().packets[2].status, PacketStatus::Received);
    EXPECT_EQ(fb.value().packets[2].delta_250us, 0x30);
}

// ---------------------------------------------------------------------------
// T5: run length chunk of symbol 3 -- "packet received, w/o recv delta".
// The draft's own example 2 uses this symbol; no delta bytes follow.
// ---------------------------------------------------------------------------
TEST(TransportCcFeedback, RunLengthChunkReceivedWithoutDelta) {
    ByteBuffer body;
    append_u16(body, run_length_chunk(3, 4));

    auto pkt = make_transport_cc_packet(1, 2, 0, 4, 0, 0, ByteSpan{body});
    Parser parser;
    auto fb = parser.parse_transport_cc(pkt);
    ASSERT_TRUE(fb.ok()) << fb.error().message();
    ASSERT_EQ(fb.value().packets.size(), 4u);
    for (const auto& ps : fb.value().packets) {
        EXPECT_EQ(ps.status, PacketStatus::ReceivedNoDelta);
        EXPECT_EQ(ps.delta_250us, 0);
    }
}

// ---------------------------------------------------------------------------
// T6: one-bit status vector chunk -- the 14 symbol bits live inside the
// 16-bit chunk header itself, and every "received" symbol is followed by an
// 8-bit delta.
// ---------------------------------------------------------------------------
TEST(TransportCcFeedback, OneBitStatusVectorChunk) {
    const bool received[14] = {false, true, true, false, false, false,
                               false, false, false, false, false, false,
                               false, false};
    ByteBuffer body;
    append_u16(body, one_bit_chunk(received));
    append_u8(body, 0x07);   // 7 ticks
    append_u8(body, 0x09);   // 9 ticks

    auto pkt = make_transport_cc_packet(1, 2, 1, 4, 0, 0, ByteSpan{body});
    Parser parser;
    auto fb = parser.parse_transport_cc(pkt);
    ASSERT_TRUE(fb.ok()) << fb.error().message();
    // The chunk carries 14 symbols, but only 4 packets are reported.
    ASSERT_EQ(fb.value().packets.size(), 4u);
    EXPECT_EQ(fb.value().packets[0].status, PacketStatus::NotReceived);
    EXPECT_EQ(fb.value().packets[1].status, PacketStatus::Received);
    EXPECT_EQ(fb.value().packets[1].delta_250us, 0x07);
    EXPECT_EQ(fb.value().packets[2].status, PacketStatus::Received);
    EXPECT_EQ(fb.value().packets[2].delta_250us, 0x09);
    EXPECT_EQ(fb.value().packets[3].status, PacketStatus::NotReceived);
}

// ---------------------------------------------------------------------------
// T7: two-bit status vector chunk -- the draft's example 2 verbatim:
//   1x "not received", 1x "received w/o delta", 3x "received", 2x "not received".
// ---------------------------------------------------------------------------
TEST(TransportCcFeedback, TwoBitStatusVectorChunk) {
    const std::uint8_t symbols[7] = {0, 3, 1, 1, 1, 0, 0};
    ByteBuffer body;
    append_u16(body, two_bit_chunk(symbols));
    append_u8(body, 0x05);   // 5 ticks
    append_u8(body, 0x0A);   // 10 ticks
    append_u8(body, 0x0F);   // 15 ticks

    auto pkt = make_transport_cc_packet(1, 2, 0, 7, 0, 0, ByteSpan{body});
    Parser parser;
    auto fb = parser.parse_transport_cc(pkt);
    ASSERT_TRUE(fb.ok()) << fb.error().message();
    ASSERT_EQ(fb.value().packets.size(), 7u);
    EXPECT_EQ(fb.value().packets[0].status, PacketStatus::NotReceived);
    EXPECT_EQ(fb.value().packets[1].status, PacketStatus::ReceivedNoDelta);
    EXPECT_EQ(fb.value().packets[1].delta_250us, 0);
    EXPECT_EQ(fb.value().packets[2].status, PacketStatus::Received);
    EXPECT_EQ(fb.value().packets[2].delta_250us, 0x05);
    EXPECT_EQ(fb.value().packets[3].status, PacketStatus::Received);
    EXPECT_EQ(fb.value().packets[3].delta_250us, 0x0A);
    EXPECT_EQ(fb.value().packets[4].status, PacketStatus::Received);
    EXPECT_EQ(fb.value().packets[4].delta_250us, 0x0F);
    EXPECT_EQ(fb.value().packets[5].status, PacketStatus::NotReceived);
    EXPECT_EQ(fb.value().packets[6].status, PacketStatus::NotReceived);
}

// ---------------------------------------------------------------------------
// T8: large / negative delta -- symbol 2 ("received, large or negative
// delta") is followed by a 16-bit *signed* value.
// ---------------------------------------------------------------------------
TEST(TransportCcFeedback, LargeNegativeDelta) {
    ByteBuffer body;
    append_u16(body, run_length_chunk(2, 2));
    append_u16(body, 0xFF9Cu);   // -100 ticks
    append_u16(body, 0x0100u);   // +256 ticks

    auto pkt = make_transport_cc_packet(1, 2, 0, 2, 0, 0, ByteSpan{body});
    Parser parser;
    auto fb = parser.parse_transport_cc(pkt);
    ASSERT_TRUE(fb.ok()) << fb.error().message();
    ASSERT_EQ(fb.value().packets.size(), 2u);
    EXPECT_EQ(fb.value().packets[0].status, PacketStatus::ReceivedSmall);
    EXPECT_EQ(fb.value().packets[0].delta_250us, -100);
    EXPECT_EQ(fb.value().packets[1].status, PacketStatus::ReceivedSmall);
    EXPECT_EQ(fb.value().packets[1].delta_250us, 256);
}

// ---------------------------------------------------------------------------
// T9: several chunks -- the receive deltas follow *all* the chunks, in
// packet order, regardless of which chunk a packet's status came from.
// ---------------------------------------------------------------------------
TEST(TransportCcFeedback, MultipleChunks) {
    ByteBuffer body;
    append_u16(body, run_length_chunk(0, 3));       // 3 x not received
    const bool received[14] = {true, false, true, false, false, false,
                               false, false, false, false, false, false,
                               false, false};
    append_u16(body, one_bit_chunk(received));       // 5 symbols used
    append_u8(body, 0x11);                           // delta of packet 3
    append_u8(body, 0x22);                           // delta of packet 5

    auto pkt = make_transport_cc_packet(1, 2, 0, 8, 0, 0, ByteSpan{body});
    Parser parser;
    auto fb = parser.parse_transport_cc(pkt);
    ASSERT_TRUE(fb.ok()) << fb.error().message();
    ASSERT_EQ(fb.value().packets.size(), 8u);
    EXPECT_EQ(fb.value().packets[0].status, PacketStatus::NotReceived);
    EXPECT_EQ(fb.value().packets[1].status, PacketStatus::NotReceived);
    EXPECT_EQ(fb.value().packets[2].status, PacketStatus::NotReceived);
    EXPECT_EQ(fb.value().packets[3].status, PacketStatus::Received);
    EXPECT_EQ(fb.value().packets[3].delta_250us, 0x11);
    EXPECT_EQ(fb.value().packets[4].status, PacketStatus::NotReceived);
    EXPECT_EQ(fb.value().packets[5].status, PacketStatus::Received);
    EXPECT_EQ(fb.value().packets[5].delta_250us, 0x22);
    EXPECT_EQ(fb.value().packets[6].status, PacketStatus::NotReceived);
    EXPECT_EQ(fb.value().packets[7].status, PacketStatus::NotReceived);
}

// ---------------------------------------------------------------------------
// T10: header fields -- the reference time is in multiples of 64 ms, and the
// feedback-packet counter is the byte that follows it.
// ---------------------------------------------------------------------------
TEST(TransportCcFeedback, HeaderFields) {
    ByteBuffer body;
    append_u16(body, run_length_chunk(0, 1));

    auto pkt = make_transport_cc_packet(0xCAFEBABEu, 0xFEEDFACEu, 42, 1,
                                         0x00ABCDEFu, 0x5A, ByteSpan{body});
    Parser parser;
    auto fb = parser.parse_transport_cc(pkt);
    ASSERT_TRUE(fb.ok()) << fb.error().message();
    EXPECT_EQ(fb.value().sender_ssrc, 0xCAFEBABEu);
    EXPECT_EQ(fb.value().media_ssrc,  0xFEEDFACEu);
    EXPECT_EQ(fb.value().base_seq, 42);
    EXPECT_EQ(fb.value().reference_time_24mhz, 0x00ABCDEFu);
    EXPECT_EQ(fb.value().reference_time_ms, 0x00ABCDEFu * 64u);
    EXPECT_EQ(fb.value().feedback_packet_count, 0x5A);
}

// ---------------------------------------------------------------------------
// T11: the arrival-time series is reconstructible -- the reference time plus
// the cumulative sum of the deltas is the arrival time of each packet, and
// the series is monotonic.
// ---------------------------------------------------------------------------
TEST(TransportCcFeedback, ArrivalSeriesIsReconstructible) {
    ByteBuffer body;
    append_u16(body, run_length_chunk(1, 3));
    append_u8(body, 0x10);   // 16 ticks
    append_u8(body, 0x10);   // 16 ticks
    append_u8(body, 0x10);   // 16 ticks

    auto pkt = make_transport_cc_packet(1, 2, 0, 3, 100u, 0, ByteSpan{body});
    Parser parser;
    auto fb = parser.parse_transport_cc(pkt);
    ASSERT_TRUE(fb.ok()) << fb.error().message();
    ASSERT_EQ(fb.value().packets.size(), 3u);

    // The reference time is in 64 ms units; the deltas in 250 us units.
    std::uint64_t arrival_us = fb.value().reference_time_ms * 1000u;
    std::uint64_t previous = 0;
    for (const auto& ps : fb.value().packets) {
        arrival_us += static_cast<std::uint64_t>(ps.delta_250us) * 250u;
        EXPECT_GE(arrival_us, previous);
        previous = arrival_us;
    }
    EXPECT_EQ(arrival_us, fb.value().reference_time_ms * 1000u + 3u * 16u * 250u);
}

// ---------------------------------------------------------------------------
// T12: a wrong feedback message type is rejected.
// ---------------------------------------------------------------------------
TEST(TransportCcFeedback, RejectsWrongFmt) {
    ByteBuffer body;
    append_u16(body, run_length_chunk(0, 1));
    auto pkt = make_transport_cc_packet(1, 2, 0, 1, 0, 0, ByteSpan{body});
    pkt[0] = static_cast<std::uint8_t>(0x80u | 1u);   // FMT = 1, not 15

    Parser parser;
    auto fb = parser.parse_transport_cc(pkt);
    EXPECT_FALSE(fb.ok());
}

// ---------------------------------------------------------------------------
// T13: truncated receive deltas are rejected -- a "received" symbol whose
// delta bytes are missing is a malformed message, not a silent zero.
// ---------------------------------------------------------------------------
TEST(TransportCcFeedback, RejectsTruncatedDeltas) {
    ByteBuffer body;
    append_u16(body, run_length_chunk(1, 2));   // 2 received packets
    append_u8(body, 0x10);                      // only one delta present

    auto pkt = make_transport_cc_packet(1, 2, 0, 2, 0, 0, ByteSpan{body});
    Parser parser;
    auto fb = parser.parse_transport_cc(pkt);
    EXPECT_FALSE(fb.ok());
}

// ---------------------------------------------------------------------------
// T14: truncated packet chunks are rejected -- the chunk section ends before
// the declared packet status count is covered.
// ---------------------------------------------------------------------------
TEST(TransportCcFeedback, RejectsTruncatedChunks) {
    ByteBuffer body;
    append_u16(body, run_length_chunk(0, 4));   // covers 4 packets only

    auto pkt = make_transport_cc_packet(1, 2, 0, 8, 0, 0, ByteSpan{body});
    Parser parser;
    auto fb = parser.parse_transport_cc(pkt);
    EXPECT_FALSE(fb.ok());
}

// ---------------------------------------------------------------------------
// T15: an empty packet status is rejected.
// ---------------------------------------------------------------------------
TEST(TransportCcFeedback, RejectsEmptyStatus) {
    ByteBuffer body;
    append_u16(body, run_length_chunk(0, 1));

    auto pkt = make_transport_cc_packet(1, 2, 0, 0, 0, 0, ByteSpan{body});
    Parser parser;
    auto fb = parser.parse_transport_cc(pkt);
    EXPECT_FALSE(fb.ok());
}
