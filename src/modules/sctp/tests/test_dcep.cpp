/**
 * @file src/sctp/tests/test_dcep.cpp
 * @brief Unit tests for the DCEP codec (RFC 8832 §5.1 / §8.2.2).
 *
 * Covers:
 *   1. DATA_CHANNEL_ACK encoding (type byte only).
 *   2. DATA_CHANNEL_OPEN encoding — header layout, label, protocol
 *      length, reliability parameter.
 *   3. Channel Type round-trip over all six registered values
 *      (0x00 / 0x01 / 0x02 / 0x80 / 0x81 / 0x82).
 *   4. Priority round-trip (u16 BE).
 *   5. Reliability Parameter: 0 for reliable channels, the TTL / the
 *      retransmit count for the partial-reliability classes.
 *   6. Malformed input is rejected (nullopt).
 *   7. The wire PPID for user payloads is the binary PPID (53).
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <nimrtc/plugins/datachannel.hpp>
#include <nimrtc/sctp/dcep.hpp>

namespace {

using nimrtc::plugins::DataChannelConfig;
using nimrtc::plugins::DataChannelReliability;
namespace sctp = nimrtc::sctp;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

DataChannelConfig make_cfg(DataChannelReliability rel, bool ordered) {
    DataChannelConfig cfg;
    cfg.label       = "telemetry";
    cfg.reliability = rel;
    cfg.ordered     = ordered;
    cfg.priority    = 128;
    return cfg;
}

/** Decode a DATA_CHANNEL_OPEN produced by `dcep_encode_open()`. */
std::optional<sctp::DcepOpenParams> round_trip(const DataChannelConfig& cfg) {
    const auto bytes = sctp::dcep_encode_open(cfg);
    return sctp::dcep_decode_open(bytes.data(), bytes.size());
}

} // namespace

// ---------------------------------------------------------------------------
// ACK
// ---------------------------------------------------------------------------
TEST(Dcep, AckEncodesToTypeByteOnly) {
    const auto ack = sctp::dcep_encode_ack();
    ASSERT_EQ(ack.size(), 1u);
    EXPECT_EQ(ack[0], sctp::kDcepAck);
}

// ---------------------------------------------------------------------------
// OPEN — header layout
// ---------------------------------------------------------------------------
TEST(Dcep, OpenHeaderLayout) {
    DataChannelConfig cfg;
    cfg.label    = "cmd";
    cfg.priority = 16;

    const auto bytes = sctp::dcep_encode_open(cfg);
    ASSERT_EQ(bytes.size(), 12u + cfg.label.size());

    EXPECT_EQ(bytes[0], sctp::kDcepOpen) << "message type must be 0x03";
    EXPECT_EQ(bytes[1], sctp::kDcepReliable)
        << "an ordered reliable channel encodes as 0x00";

    // Priority (u16 BE) — 16 lives in the low byte.
    EXPECT_EQ(bytes[2], 0x00);
    EXPECT_EQ(bytes[3], 16);

    // Reliability parameter (u32 BE) — 0 for a reliable channel.
    EXPECT_EQ(bytes[4], 0x00);
    EXPECT_EQ(bytes[5], 0x00);
    EXPECT_EQ(bytes[6], 0x00);
    EXPECT_EQ(bytes[7], 0x00);

    // Label length + protocol length (u16 BE each).
    EXPECT_EQ(bytes[8], 0x00);
    EXPECT_EQ(bytes[9], 3);
    EXPECT_EQ(bytes[10], 0x00);
    EXPECT_EQ(bytes[11], 0x00);

    EXPECT_EQ(std::string(bytes.begin() + 12, bytes.end()), "cmd");
}

// ---------------------------------------------------------------------------
// OPEN — Channel Type round-trip over the six registered values
// ---------------------------------------------------------------------------
TEST(Dcep, ChannelTypeReliableOrdered) {
    const auto params = round_trip(make_cfg(DataChannelReliability::kReliableOrdered,
                                             /*ordered=*/true));
    ASSERT_TRUE(params.has_value());
    EXPECT_EQ(params->reliability, DataChannelReliability::kReliableOrdered);
    EXPECT_TRUE(params->ordered);
    EXPECT_EQ(params->label, "telemetry");
}

