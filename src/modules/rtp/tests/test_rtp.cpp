// modules/rtp/tests/test_rtp.cpp
// =============================================================================
// RTP module tests — covers data-structure invariants, parser/builder
// round-trips, error paths, and the RFC 3550 / RFC 5285 fixtures.
// =============================================================================

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/error.hpp>
#include <nimrtc/rtp/packet.hpp>

namespace {

using namespace nimrtc::rtp;
using nimrtc::core::ByteSpan;
using nimrtc::core::ErrorCode;
using nimrtc::core::Result;

namespace bspan {

// Build a ByteSpan from an initializer-list of bytes (tests only).
inline ByteSpan of(std::initializer_list<std::uint8_t> il) {
    static std::vector<std::uint8_t> storage;  // single-threaded test, OK
    storage.assign(il);
    return ByteSpan{storage.data(), storage.size()};
}

}  // namespace bspan

// -----------------------------------------------------------------------------
// PacketView — default construction
// -----------------------------------------------------------------------------
TEST(RtpPacketView, DefaultConstructsToZero) {
    PacketView pv;
    EXPECT_EQ(pv.ssrc, 0u);
    EXPECT_EQ(pv.seq, 0u);
    EXPECT_EQ(pv.timestamp, 0u);
    EXPECT_EQ(pv.payload_type, 0u);
    EXPECT_FALSE(pv.marker);
    EXPECT_TRUE(pv.csrc.empty());
    EXPECT_FALSE(pv.extension.has_value());
    EXPECT_TRUE(pv.extmap_uri.empty());
    EXPECT_FALSE(pv.arrival.has_value());
}

// -----------------------------------------------------------------------------
// PacketBuilder — fluent setter chain
// -----------------------------------------------------------------------------
TEST(RtpPacketBuilder, FluentSetters) {
    PacketBuilder b;
    auto& ref = b.set_ssrc(0x12345678)
                    .set_seq(42)
                    .set_timestamp(0xDEADBEEF)
                    .set_payload_type(96)
                    .set_marker(true);

    // Chain returns self.
    EXPECT_EQ(&ref, &b);

    // Values are stored.
    EXPECT_EQ(b.compute_size(), 12u);  // fixed header only
}

TEST(RtpPacketBuilder, ComputeSizeAccountsForAllFields) {
    PacketBuilder b;
    b.set_ssrc(0xCAFEBABE)
        .set_seq(7)
        .set_timestamp(12345)
        .set_payload_type(96)
        .add_csrc(0x11111111)
        .add_csrc(0x22222222)
        .set_extension(0xBEDE, bspan::of({1, 2, 3, 4}))    // 4 bytes → +8 (header + 1 word)
        .set_payload(bspan::of({'a', 'b', 'c'}))
        .set_padding(4);                       // 4 padding-data bytes + 1 count byte

    // 12 (fixed) + 8 (csrc) + 8 (ext header + 1 word) + 3 (payload) + 5 (pad+count) = 36
    EXPECT_EQ(b.compute_size(), 36u);
}

TEST(RtpPacketBuilder, ResetClearsState) {
    PacketBuilder b;
    b.set_ssrc(1).set_seq(2).set_timestamp(3).set_payload_type(96);
    b.reset();
    EXPECT_EQ(b.compute_size(), 12u);  // back to fixed header only
}

// -----------------------------------------------------------------------------
// Seq helpers
// -----------------------------------------------------------------------------
TEST(RtpSeqHelpers, SeqIsNewer) {
    EXPECT_TRUE(seq_is_newer(5, 4));
    EXPECT_TRUE(seq_is_newer(0, 65535));   // wrap-around
    EXPECT_FALSE(seq_is_newer(4, 5));
    EXPECT_FALSE(seq_is_newer(100, 100));
}

TEST(RtpSeqHelpers, SeqDistance) {
    // Forward distance
    EXPECT_EQ(seq_distance(5, 3), 2u);
    // Wrap-around: 0->65535 means distance 1
    EXPECT_EQ(seq_distance(0, 65535), 1u);
    // Same → 0
    EXPECT_EQ(seq_distance(42, 42), 0u);
}

// -----------------------------------------------------------------------------
// RTCP enums
// -----------------------------------------------------------------------------
TEST(RtcpTypes, EnumValuesMatchRfc3550) {
    EXPECT_EQ(static_cast<std::uint8_t>(RtcpType::SR), 200u);
    EXPECT_EQ(static_cast<std::uint8_t>(RtcpType::RR), 201u);
    EXPECT_EQ(static_cast<std::uint8_t>(RtcpType::SDES), 202u);
    EXPECT_EQ(static_cast<std::uint8_t>(RtcpType::BYE), 203u);
    EXPECT_EQ(static_cast<std::uint8_t>(RtcpType::RTPFB), 205u);
    EXPECT_EQ(static_cast<std::uint8_t>(RtcpType::PSFB), 206u);
}

TEST(RtcpFbKind, Values) {
    EXPECT_EQ(static_cast<std::uint8_t>(RtcpFbKind::GenericNACK), 1u);
    EXPECT_EQ(static_cast<std::uint8_t>(RtcpFbKind::PLI), 6u);
    EXPECT_EQ(static_cast<std::uint8_t>(RtcpFbKind::FIR), 8u);
}

TEST(RtcpPsFbKind, RembValue) {
    EXPECT_EQ(static_cast<std::uint8_t>(RtcpPsFbKind::REMB), 15u);
}

// -----------------------------------------------------------------------------
// Parser — state (extension URI table)
// -----------------------------------------------------------------------------
TEST(RtpParser, ExtensionUriTable) {
    Parser p;
    EXPECT_EQ(p.declared_extension_uri_count(), 0u);
    EXPECT_FALSE(p.declared_extension_uri(1).has_value());

    p.declare_extension_uri(1, "urn:ietf:params:rtp-hdrext:sdes:mid");
    p.declare_extension_uri(4, "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id");

    EXPECT_EQ(p.declared_extension_uri_count(), 2u);
    ASSERT_TRUE(p.declared_extension_uri(1).has_value());
    EXPECT_EQ(*p.declared_extension_uri(1), "urn:ietf:params:rtp-hdrext:sdes:mid");
    EXPECT_FALSE(p.declared_extension_uri(99).has_value());

    p.clear_extension_uris();
    EXPECT_EQ(p.declared_extension_uri_count(), 0u);
}

// -----------------------------------------------------------------------------
// Parser — error paths
// -----------------------------------------------------------------------------
TEST(RtpParser, RejectsTooShort) {
    Parser p;
    auto r = p.parse(bspan::of({0x80, 0x00, 0x00}));  // only 3 bytes
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), ErrorCode::ProtocolError);
}

