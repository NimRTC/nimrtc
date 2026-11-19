/**
 * @file src/modules/rtx/tests/test_rtx.cpp
 * @brief The RFC 4588 retransmission payload format: OSN framing + the
 *        `apt=` / `rtx-time=` SDP parameters.
 *
 * ## What is pinned here
 *
 * The framing is the one thing both ends of a retransmission have to
 * agree on byte-for-byte, so the tests are written against the
 * *normative text* (RFC 4588 §4.1) rather than against the
 * implementation:
 *
 *   "The length of the retransmission payload header is 2 octets.  This
 *    payload header contains only one field, OSN (original sequence
 *    number) ...  The original RTP packet payload, including any
 *    possible payload headers specific to the original payload type,
 *    MUST be placed right after the retransmission payload header."
 *
 * In particular there is **no marker byte** before the OSN — the
 * header is exactly two octets wide.
 */

#include <gtest/gtest.h>

#include <nimrtc/rtx/rtx.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using nimrtc::core::ByteSpan;
using nimrtc::core::MutableByteSpan;

/// A byte span over a std::vector, for the builder's output.
MutableByteSpan mutable_span(std::vector<std::uint8_t>& v) {
    return MutableByteSpan{v.data(), v.size()};
}

ByteSpan const_span(const std::vector<std::uint8_t>& v) {
    return ByteSpan{v.data(), v.size()};
}

// ---------------------------------------------------------------------------
// The header is exactly two octets
// ---------------------------------------------------------------------------

TEST(rtx_format, the_header_is_two_octets) {
    EXPECT_EQ(nimrtc::rtx::kHeaderSize, 2u);
    EXPECT_EQ(nimrtc::rtx::kMinPayloadSize, 3u);
}

TEST(rtx_format, a_wrapped_payload_is_the_header_plus_the_original) {
    const std::vector<std::uint8_t> original{0xAA, 0xBB, 0xCC};

    std::vector<std::uint8_t> out(nimrtc::rtx::payload_size_for(const_span(original)));
    const std::size_t written =
        nimrtc::rtx::build_payload(mutable_span(out), 0x1234, const_span(original));

    ASSERT_EQ(written, out.size());
    ASSERT_EQ(written, 5u);

    // The OSN comes first, in network byte order.
    EXPECT_EQ(out[0], 0x12u);
    EXPECT_EQ(out[1], 0x34u);
    // Then the original payload, verbatim.
    EXPECT_EQ(out[2], 0xAAu);
    EXPECT_EQ(out[3], 0xBBu);
    EXPECT_EQ(out[4], 0xCCu);
}

TEST(rtx_format, there_is_no_marker_byte_before_the_osn) {
    // The regression this pins: a description of RTX that puts a 0x90
    // marker in front of the OSN.  RFC 4588 §4.1 has no such byte --
    // the header is two octets and holds only the OSN -- so a peer
    // following the RFC would read the marker as the high byte of the
    // OSN and restore a sequence number that is 0x9000 too large.
    const std::vector<std::uint8_t> original{0x01, 0x02};

    std::vector<std::uint8_t> out(nimrtc::rtx::payload_size_for(const_span(original)));
    ASSERT_EQ(nimrtc::rtx::build_payload(mutable_span(out), 0x0102,
                                          const_span(original)),
              4u);

    EXPECT_EQ(out[0], 0x01u);   // OSN high byte -- NOT a marker.
    EXPECT_EQ(out[1], 0x02u);   // OSN low byte.
    EXPECT_NE(out[0], 0x90u);

    // And the round trip restores exactly the original sequence number.
    const auto parsed = nimrtc::rtx::parse_payload(const_span(out));
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.value().osn, 0x0102u);
}

// ---------------------------------------------------------------------------
// The round trip
// ---------------------------------------------------------------------------

