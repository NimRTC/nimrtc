/**
 * @file tests/test_transport_cc.cpp
 * @brief Tests for RFC 9143 abs-send-time and transport-wide CC feedback.
 *
 * Covers:
 *   T1 — abs-send-time round-trip (build → parse → equal)
 *   T2 — abs-send-time 24-bit wrap-around
 *   T3 — transport-cc parse: run length chunk
 *   T4 — transport-cc parse: status vector chunk (S_bit=0)
 *   T5 — transport-cc parse: status + delta chunk (S_bit=1) with deltas
 *   T6 — minimum valid feedback fixture (base_seq=0, 1 received packet)
 */

#include <gtest/gtest.h>

#include <cstring>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/rtp/packet.hpp>

using nimrtc::core::ByteBuffer;
using nimrtc::core::ByteSpan;
using namespace nimrtc::rtp;

// ---------------------------------------------------------------------------
// T1: abs-send-time round-trip — a fresh value emits RFC 5285 one-byte-form
// extension (profile 0xBEDE, id=1, 3-byte timestamp) and parses back equal.
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

    // Verify on-wire layout of the extension header:
    //   byte 12 (after fixed header) = 0xBE, byte 13 = 0xDE, byte 14 = 0x00, byte 15 = 0x01
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
// T2: 24-bit wrap — the field is masked to 24 bits at build time so
// setting a 32-bit value > 0xFFFFFF must round-trip the low 24 bits.
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

// ---------------------------------------------------------------------------
// Helper — build a minimal but well-formed transport-cc feedback packet.
// The transport-cc RTCP packet is laid out as:
//   4 bytes  RTCP header (V=2, P=0, FMT=15, PT=205, length)
//   4 bytes  sender SSRC
//   4 bytes  media SSRC
//   2 bytes  base seq
//   2 bytes  packet status count
//   3 bytes  reference time (24-bit)
//   1 byte   fb_ect_0_etc = 0 reserved for now
//   N bytes  packet chunks
// -----------------------------------------------------------------
static nimrtc::core::ByteBuffer make_transport_cc_packet(
    std::uint32_t sender_ssrc,
    std::uint32_t media_ssrc,
    std::uint16_t base_seq,
    std::uint16_t packet_status_count,
    std::uint32_t reference_time_24mhz,
    nimrtc::core::ByteSpan chunks) {

    // Header (4) + ssrc pair (8) + base seq (2) + status count (2)
    //   + reference time (3) + 1 reserved = 20 bytes prefix
    const std::size_t total = 20 + chunks.size();
    nimrtc::core::ByteBuffer buf(total, 0);

    buf[0] = 0x80u | 15u;             // V=2 (bits 6-7), P=0, FMT=15
    buf[1] = 205u;                    // PT = RTPFB
    const std::uint16_t length_words = static_cast<std::uint16_t>((total / 4u) - 1u);
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
        buf[off + 1] = static_cast<std::uint8_t>( v       & 0xFFu);
    };

    w32(4, sender_ssrc);
    w32(8, media_ssrc);
    w16(12, base_seq);
    w16(14, packet_status_count);
    // Reference time (24-bit), big-endian, padded to 4 bytes (offset 16..19)
    buf[16] = static_cast<std::uint8_t>((reference_time_24mhz >> 16) & 0xFFu);
    buf[17] = static_cast<std::uint8_t>((reference_time_24mhz >>  8) & 0xFFu);
    buf[18] = static_cast<std::uint8_t>( reference_time_24mhz        & 0xFFu);
    buf[19] = 0;   // fb_ect_0 + reserved

    std::memcpy(buf.data() + 20, chunks.data(), chunks.size());
    return buf;
}