TEST(RtpParser, RejectsBadVersion) {
    Parser p;
    auto r = p.parse(bspan::of({
        0xC0, 0x00, 0x00, 0x01,    // version=3 (top bits = 11)
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x01,
    }));
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), ErrorCode::ProtocolError);
    EXPECT_NE(r.error().message().find("version"), std::string::npos);
}

// -----------------------------------------------------------------------------
// Parser + Builder — minimal round-trip (no extension, no padding)
// -----------------------------------------------------------------------------
TEST(RtpRoundTrip, MinimalPacket) {
    // Build a fixed 12-byte header + 4-byte payload.
    auto buf = PacketBuilder()
        .set_ssrc(0x12345678)
        .set_seq(42)
        .set_timestamp(0xDEADBEEF)
        .set_payload_type(96)
        .set_marker(true)
        .set_payload(bspan::of({'h','e','l','l','o','!'}))
        .build();
    ASSERT_EQ(buf.size(), 18u);

    Parser p;
    auto r = p.parse(ByteSpan{buf.data(), buf.size()});
    ASSERT_TRUE(r.ok()) << r.error().message();

    const auto& pv = r.value();
    EXPECT_EQ(pv.ssrc, 0x12345678u);
    EXPECT_EQ(pv.seq, 42u);
    EXPECT_EQ(pv.timestamp, 0xDEADBEEFu);
    EXPECT_EQ(pv.payload_type, 96u);
    EXPECT_TRUE(pv.marker);
    EXPECT_TRUE(pv.csrc.empty());
    EXPECT_FALSE(pv.extension.has_value());
    ASSERT_EQ(pv.payload.size(), 6u);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(pv.payload.data()), pv.payload.size()),
              "hello!");
}