TEST(rtx_format, the_round_trip_restores_the_sequence_number_and_payload) {
    for (std::uint16_t osn : {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{0x00FF},
                               std::uint16_t{0xFF00}, std::uint16_t{0x7FFF},
                               std::uint16_t{0xFFFF}}) {
        const std::vector<std::uint8_t> original{0xDE, 0xAD, 0xBE, 0xEF};

        std::vector<std::uint8_t> out(
            nimrtc::rtx::payload_size_for(const_span(original)));
        ASSERT_EQ(nimrtc::rtx::build_payload(mutable_span(out), osn,
                                              const_span(original)),
                  out.size());

        const auto parsed = nimrtc::rtx::parse_payload(const_span(out));
        ASSERT_TRUE(parsed.ok()) << "osn=" << osn;
        EXPECT_EQ(parsed.value().osn, osn);
        ASSERT_EQ(parsed.value().original.size(), original.size());
        for (std::size_t i = 0; i < original.size(); ++i) {
            EXPECT_EQ(parsed.value().original[i], original[i]);
        }
    }
}

TEST(rtx_format, the_round_trip_survives_an_empty_original_payload) {
    // A zero-length original payload is degenerate but must not corrupt
    // the framing: the OSN still round-trips.
    const std::vector<std::uint8_t> original;

    std::vector<std::uint8_t> out(nimrtc::rtx::payload_size_for(const_span(original)));
    ASSERT_EQ(nimrtc::rtx::build_payload(mutable_span(out), 0x4321,
                                          const_span(original)),
              2u);

    const auto parsed = nimrtc::rtx::parse_payload(const_span(out));
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.value().osn, 0x4321u);
    EXPECT_TRUE(parsed.value().original.empty());
}

// ---------------------------------------------------------------------------
// The builder's bounds
// ---------------------------------------------------------------------------

TEST(rtx_format, the_builder_refuses_a_buffer_that_is_too_small) {
    const std::vector<std::uint8_t> original{0x01, 0x02, 0x03};

    // One byte short of what is needed.
    std::vector<std::uint8_t> out(nimrtc::rtx::payload_size_for(const_span(original)) - 1);
    EXPECT_EQ(nimrtc::rtx::build_payload(mutable_span(out), 0x1234,
                                         const_span(original)),
              0u);
}

TEST(rtx_format, the_builder_accepts_an_exactly_sized_buffer) {
    const std::vector<std::uint8_t> original{0x01, 0x02, 0x03};
    std::vector<std::uint8_t> out(nimrtc::rtx::payload_size_for(const_span(original)));
    EXPECT_EQ(nimrtc::rtx::build_payload(mutable_span(out), 0x1234,
                                         const_span(original)),
              out.size());
}

// ---------------------------------------------------------------------------
// The parser's bounds
// ---------------------------------------------------------------------------

TEST(rtx_format, the_parser_rejects_a_payload_shorter_than_the_header) {
    const std::vector<std::uint8_t> too_short{0x01};
    const auto parsed = nimrtc::rtx::parse_payload(const_span(too_short));
    EXPECT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.error().code(), nimrtc::core::ErrorCode::ProtocolError);

    const std::vector<std::uint8_t> empty;
    EXPECT_FALSE(nimrtc::rtx::parse_payload(const_span(empty)).ok());
}

TEST(rtx_format, the_parser_into_variant_rejects_a_short_payload) {
    const std::vector<std::uint8_t> too_short{0x01};
    std::uint16_t osn = 0;
    nimrtc::core::ByteSpan body;
    std::vector<std::uint8_t> storage(16);
    const auto rc = nimrtc::rtx::parse_payload_into(
        const_span(too_short), osn, body, mutable_span(storage));
    EXPECT_FALSE(rc.ok());
}

TEST(rtx_format, the_parser_into_variant_rejects_a_small_scratch) {
    const std::vector<std::uint8_t> payload{0x00, 0x05, 0xAA, 0xBB};
    std::uint16_t osn = 0;
    nimrtc::core::ByteSpan body;
    std::vector<std::uint8_t> storage(1);   // needs 2
    const auto rc = nimrtc::rtx::parse_payload_into(
        const_span(payload), osn, body, mutable_span(storage));
    EXPECT_FALSE(rc.ok());
    EXPECT_EQ(rc.error().code(), nimrtc::core::ErrorCode::InvalidArgument);
}

TEST(rtx_format, the_parser_into_variant_restores_the_original) {
    const std::vector<std::uint8_t> payload{0x12, 0x34, 0xAA, 0xBB, 0xCC};
    std::uint16_t osn = 0;
    nimrtc::core::ByteSpan body;
    std::vector<std::uint8_t> storage(8);
    const auto rc = nimrtc::rtx::parse_payload_into(
        const_span(payload), osn, body, mutable_span(storage));
    ASSERT_TRUE(rc.ok());
    EXPECT_EQ(osn, 0x1234u);
    ASSERT_EQ(body.size(), 3u);
    EXPECT_EQ(body[0], 0xAAu);
    EXPECT_EQ(body[1], 0xBBu);
    EXPECT_EQ(body[2], 0xCCu);
}