// ---------------------------------------------------------------------------
// T3: Run length chunk — T=0, status=NotReceived, run=5 → 5 NotReceived.
// ---------------------------------------------------------------------------
TEST(TransportCcFeedback, RunLengthChunk) {
    // word: 0|00|run_length(13 bits) — status=00 (NotReceived), run=5
    const std::uint16_t word = static_cast<std::uint16_t>(0x0005u);
    const std::uint8_t bytes[2] = {
        static_cast<std::uint8_t>((word >> 8) & 0xFFu),
        static_cast<std::uint8_t>( word       & 0xFFu)};

    auto pkt = make_transport_cc_packet(
        /*sender*/ 0x10000001u, /*media*/ 0x20000002u,
        /*base seq*/ 1000, /*count*/ 5, /*ref time*/ 0x00123456u,
        nimrtc::core::ByteSpan{bytes, 2});

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
// T4: Status vector chunk (T=1, S=0) — 1-bit symbols.
// ---------------------------------------------------------------------------
TEST(TransportCcFeedback, StatusVectorChunk_1BitSymbols) {
    // word: 1|0|symbol_count(14 bits) — S=0, count=4
    const std::uint16_t word = static_cast<std::uint16_t>(0x8004u);
    // Symbol bits (4 × 1 bit, packed MSB-first into 1 byte):
    //   bit 0 = NotReceived, bit 0 = Received, bit 1 = NotReceived, bit 1 = Received
    //   so: 0 1 0 1 -> 0101 0000 -> 0x50
    const std::uint8_t symbol_byte = 0x50u;
    const std::uint8_t bytes[3] = {
        static_cast<std::uint8_t>((word >> 8) & 0xFFu),
        static_cast<std::uint8_t>( word       & 0xFFu),
        symbol_byte,
    };

    auto pkt = make_transport_cc_packet(
        0x1u, 0x2u, 1, 4, 0u,
        nimrtc::core::ByteSpan{bytes, 3});

    Parser parser;
    auto fb = parser.parse_transport_cc(pkt);
    ASSERT_TRUE(fb.ok()) << fb.error().message();
    ASSERT_EQ(fb.value().packets.size(), 4u);
    EXPECT_EQ(fb.value().packets[0].status, PacketStatus::NotReceived);
    EXPECT_EQ(fb.value().packets[1].status, PacketStatus::Received);
    EXPECT_EQ(fb.value().packets[2].status, PacketStatus::NotReceived);
    EXPECT_EQ(fb.value().packets[3].status, PacketStatus::Received);
}

// ---------------------------------------------------------------------------
// T5: Status + delta chunk (T=1, S=1) — 2-bit symbols + 14-bit delta per
// "received" packet. RFC 9143 §7.1.1.
// ---------------------------------------------------------------------------
TEST(TransportCcFeedback, StatusDeltaChunk_2BitsAndDeltas) {
    // word: 1|1|symbol_count=2 — S=1, count=2
    const std::uint16_t word = static_cast<std::uint16_t>(0xC002u);
    // Two 2-bit symbol values packed MSB-first into 1 byte:
    //   symbol 0 = 01 (Received, large delta), symbol 1 = 10 (ReceivedSmall)
    //   bits: 01 10 -> 0110 0000 -> 0x60
    const std::uint8_t symbols_byte = 0x60u;
    const std::uint8_t delta_bytes[4] = {0x3F, 0xFC, 0x01, 0x00};
    const std::uint8_t bytes[7] = {
        static_cast<std::uint8_t>((word >> 8) & 0xFFu),
        static_cast<std::uint8_t>( word       & 0xFFu),
        symbols_byte,
        delta_bytes[0], delta_bytes[1], delta_bytes[2], delta_bytes[3],
    };

    auto pkt = make_transport_cc_packet(
        0x1u, 0x2u, 0, 2, 0u,
        nimrtc::core::ByteSpan{bytes, 7});

    Parser parser;
    auto fb = parser.parse_transport_cc(pkt);
    ASSERT_TRUE(fb.ok()) << fb.error().message();
    ASSERT_EQ(fb.value().packets.size(), 2u);
    EXPECT_EQ(fb.value().packets[0].status, PacketStatus::Received);
    EXPECT_EQ(fb.value().packets[0].delta_250us, 0x0FFF);
    EXPECT_EQ(fb.value().packets[1].status, PacketStatus::ReceivedSmall);
    EXPECT_EQ(fb.value().packets[1].delta_250us, 0x0010);
}

// ---------------------------------------------------------------------------
// T6: minimum valid feedback — base seq=42, 1 received packet via run length.
// Exercises the full header parse + chunk loop body in one go.
// ---------------------------------------------------------------------------
TEST(TransportCcFeedback, MinimumValidFeedback) {
    // Run length, status=Received (01), run=1
    const std::uint16_t word = static_cast<std::uint16_t>(0x2001u);
    const std::uint8_t bytes[2] = {
        static_cast<std::uint8_t>((word >> 8) & 0xFFu),
        static_cast<std::uint8_t>( word       & 0xFFu)};

    auto pkt = make_transport_cc_packet(
        0xCAFEBABEu, 0xFEEDFACEu, 42, 1, 0x00ABCDEFu,
        nimrtc::core::ByteSpan{bytes, 2});

    Parser parser;
    auto fb = parser.parse_transport_cc(pkt);
    ASSERT_TRUE(fb.ok()) << fb.error().message();
    EXPECT_EQ(fb.value().sender_ssrc, 0xCAFEBABEu);
    EXPECT_EQ(fb.value().media_ssrc,  0xFEEDFACEu);
    EXPECT_EQ(fb.value().base_seq, 42);
    EXPECT_EQ(fb.value().reference_time_24mhz, 0x00ABCDEFu);
    ASSERT_EQ(fb.value().packets.size(), 1u);
    EXPECT_EQ(fb.value().packets[0].status, PacketStatus::Received);
}