TEST(RtpRoundTrip, CsrcListRoundTrip) {
    auto buf = PacketBuilder()
        .set_ssrc(0xCAFEBABE)
        .set_seq(7)
        .set_timestamp(12345)
        .set_payload_type(98)
        .add_csrc(0xAAAAAAAA)
        .add_csrc(0xBBBBBBBB)
        .add_csrc(0xCCCCCCCC)
        .set_payload(bspan::of({1, 2, 3}))
        .build();

    Parser p;
    auto r = p.parse(ByteSpan{buf.data(), buf.size()});
    ASSERT_TRUE(r.ok()) << r.error().message();

    const auto& pv = r.value();
    ASSERT_EQ(pv.csrc.size(), 3u);
    EXPECT_EQ(pv.csrc[0], 0xAAAAAAAAu);
    EXPECT_EQ(pv.csrc[1], 0xBBBBBBBBu);
    EXPECT_EQ(pv.csrc[2], 0xCCCCCCCCu);
    EXPECT_EQ(pv.payload.size(), 3u);
}

// -----------------------------------------------------------------------------
// Parser — one-byte RFC 5285 extension walk
// -----------------------------------------------------------------------------
TEST(RtpParser, OneByteExtensionWalk) {
    // Build an RTP packet with a one-byte extension containing two elements:
    //   id=1, len=2, data="ab"   (4 bytes total after id|len)
    //   id=4, len=1, data="x"    (2 bytes total after id|len)
    //   terminator 0x00          (1 byte)
    //   1 byte padding           → total data = 8 bytes → 1 word (4 bytes)
    //   But 8 bytes doesn't align to 4 nicely; use exactly 8 → 2 words.
    //
    // We'll use: 1 hdr(1) + 2 data + 1 hdr(1) + 1 data + 1 term + 2 pad = 8 bytes = 2 words.
    std::vector<std::uint8_t> packet = {
        0x90, 0x60,                          // V=2, P=0, X=1, CC=0, M=0, PT=96
        0x00, 0x01,                          // seq=1
        0x00, 0x00, 0x00, 0x0A,              // timestamp=10
        0x12, 0x34, 0x56, 0x78,              // ssrc
        0xBE, 0xDE,                          // one-byte profile
        0x00, 0x02,                          // length = 2 words = 8 bytes
        // extension data (8 bytes):
        0x12, 0x61, 0x62,                    // id=1 len=2 data="ab"
        0x41, 0x78,                          // id=4 len=1 data="x"
        0x00,                                // terminator
        0x00, 0x00,                          // padding
        // payload:
        0xDE, 0xAD, 0xBE, 0xEF,
    };

    Parser p;
    p.declare_extension_uri(1, "urn:ietf:params:rtp-hdrext:sdes:mid");
    p.declare_extension_uri(4, "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id");

    auto r = p.parse(ByteSpan{packet.data(), packet.size()});
    ASSERT_TRUE(r.ok()) << r.error().message();

    const auto& pv = r.value();
    ASSERT_TRUE(pv.extension.has_value());
    EXPECT_EQ(pv.extension->type, 0xBEDEu);
    ASSERT_EQ(pv.extmap_uri.size(), 2u);
    EXPECT_EQ(pv.extmap_uri.at(1), "urn:ietf:params:rtp-hdrext:sdes:mid");
    EXPECT_EQ(pv.extmap_uri.at(4), "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id");
    EXPECT_EQ(pv.payload.size(), 4u);
}

TEST(RtpParser, OneByteExtensionEmptyUriTableSkipsWalk) {
    // Same packet as above, but Parser has no URI declarations → extmap_uri empty,
    // but raw extension data is still preserved.
    std::vector<std::uint8_t> packet = {
        0x90, 0x60,
        0x00, 0x01,
        0x00, 0x00, 0x00, 0x0A,
        0x12, 0x34, 0x56, 0x78,
        0xBE, 0xDE,
        0x00, 0x01,                          // length = 1 word = 4 bytes
        0x12, 0x61, 0x62, 0x00,              // id=1 len=2 data="ab" + 1 pad byte
        0xDE, 0xAD, 0xBE, 0xEF,              // payload
    };
    Parser p;
    auto r = p.parse(ByteSpan{packet.data(), packet.size()});
    ASSERT_TRUE(r.ok()) << r.error().message();
    EXPECT_TRUE(r.value().extmap_uri.empty());
    ASSERT_TRUE(r.value().extension.has_value());
    EXPECT_EQ(r.value().extension->data.size(), 4u);
}