TEST(Dcep, ChannelTypeReliableUnordered) {
    // NimRTC's "unreliable" is the SCTP_UNORDERED flag on a still
    // reliable association, so the wire value is 0x80 (reliable,
    // unordered).
    const auto cfg = make_cfg(DataChannelReliability::kUnreliable,
                              /*ordered=*/false);
    const auto bytes = sctp::dcep_encode_open(cfg);
    ASSERT_GE(bytes.size(), 2u);
    EXPECT_EQ(bytes[1], sctp::kDcepReliableUnordered);

    const auto params = round_trip(cfg);
    ASSERT_TRUE(params.has_value());
    EXPECT_EQ(params->reliability, DataChannelReliability::kUnreliable);
    EXPECT_FALSE(params->ordered);
}

TEST(Dcep, ChannelTypePartialReliableTimed) {
    auto cfg = make_cfg(DataChannelReliability::kPartialReliableTTL,
                        /*ordered=*/true);
    cfg.max_packet_lifetime_ms = 500;

    const auto bytes = sctp::dcep_encode_open(cfg);
    ASSERT_GE(bytes.size(), 2u);
    EXPECT_EQ(bytes[1], sctp::kDcepPartialReliableTimed);

    const auto params = round_trip(cfg);
    ASSERT_TRUE(params.has_value());
    EXPECT_EQ(params->reliability, DataChannelReliability::kPartialReliableTTL);
    EXPECT_TRUE(params->ordered);
    EXPECT_EQ(params->max_packet_lifetime_ms, 500);
}

TEST(Dcep, ChannelTypePartialReliableRexmit) {
    auto cfg = make_cfg(DataChannelReliability::kPartialReliableCount,
                        /*ordered=*/true);
    cfg.max_retransmits = 7;

    const auto bytes = sctp::dcep_encode_open(cfg);
    ASSERT_GE(bytes.size(), 2u);
    EXPECT_EQ(bytes[1], sctp::kDcepPartialReliableRexmit);

    const auto params = round_trip(cfg);
    ASSERT_TRUE(params.has_value());
    EXPECT_EQ(params->reliability, DataChannelReliability::kPartialReliableCount);
    EXPECT_TRUE(params->ordered);
    EXPECT_EQ(params->max_retransmits, 7);
}

TEST(Dcep, ChannelTypeUnorderedFlavoursSetTheHighBit) {
    // The unordered flag is the high bit of the Channel Type
    // (RFC 8832 §8.2.2) — the low bits keep the reliability class.
    auto timed = make_cfg(DataChannelReliability::kPartialReliableTTL,
                          /*ordered=*/false);
    timed.max_packet_lifetime_ms = 250;
    auto timed_bytes = sctp::dcep_encode_open(timed);
    ASSERT_GE(timed_bytes.size(), 2u);
    EXPECT_EQ(timed_bytes[1], sctp::kDcepPartialReliableTimedUnordered);

    auto rexmit = make_cfg(DataChannelReliability::kPartialReliableCount,
                           /*ordered=*/false);
    rexmit.max_retransmits = 3;
    auto rexmit_bytes = sctp::dcep_encode_open(rexmit);
    ASSERT_GE(rexmit_bytes.size(), 2u);
    EXPECT_EQ(rexmit_bytes[1], sctp::kDcepPartialReliableRexmitUnordered);

    const auto timed_params = round_trip(timed);
    ASSERT_TRUE(timed_params.has_value());
    EXPECT_EQ(timed_params->reliability, DataChannelReliability::kPartialReliableTTL);
    EXPECT_FALSE(timed_params->ordered);
    EXPECT_EQ(timed_params->max_packet_lifetime_ms, 250);

    const auto rexmit_params = round_trip(rexmit);
    ASSERT_TRUE(rexmit_params.has_value());
    EXPECT_EQ(rexmit_params->reliability, DataChannelReliability::kPartialReliableCount);
    EXPECT_FALSE(rexmit_params->ordered);
    EXPECT_EQ(rexmit_params->max_retransmits, 3);
}

TEST(Dcep, AllSixRegisteredChannelTypesDecode) {
    // Every IANA-registered Channel Type must decode into a
    // (reliability, ordered) pair — no "unassigned" hole.
    struct Case {
        std::uint8_t   type;
        DataChannelReliability reliability;
        bool           ordered;
    };
    const Case cases[] = {
        {sctp::kDcepReliable,                    DataChannelReliability::kReliableOrdered,     true},
        {sctp::kDcepPartialReliableRexmit,       DataChannelReliability::kPartialReliableCount, true},
        {sctp::kDcepPartialReliableTimed,        DataChannelReliability::kPartialReliableTTL,   true},
        {sctp::kDcepReliableUnordered,           DataChannelReliability::kUnreliable,           false},
        {sctp::kDcepPartialReliableRexmitUnordered, DataChannelReliability::kPartialReliableCount, false},
        {sctp::kDcepPartialReliableTimedUnordered,  DataChannelReliability::kPartialReliableTTL,   false},
    };

    for (const auto& c : cases) {
        // Build a minimal OPEN with the given Channel Type.
        std::vector<std::uint8_t> raw(12, 0);
        raw[0] = sctp::kDcepOpen;
        raw[1] = c.type;

        const auto params = sctp::dcep_decode_open(raw.data(), raw.size());
        ASSERT_TRUE(params.has_value())
            << "Channel Type 0x" << std::hex << static_cast<int>(c.type)
            << std::dec << " must decode";
        EXPECT_EQ(params->reliability, c.reliability);
        EXPECT_EQ(params->ordered, c.ordered);
    }
}

