/**
 * @file tests/test_rtp_packetizer.cpp
 * @brief The engine's RTP packet builder (ADR-025 Decision 4).
 *
 * The engine's outbound RTP packets are built in exactly one place --
 * `src/engine/src/rtp_packetizer.cpp` -- so that the engine's exemption
 * from Layout Invariant 4 has a visible boundary, and so that a fix to
 * the RTP header lands in one place rather than in the audio path and
 * the video path separately.
 *
 * The collaborator is `detail::RtpPacketizer`, an engine-private class.
 * The test exercises it directly -- no engine, no transport, no clock --
 * because that is the shape it has: a pure function of a packet
 * description.
 *
 * ## What the test is really for
 *
 * The extraction moved the packet building out of four call sites
 * (audio, DTMF, the default video stream, the extra video streams) and
 * into one.  A refactor of that shape can go wrong in two ways, and
 * both are silent:
 *
 *   1. the produced packet differs from what the old code produced
 *      (a wrong header field, a missing extension), and
 *   2. the abs-send-time encoding differs from the RTP module's.
 *
 * So the test does not check the packetizer against itself.  It checks
 * it against **the RTP module's own builder** -- the same bytes, byte
 * for byte -- and against the RTP module's own abs-send-time encoder.
 * The module is the reference; the collaborator is the thing under
 * test.
 *
 * The test compiles the engine's *private* header directory into its
 * include path, exactly as `test_bandwidth_allocation` does.  That
 * directory is not installed and is not on the public surface
 * (constraint L4).
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <vector>

#include <nimrtc/rtp/packet.hpp>
#include <nimrtc/rtp/timeline.hpp>

#include "rtp_packetizer.hpp"

namespace {

using nimrtc::core::ByteSpan;
using nimrtc::engine::detail::RtpPacketSpec;
using nimrtc::engine::detail::RtpPacketizer;

/// Build a packet the way the *RTP module* would, for the same spec.
std::vector<std::uint8_t> reference_packet(const RtpPacketSpec& spec) {
    nimrtc::rtp::PacketBuilder pb;
    pb.set_ssrc(spec.ssrc)
      .set_seq(spec.seq)
      .set_timestamp(spec.timestamp)
      .set_payload_type(spec.payload_type)
      .set_marker(spec.marker);
    if (spec.abs_send_time_ext_id != 0) {
        pb.set_abs_send_time(nimrtc::rtp::AbsSendTime{spec.abs_send_time_24mhz},
                              spec.abs_send_time_ext_id);
    }
    pb.set_payload(spec.payload);
    return pb.build();
}

/// A spec with everything filled in.
RtpPacketSpec sample_spec() {
    RtpPacketSpec spec;
    spec.ssrc         = 0xCAFEBABEu;
    spec.seq          = 0x1234;
    spec.timestamp    = 0x0A0B0C0Du;
    spec.payload_type = 96;
    spec.marker       = true;
    // The payload lives in *static* storage on purpose.  `ByteSpan` is
    // a non-owning `std::span`, so a span over a function-local array
    // would dangle the moment `sample_spec()` returns — and the test
    // would then compare two packets built from *different* garbage.
    // The bytes are a compile-time constant, so static storage costs
    // nothing and the span stays valid for the whole test.
    static constexpr std::uint8_t kPayload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    spec.payload      = ByteSpan(kPayload, sizeof(kPayload));
    return spec;
}

// ---------------------------------------------------------------------------
// The packet bytes
// ---------------------------------------------------------------------------

// The collaborator produces byte-for-byte what the RTP module's builder
// produces.  This is the test that would catch a header field being
// dropped or transposed during the extraction.
TEST(RtpPacketizer, MatchesTheRtpModuleBuilderByteForByte) {
    const RtpPacketSpec spec = sample_spec();

    const std::vector<std::uint8_t> reference = reference_packet(spec);
    ASSERT_FALSE(reference.empty());

    std::vector<std::uint8_t> actual(reference.size(), 0);
    const std::size_t written =
        RtpPacketizer::build_into(spec, actual.data(), actual.size());

    ASSERT_EQ(written, reference.size());
    EXPECT_EQ(actual, reference);
}

// The same, with the abs-send-time extension present.  The extension is
// the fiddly part: its presence changes the header's X bit and adds an
// extension block, and the negotiated id decides the RFC 5285 one-byte
// form vs. the RFC 8285 two-byte form.
TEST(RtpPacketizer, MatchesTheRtpModuleBuilderWithTheExtension) {
    for (const std::uint16_t ext_id :
         {std::uint16_t{1}, std::uint16_t{4}, std::uint16_t{14}, std::uint16_t{15},
          std::uint16_t{255}}) {
        RtpPacketSpec spec = sample_spec();
        spec.abs_send_time_ext_id = static_cast<std::uint16_t>(ext_id);
        spec.abs_send_time_24mhz = 0x00ABCDEFu;

        const std::vector<std::uint8_t> reference = reference_packet(spec);
        ASSERT_FALSE(reference.empty());

        std::vector<std::uint8_t> actual(reference.size(), 0);
        const std::size_t written =
            RtpPacketizer::build_into(spec, actual.data(), actual.size());

        EXPECT_EQ(written, reference.size()) << "ext id " << ext_id;
        EXPECT_EQ(actual, reference) << "ext id " << ext_id;
    }
}

// The size the collaborator reports is the size the packet actually has.
TEST(RtpPacketizer, ComputeSizeMatchesTheBuiltPacket) {
    RtpPacketSpec spec = sample_spec();
    spec.abs_send_time_ext_id = 4;
    spec.abs_send_time_24mhz = 0x00001234;

    const std::size_t declared = RtpPacketizer::compute_size(spec);
    const std::vector<std::uint8_t> reference = reference_packet(spec);

    EXPECT_EQ(declared, reference.size());
}

// The size call must not disturb the packet that follows it: the
// call sites size the scratch buffer first and then build into it, so
// a builder whose state leaked between the two calls would produce a
// packet of the wrong length.
TEST(RtpPacketizer, ComputeSizeDoesNotDisturbTheNextPacket) {
    const RtpPacketSpec spec = sample_spec();
    const std::vector<std::uint8_t> reference = reference_packet(spec);

    // Size it twice, then build.  The build must still match.
    (void)RtpPacketizer::compute_size(spec);
    (void)RtpPacketizer::compute_size(spec);

    std::vector<std::uint8_t> actual(reference.size(), 0);
    const std::size_t written =
        RtpPacketizer::build_into(spec, actual.data(), actual.size());
    ASSERT_EQ(written, reference.size());
    EXPECT_EQ(actual, reference);
}

// A spec with no abs-send-time extension must not carry one: the
// extension is negotiated, and a packet that carries an un-negotiated
// extension is malformed on the wire.
TEST(RtpPacketizer, NoExtensionIdMeansNoExtensionOnTheWire) {
    const RtpPacketSpec spec = sample_spec();
    EXPECT_EQ(spec.abs_send_time_ext_id, 0u);

    const std::vector<std::uint8_t> reference = reference_packet(spec);
    std::vector<std::uint8_t> actual(reference.size(), 0);
    const std::size_t written =
        RtpPacketizer::build_into(spec, actual.data(), actual.size());
    ASSERT_EQ(written, reference.size());

    // Byte 0 of the RTP header: bit 4 is the extension bit (X).
    EXPECT_EQ(actual[0] & 0x10, 0u)
        << "the X bit must be clear when no extension is negotiated";
}

// A buffer that is too small must produce nothing, not a truncated
// packet.  A truncated RTP packet on the wire is a silent corruption;
// "wrote nothing" is a reportable failure.
TEST(RtpPacketizer, ATooSmallBufferProducesNothing) {
    const RtpPacketSpec spec = sample_spec();
    const std::size_t needed = RtpPacketizer::compute_size(spec);
    ASSERT_GT(needed, 1u);

    std::vector<std::uint8_t> buffer(needed - 1, 0);
    EXPECT_EQ(RtpPacketizer::build_into(spec, buffer.data(), buffer.size()),
              0u);
}

// A null destination with a zero capacity is the documented no-op.
TEST(RtpPacketizer, ANullDestinationWritesNothing) {
    const RtpPacketSpec spec = sample_spec();
    EXPECT_EQ(RtpPacketizer::build_into(spec, nullptr, 0), 0u);
}

// An empty payload is legal (a zero-length RTP payload), and the packet
// is then exactly the header.
TEST(RtpPacketizer, AnEmptyPayloadProducesAHeaderOnlyPacket) {
    RtpPacketSpec spec = sample_spec();
    spec.payload = ByteSpan();

    const std::vector<std::uint8_t> reference = reference_packet(spec);
    std::vector<std::uint8_t> actual(reference.size(), 0);
    const std::size_t written =
        RtpPacketizer::build_into(spec, actual.data(), actual.size());

    ASSERT_EQ(written, reference.size());
    EXPECT_EQ(actual, reference);
}

// ---------------------------------------------------------------------------
// The abs-send-time encoding
// ---------------------------------------------------------------------------

// The collaborator's encoder produces the same value as the RTP
// module's encoder, for the same origin.
//
// The two take their origin in different shapes -- the module's takes
// an NTP64, the collaborator's takes the Unix-microsecond origin the
// engine already keeps (`TimelineState::boot_epoch_offset_us`) -- so
// the test converts once and compares the *values*.
TEST(RtpPacketizer, TheAbsSendTimeEncodingMatchesTheRtpModule) {
    // A round Unix-microsecond origin: 2024-01-01T00:00:00Z.
    constexpr std::int64_t kOriginUnixUs = 1'704'067'200LL * 1'000'000LL;

    // The module's origin, built from the same instant.  The NTP64's
    // fraction is 2^-32 s, so the round trip is not exact -- the test
    // allows the one-microsecond band that the conversion can lose.
    const nimrtc::rtp::Ntp64 module_origin = nimrtc::rtp::ntp64_from(
        std::chrono::system_clock::time_point{
            std::chrono::microseconds{kOriginUnixUs}});
    const std::int64_t module_origin_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            nimrtc::rtp::system_clock_from_ntp64(module_origin)
                .time_since_epoch())
            .count();
    ASSERT_LE(std::llabs(module_origin_us - kOriginUnixUs), 1);

    for (const std::int64_t monotonic_us :
         {0LL, 1LL, 999LL, 1'000'000LL, 12'345'678LL,
          255'000'000LL - 16LL, 256LL * 1'000'000LL}) {
        const std::uint32_t ours = RtpPacketizer::encode_abs_send_time_24(
            std::chrono::microseconds{monotonic_us}, kOriginUnixUs);
        const std::uint32_t theirs = nimrtc::rtp::encode_abs_send_time_24(
            std::chrono::microseconds{monotonic_us}, module_origin);

        EXPECT_EQ(ours, theirs) << "monotonic offset " << monotonic_us << " us";
    }
}

// The 24-bit field wraps every 256 s (2^24 / 65536 Hz).
TEST(RtpPacketizer, TheAbsSendTimeWrapsEvery256Seconds) {
    constexpr std::int64_t kOriginUnixUs = 1'704'067'200LL * 1'000'000LL;

    const std::uint32_t at_zero = RtpPacketizer::encode_abs_send_time_24(
        std::chrono::microseconds{0}, kOriginUnixUs);
    const std::uint32_t at_256s = RtpPacketizer::encode_abs_send_time_24(
        std::chrono::microseconds{256LL * 1'000'000LL}, kOriginUnixUs);

    EXPECT_EQ(at_zero, at_256s);
    EXPECT_NE(at_zero, RtpPacketizer::encode_abs_send_time_24(
                           std::chrono::microseconds{256LL * 1'000'000LL - 16},
                           kOriginUnixUs));
}

// A send instant *before* the origin cannot be expressed in the 24-bit
// field, so it clamps to the wrap epoch rather than wrapping around to
// a huge value.
//
// The origin has to be zero for the clamp to be reachable at all: a
// negative offset from a *later* origin is still after the Unix epoch,
// and is therefore a perfectly representable instant.
TEST(RtpPacketizer, AnInstantBeforeTheOriginClampsToZero) {
    EXPECT_EQ(RtpPacketizer::encode_abs_send_time_24(
                  std::chrono::microseconds{-1}, 0),
              0u);

    // And the RTP module agrees.
    EXPECT_EQ(nimrtc::rtp::encode_abs_send_time_24(
                  std::chrono::microseconds{-1}, nimrtc::rtp::Ntp64{0}),
              0u);
}

// An instant *after* the origin is representable and is not clamped.
TEST(RtpPacketizer, AnInstantAfterTheOriginIsNotClamped) {
    constexpr std::int64_t kOriginUnixUs = 1'000'000LL;
    EXPECT_NE(RtpPacketizer::encode_abs_send_time_24(
                  std::chrono::microseconds{0}, kOriginUnixUs),
              0u);
}

}  // namespace