// ---------------------------------------------------------------------------
// The SDP parameters (RFC 4588 §8.1)
// ---------------------------------------------------------------------------

TEST(rtx_sdp, the_apt_parameter_is_parsed) {
    EXPECT_EQ(nimrtc::rtx::parse_apt("apt=102"), 102u);
    EXPECT_EQ(nimrtc::rtx::parse_apt("apt=96"), 96u);
    EXPECT_EQ(nimrtc::rtx::parse_apt("apt=127"), 127u);
}

TEST(rtx_sdp, the_apt_parameter_is_found_among_other_parameters) {
    // RFC 4588 §8.1 allows `rtx-time` alongside `apt`, and the original
    // payload type's own parameters may ride along too.
    EXPECT_EQ(nimrtc::rtx::parse_apt("apt=102;rtx-time=200"), 102u);
    EXPECT_EQ(nimrtc::rtx::parse_apt("rtx-time=200;apt=102"), 102u);
    EXPECT_EQ(nimrtc::rtx::parse_apt("profile-level-id=42e01f;apt=102"), 102u);
}

TEST(rtx_sdp, the_apt_parameter_is_matched_case_insensitively) {
    EXPECT_EQ(nimrtc::rtx::parse_apt("APT=102"), 102u);
}

TEST(rtx_sdp, a_body_without_an_apt_parameter_yields_no_association) {
    EXPECT_FALSE(nimrtc::rtx::parse_apt("").has_value());
    EXPECT_FALSE(nimrtc::rtx::parse_apt("rtx-time=200").has_value());
    EXPECT_FALSE(nimrtc::rtx::parse_apt("packetization-mode=1").has_value());
}

TEST(rtx_sdp, a_malformed_apt_parameter_yields_no_association) {
    EXPECT_FALSE(nimrtc::rtx::parse_apt("apt=").has_value());
    EXPECT_FALSE(nimrtc::rtx::parse_apt("apt=abc").has_value());
    EXPECT_FALSE(nimrtc::rtx::parse_apt("apt=1024").has_value());   // > 127
}

TEST(rtx_sdp, the_rtx_time_parameter_is_parsed) {
    EXPECT_EQ(nimrtc::rtx::parse_rtx_time_ms("apt=102;rtx-time=200"), 200u);
    EXPECT_EQ(nimrtc::rtx::parse_rtx_time_ms("rtx-time=500"), 500u);
    EXPECT_FALSE(nimrtc::rtx::parse_rtx_time_ms("apt=102").has_value());
    EXPECT_FALSE(nimrtc::rtx::parse_rtx_time_ms("").has_value());
}

TEST(rtx_sdp, the_fmtp_body_is_built) {
    EXPECT_EQ(nimrtc::rtx::build_fmtp(102, std::nullopt), "apt=102");
    EXPECT_EQ(nimrtc::rtx::build_fmtp(102, 200u), "apt=102;rtx-time=200");
}

TEST(rtx_sdp, the_built_fmtp_body_parses_back) {
    const std::string body = nimrtc::rtx::build_fmtp(102, 200u);
    EXPECT_EQ(nimrtc::rtx::parse_apt(body), 102u);
    EXPECT_EQ(nimrtc::rtx::parse_rtx_time_ms(body), 200u);
}

// ---------------------------------------------------------------------------
// The encoding name and the clock rate
// ---------------------------------------------------------------------------

TEST(rtx_sdp, the_encoding_name_is_rtx) {
    EXPECT_STREQ(nimrtc::rtx::kEncodingName, "rtx");
}

TEST(rtx_sdp, the_video_clock_rate_is_90_khz) {
    // RFC 4588 §4: the retransmission stream's clock rate MUST equal
    // the associated original stream's clock rate.  Every video codec
    // NimRTC negotiates runs on 90 kHz.
    EXPECT_EQ(nimrtc::rtx::kVideoClockRateHz, 90000u);
}

} // anonymous namespace
