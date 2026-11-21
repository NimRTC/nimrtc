// modules/ice/tests/test_tcp_framing.cpp
// =============================================================================
// RFC 6544 §3.2 stream framing unit tests.
//
// TCP is a byte stream: one recv() is not one STUN message.  Every STUN
// message is prefixed with a two-octet network-order length that counts
// the STUN message *including* its 20-byte header.  These tests pin the
// frame/unframe pair against the wire format so the connectivity-check
// driver can trust it.
// =============================================================================

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "tcp_framing.hpp"

namespace {

using nimrtc::ice::frame_message;
using nimrtc::ice::TcpUnframer;

std::vector<std::uint8_t> bytes(std::string s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

TEST(TcpFraming, FramePrefixCountsStunIncludingHeader) {
    // A 20-byte STUN header + 4-byte attribute = 24 bytes total.  The
    // prefix must be 24 (RFC 6544 §3.2: "the two-octet length field ...
    // indicates the length of the STUN message ... including the header"),
    // NOT 4 and NOT 26.
    std::vector<std::uint8_t> msg(24, 0xAB);
    const auto framed = frame_message(msg);
    ASSERT_EQ(framed.size(), 26u);
    EXPECT_EQ(framed[0], 0x00);
    EXPECT_EQ(framed[1], 0x18);  // 24, big-endian
    EXPECT_EQ(std::vector<std::uint8_t>(framed.begin() + 2, framed.end()), msg);
}

TEST(TcpFraming, UnframeSingleMessage) {
    std::vector<std::uint8_t> msg(20, 0x01);
    const auto framed = frame_message(msg);

    TcpUnframer u;
    std::vector<std::vector<std::uint8_t>> out;
    ASSERT_TRUE(u.push(framed, out));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], msg);
    EXPECT_TRUE(u.empty());
}

TEST(TcpFraming, UnframeStreamSplitAcrossPushes) {
    std::vector<std::uint8_t> a(20, 0x11);
    std::vector<std::uint8_t> b(32, 0x22);
    const auto framed_a = frame_message(a);
    const auto framed_b = frame_message(b);

    // Deliver one byte at a time — the worst case for a stream consumer.
    TcpUnframer u;
    std::vector<std::vector<std::uint8_t>> out;
    std::vector<std::uint8_t> all;
    all.insert(all.end(), framed_a.begin(), framed_a.end());
    all.insert(all.end(), framed_b.begin(), framed_b.end());
    for (const auto byte : all) {
        const std::vector<std::uint8_t> chunk{byte};
        ASSERT_TRUE(u.push(chunk, out));
    }
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], a);
    EXPECT_EQ(out[1], b);
    EXPECT_TRUE(u.empty());
}

TEST(TcpFraming, UnframeTwoMessagesInOnePush) {
    std::vector<std::uint8_t> a(20, 0x33);
    std::vector<std::uint8_t> b(44, 0x44);
    const auto framed_a = frame_message(a);
    const auto framed_b = frame_message(b);

    std::vector<std::uint8_t> all;
    all.insert(all.end(), framed_a.begin(), framed_a.end());
    all.insert(all.end(), framed_b.begin(), framed_b.end());

    TcpUnframer u;
    std::vector<std::vector<std::uint8_t>> out;
    ASSERT_TRUE(u.push(all, out));
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], a);
    EXPECT_EQ(out[1], b);
}

TEST(TcpFraming, RejectsLengthShorterThanAStunHeader) {
    // A frame whose declared length is < 20 cannot be a STUN message; the
    // unframer must declare the stream dead rather than emit garbage.
    std::vector<std::uint8_t> framed = {0x00, 0x05, 'h', 'e', 'l', 'l', 'o'};
    TcpUnframer u;
    std::vector<std::vector<std::uint8_t>> out;
    EXPECT_FALSE(u.push(framed, out));
}

TEST(TcpFraming, RejectsOversizeMessage) {
    // 0xFFFF is the largest length a 16-bit prefix can express; anything
    // beyond that cannot be framed and must yield an empty result.
    std::vector<std::uint8_t> msg(70000, 0x00);
    const auto framed = frame_message(msg);
    EXPECT_TRUE(framed.empty());
}

TEST(TcpFraming, ResetDropsPartialBuffer) {
    std::vector<std::uint8_t> msg(20, 0x55);
    const auto framed = frame_message(msg);
    // Push only the length prefix, then reset.
    TcpUnframer u;
    std::vector<std::vector<std::uint8_t>> out;
    ASSERT_TRUE(u.push(std::vector<std::uint8_t>(framed.begin(), framed.begin() + 2), out));
    EXPECT_TRUE(out.empty());
    EXPECT_FALSE(u.empty());
    u.reset();
    EXPECT_TRUE(u.empty());
    // A fresh connection (same unframer after reset) works again.
    ASSERT_TRUE(u.push(framed, out));
    ASSERT_EQ(out.size(), 1u);
}

}  // namespace