// -----------------------------------------------------------------------------
// Parser — padding handling
// -----------------------------------------------------------------------------
TEST(RtpParser, PaddingTrimmedFromPayload) {
    // Build manually: header(12) + payload(4) + pad(2) + pad_count(1) = 19
    // Last byte = 2 (padding count).
    std::vector<std::uint8_t> packet = {
        0xA0, 0x60,                          // V=2, P=1, X=0, CC=0, M=0, PT=96
        0x00, 0x01,
        0x00, 0x00, 0x00, 0x0A,
        0x12, 0x34, 0x56, 0x78,
        0xDE, 0xAD, 0xBE, 0xEF,              // payload (4 bytes)
        0x00, 0x00,                          // padding bytes (2)
        0x02,                                // last byte = pad count = 2
    };
    Parser p;
    auto r = p.parse(ByteSpan{packet.data(), packet.size()});
    ASSERT_TRUE(r.ok()) << r.error().message();
    EXPECT_EQ(r.value().payload.size(), 4u);
    // Raw is the full packet including pad.
    EXPECT_EQ(r.value().raw.size(), 19u);
}

TEST(RtpParser, InvalidPaddingCountRejected) {
    // Pad count of 0 must be rejected.
    std::vector<std::uint8_t> packet = {
        0xA0, 0x60,
        0x00, 0x01,
        0x00, 0x00, 0x00, 0x0A,
        0x12, 0x34, 0x56, 0x78,
        0xDE, 0xAD, 0xBE, 0xEF,
        0x00,                                // pad count = 0 — invalid
    };
    Parser p;
    auto r = p.parse(ByteSpan{packet.data(), packet.size()});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code(), ErrorCode::ProtocolError);
}

// -----------------------------------------------------------------------------
// Builder + Parser — full round-trip with extension
// -----------------------------------------------------------------------------
TEST(RtpRoundTrip, PacketWithExtension) {
    auto buf = PacketBuilder()
        .set_ssrc(0xAAAA)
        .set_seq(100)
        .set_timestamp(2000)
        .set_payload_type(111)
        .set_marker(false)
        .set_extension(0xBEDE, bspan::of({0x12, 0x61, 0x62, 0x00}))
        .set_payload(bspan::of({'p', 'a', 'y'}))
        .build();

    Parser p;
    auto r = p.parse(ByteSpan{buf.data(), buf.size()});
    ASSERT_TRUE(r.ok()) << r.error().message();

    const auto& pv = r.value();
    EXPECT_EQ(pv.ssrc, 0xAAAAu);
    EXPECT_EQ(pv.seq, 100u);
    EXPECT_EQ(pv.timestamp, 2000u);
    EXPECT_EQ(pv.payload_type, 111u);
    EXPECT_FALSE(pv.marker);
    ASSERT_TRUE(pv.extension.has_value());
    EXPECT_EQ(pv.extension->type, 0xBEDEu);
    EXPECT_EQ(pv.payload.size(), 3u);
}

TEST(RtpRoundTrip, PacketWithPadding) {
    auto buf = PacketBuilder()
        .set_ssrc(0x5555)
        .set_seq(10)
        .set_timestamp(20)
        .set_payload_type(96)
        .set_payload(bspan::of({1, 2, 3}))
        .set_padding(4)                       // 4 bytes of zero padding + 1 trailer
        .build();

    Parser p;
    auto r = p.parse(ByteSpan{buf.data(), buf.size()});
    ASSERT_TRUE(r.ok()) << r.error().message();

    const auto& pv = r.value();
    EXPECT_EQ(pv.payload.size(), 3u);   // padding stripped
    EXPECT_EQ(pv.raw.size(), buf.size());
}