// ---------------------------------------------------------------------------
// Priority round-trip
// ---------------------------------------------------------------------------
TEST(Dcep, PriorityRoundTrips) {
    for (std::uint8_t p : {std::uint8_t{0}, std::uint8_t{16},
                            std::uint8_t{128}, std::uint8_t{255}}) {
        auto cfg = make_cfg(DataChannelReliability::kReliableOrdered, true);
        cfg.priority = p;
        const auto params = round_trip(cfg);
        ASSERT_TRUE(params.has_value());
        EXPECT_EQ(params->priority, static_cast<std::uint16_t>(p))
            << "priority must survive the DCEP round-trip";
    }
}

// ---------------------------------------------------------------------------
// Reliability parameter
// ---------------------------------------------------------------------------
TEST(Dcep, ReliableChannelHasZeroReliabilityParameter) {
    auto cfg = make_cfg(DataChannelReliability::kReliableOrdered, true);
    cfg.max_retransmits         = 9;   // ignored for a reliable channel
    cfg.max_packet_lifetime_ms  = 900; // ignored for a reliable channel

    const auto bytes = sctp::dcep_encode_open(cfg);
    ASSERT_GE(bytes.size(), 8u);
    EXPECT_EQ(bytes[4], 0x00);
    EXPECT_EQ(bytes[5], 0x00);
    EXPECT_EQ(bytes[6], 0x00);
    EXPECT_EQ(bytes[7], 0x00)
        << "RFC 8832 §5.1: the reliability parameter MUST be 0 for a "
           "reliable channel";
}

// ---------------------------------------------------------------------------
// Malformed input
// ---------------------------------------------------------------------------
TEST(Dcep, DecodeRejectsMalformedInput) {
    EXPECT_FALSE(sctp::dcep_decode_open(nullptr, 12).has_value())
        << "a null buffer must be rejected";

    const std::vector<std::uint8_t> too_short(11, 0);
    EXPECT_FALSE(sctp::dcep_decode_open(too_short.data(), too_short.size())
                     .has_value())
        << "a buffer shorter than the 12-byte header must be rejected";

    std::vector<std::uint8_t> wrong_type(12, 0);
    wrong_type[0] = 0x07;   // not a DCEP message type we know
    EXPECT_FALSE(sctp::dcep_decode_open(wrong_type.data(), wrong_type.size())
                     .has_value());

    std::vector<std::uint8_t> reserved_type(12, 0);
    reserved_type[0] = sctp::kDcepOpen;
    reserved_type[1] = 0x7f;   // Reserved by RFC 8832
    EXPECT_FALSE(sctp::dcep_decode_open(reserved_type.data(),
                                        reserved_type.size()).has_value())
        << "a reserved Channel Type must be rejected";

    std::vector<std::uint8_t> bad_length(12, 0);
    bad_length[0] = sctp::kDcepOpen;
    bad_length[8] = 0x00;
    bad_length[9] = 0x08;      // claims an 8-byte label ...
    EXPECT_FALSE(sctp::dcep_decode_open(bad_length.data(), bad_length.size())
                     .has_value())
        << "a label length that does not match the buffer must be rejected";
}

// ---------------------------------------------------------------------------
// PPID selection
// ---------------------------------------------------------------------------
TEST(Dcep, UserPayloadsUseTheBinaryPpid) {
    DataChannelConfig cfg;
    EXPECT_EQ(sctp::dcep_ppid_for_config(cfg), sctp::kPpidBinary)
        << "NimRTC's DataChannel carries opaque binary payloads, so the "
           "binary PPID (53) is the one on the wire";
    EXPECT_EQ(sctp::kPpidDcep,   50u);
    EXPECT_EQ(sctp::kPpidString, 51u);
    EXPECT_EQ(sctp::kPpidBinary, 53u);
}