// -----------------------------------------------------------------------------
// RTCP — round-trip tests for SR / RR / NACK (in-process; no UDP).
// -----------------------------------------------------------------------------
TEST(Rtcp, SrRoundTrip) {
    SrBuilder b;
    b.set_ssrc(0xAABBCCDD)
        .set_ntp_timestamp(((std::uint64_t)0x12345678 << 32) | 0x9ABCDEF0u)
        .set_rtp_timestamp(0xDEADBEEFu)
        .set_sender_packet_count(12345)
        .set_sender_octet_count(987654321);
    b.add_report_block(ReportBlock{
        0x11111111u, 13, -7,
        0x00010000u + 0x1234u, 0x00010002u, 0x80000000u, 0x00010003u});

    auto buf = b.build();
    Parser p;
    auto r = p.parse_sr(ByteSpan{buf.data(), buf.size()});
    ASSERT_TRUE(r.ok()) << r.error().message();
    const auto& sr = r.value();
    EXPECT_EQ(sr.ssrc, 0xAABBCCDDu);
    EXPECT_EQ(sr.ntp_timestamp,
              ((std::uint64_t)0x12345678 << 32) | 0x9ABCDEF0u);
    EXPECT_EQ(sr.rtp_timestamp, 0xDEADBEEFu);
    EXPECT_EQ(sr.sender_packet_count, 12345u);
    EXPECT_EQ(sr.sender_octet_count, 987654321u);
    ASSERT_EQ(sr.report_blocks.size(), 1u);
    EXPECT_EQ(sr.report_blocks[0].cumulative_packets_lost, -7);
}

TEST(Rtcp, RrRoundTrip) {
    RrBuilder b;
    b.set_ssrc(0xF1F1F1F1u);
    b.add_report_block(ReportBlock{
        0xFEEDFACEu, 0, -1,
        0x00070000u + 0xBE, 0x00070010u, 0u, 0u});
    auto buf = b.build();
    Parser p;
    auto r = p.parse_rr(ByteSpan{buf.data(), buf.size()});
    ASSERT_TRUE(r.ok()) << r.error().message();
    EXPECT_EQ(r.value().ssrc, 0xF1F1F1F1u);
    ASSERT_EQ(r.value().report_blocks.size(), 1u);
    EXPECT_EQ(r.value().report_blocks[0].cumulative_packets_lost, -1);
}

TEST(Rtcp, NackRoundTrip) {
    NackBuilder b;
    b.set_sender_ssrc(0x44444444u)
        .set_media_ssrc(0x55555555u)
        .add_entry(100, 0x8001)
        .add_entry(200, 0xFFFF)
        .add_entry(300, 0);
    auto buf = b.build();
    Parser p;
    auto r = p.parse_nack(ByteSpan{buf.data(), buf.size()});
    ASSERT_TRUE(r.ok()) << r.error().message();
    EXPECT_EQ(r.value().sender_ssrc, 0x44444444u);
    EXPECT_EQ(r.value().media_ssrc,  0x55555555u);
    ASSERT_EQ(r.value().entries.size(), 3u);
    EXPECT_EQ(r.value().entries[0].packet_id, 100);
    EXPECT_EQ(r.value().entries[0].blp, 0x8001);
    EXPECT_EQ(r.value().entries[2].blp, 0u);
}

TEST(Rtcp, SrZeroReportBlocks) {
    // Edge case: SR with RC=0 is legal — 28-byte packed (4 hdr + 4 ssrc + 20 sender info).
    auto buf = SrBuilder{}.set_ssrc(0x123u).build();
    EXPECT_EQ(buf.size(), 28u);
    // Verify the length field equals total_words - 1 = (28/4) - 1 = 6.
    const std::uint16_t length_words =
        (std::uint16_t(buf[2]) << 8) | buf[3];
    EXPECT_EQ(length_words, 6u);

    Parser p;
    auto r = p.parse_sr(ByteSpan{buf.data(), buf.size()});
    ASSERT_TRUE(r.ok()) << r.error().message();
    EXPECT_EQ(r.value().ssrc, 0x123u);
    EXPECT_EQ(r.value().report_blocks.size(), 0u);
}

}  // namespace
