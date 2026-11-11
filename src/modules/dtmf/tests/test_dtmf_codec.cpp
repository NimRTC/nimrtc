/**
 * @file src/modules/dtmf/tests/test_dtmf_codec.cpp
 * @brief The RFC 4733 (telephone-event) wire format.
 *
 * RFC 4733 §2.3 (Figure 1) fixes the layout of the four-byte payload:
 *
 *     0                   1                   2                   3
 *     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |     event     |E|R|   volume  |          duration             |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * Two details of that figure are the reason this file exists:
 *
 *   - the **E bit is the most significant bit of byte 1** (mask 0x80),
 *     not the least significant one.  A codec that puts it in bit 0
 *     interoperates with nobody: the peer never sees the end of the
 *     event, and the key press is reported as an unterminated one;
 *   - the **duration is in timestamp units**, and the telephone-event
 *     clock is 8000 Hz (RFC 4733 §2.4) — so one unit is 0.125 ms.  A
 *     codec that reads the field as milliseconds makes every key press
 *     eight times too long.
 *
 * The tests below pin both, plus the malformed-input handling.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include <nimrtc/dtmf/dtmf.hpp>
#include <nimrtc/plugins/dtmf.hpp>

namespace {

using nimrtc::dtmf::TelephoneEventCodec;
using nimrtc::plugins::DtmfEvent;

/** The four bytes of a telephone-event payload, as the codec produced
 *  or consumed them. */
struct Payload {
    std::uint8_t event    = 0;
    std::uint8_t byte1    = 0;
    std::uint8_t duration_hi = 0;
    std::uint8_t duration_lo = 0;

    std::string bytes() const {
        std::string out;
        out.push_back(static_cast<char>(event));
        out.push_back(static_cast<char>(byte1));
        out.push_back(static_cast<char>(duration_hi));
        out.push_back(static_cast<char>(duration_lo));
        return out;
    }
};

Payload payload_of(const std::string& s) {
    Payload p;
    p.event       = static_cast<std::uint8_t>(s[0]);
    p.byte1       = static_cast<std::uint8_t>(s[1]);
    p.duration_hi = static_cast<std::uint8_t>(s[2]);
    p.duration_lo = static_cast<std::uint8_t>(s[3]);
    return p;
}

// ---------------------------------------------------------------------------
// The end-of-event bit
// ---------------------------------------------------------------------------

TEST(DtmfCodec, TheEndOfEventBitIsTheMostSignificantBitOfByteOne) {
    TelephoneEventCodec codec;

    DtmfEvent ev;
    ev.code        = 5;
    ev.duration_ms = 100;
    ev.end         = true;

    const std::string encoded = codec.encode(ev);
    ASSERT_EQ(encoded.size(), 4u);
    const auto p = payload_of(encoded);

    // RFC 4733 §2.3.2: the E bit is bit 7 of byte 1.
    EXPECT_NE(p.byte1 & 0x80, 0u) << "the E bit must be set in 0x80";

    // ... and it must *not* be in the low bit: the low six bits carry
    // the volume.  This is the regression guard for the layout that
    // put the E bit in bit 0.
    EXPECT_EQ(p.byte1 & 0x01, 0u)
        << "the end-of-event bit must not leak into the volume field";
}

TEST(DtmfCodec, AnEventThatIsNotEndedCarriesNoEndOfEventBit) {
    TelephoneEventCodec codec;

    DtmfEvent ev;
    ev.code        = 5;
    ev.duration_ms = 100;
    ev.end         = false;

    const auto p = payload_of(codec.encode(ev));
    EXPECT_EQ(p.byte1 & 0x80, 0u)
        << "an event in progress must not carry the E bit";
}

TEST(DtmfCodec, TheReservedBitIsNeverSet) {
    TelephoneEventCodec codec;

    DtmfEvent ev;
    ev.code        = 1;
    ev.duration_ms = 50;
    ev.end         = true;

    const auto p = payload_of(codec.encode(ev));
    EXPECT_EQ(p.byte1 & 0x40, 0u)
        << "RFC 4733 §2.3.3: the reserved bit must be sent as zero";
}

TEST(DtmfCodec, TheVolumeOccupiesTheSixLowBitsOfByteOne) {
    TelephoneEventCodec codec;

    DtmfEvent ev;
    ev.code        = 1;
    ev.duration_ms = 50;
    ev.end         = true;

    const auto p = payload_of(codec.encode(ev));
    // The default volume is 10 (RFC 4733 §2.3.1).
    EXPECT_EQ(p.byte1 & 0x3F, TelephoneEventCodec::kDefaultVolume);
    EXPECT_EQ(p.byte1 & 0xC0, 0x80u)
        << "only the E bit may sit above the volume field";
}

// ---------------------------------------------------------------------------
// The duration unit
// ---------------------------------------------------------------------------

TEST(DtmfCodec, TheDurationIsEncodedInTimestampUnits) {
    TelephoneEventCodec codec;

    DtmfEvent ev;
    ev.code        = 9;
    ev.duration_ms = 100;
    ev.end         = true;

    const auto p = payload_of(codec.encode(ev));

    // RFC 4733 §2.3.5: the duration is in timestamp units, and the
    // telephone-event clock is 8000 Hz (RFC 4733 §2.4) — 100 ms is
    // therefore 800 units, not 100.
    const std::uint16_t units =
        static_cast<std::uint16_t>((p.duration_hi << 8) | p.duration_lo);
    EXPECT_EQ(units, 800u)
        << "100 ms at 8000 Hz is 800 timestamp units";
    EXPECT_EQ(p.duration_hi, 0x03u);
    EXPECT_EQ(p.duration_lo, 0x20u);
}

TEST(DtmfCodec, TheDurationIsDecodedIntoMilliseconds) {
    TelephoneEventCodec codec;

    // 800 timestamp units at 8000 Hz = 100 ms.
    DtmfEvent ev;
    std::string raw;
    raw.push_back(static_cast<char>(9));       // event code
    raw.push_back(static_cast<char>(0x80));    // E bit
    raw.push_back(static_cast<char>(0x03));    // duration high
    raw.push_back(static_cast<char>(0x20));    // duration low

    DtmfEvent out;
    ASSERT_TRUE(codec.decode(raw, out));
    EXPECT_EQ(out.code, 9u);
    EXPECT_EQ(out.duration_ms, 100u)
        << "800 timestamp units are 100 ms, not 800 ms";
    EXPECT_TRUE(out.end);
}

TEST(DtmfCodec, AKeyPressIsNotEightTimesTooLong) {
    TelephoneEventCodec codec;

    // The regression this guards: a codec that treats the duration
    // field as milliseconds reports a 100 ms key press as 800 ms.
    DtmfEvent ev;
    ev.code        = 3;
    ev.duration_ms = 100;
    ev.end         = true;

    DtmfEvent out;
    ASSERT_TRUE(codec.decode(codec.encode(ev), out));
    EXPECT_EQ(out.duration_ms, 100u);
}

// ---------------------------------------------------------------------------
// The round trip
// ---------------------------------------------------------------------------

TEST(DtmfCodec, AnEventSurvivesTheRoundTrip) {
    TelephoneEventCodec codec;

    DtmfEvent ev;
    ev.code        = 11;      // '#'
    ev.duration_ms = 250;
    ev.end         = true;

    DtmfEvent out;
    ASSERT_TRUE(codec.decode(codec.encode(ev), out));
    EXPECT_EQ(out.code, ev.code);
    EXPECT_EQ(out.duration_ms, ev.duration_ms);
    EXPECT_TRUE(out.end);
}

TEST(DtmfCodec, EveryEventCodeTheRfcDefinesIsAccepted) {
    TelephoneEventCodec codec;

    for (std::uint8_t code = 0; code <= TelephoneEventCodec::kMaxEventCode;
         ++code) {
        DtmfEvent ev;
        ev.code        = code;
        ev.duration_ms = 20;
        ev.end         = true;

        DtmfEvent out;
        ASSERT_TRUE(codec.decode(codec.encode(ev), out))
            << "event code " << static_cast<int>(code) << " must be accepted";
        EXPECT_EQ(out.code, code);
    }
}

TEST(DtmfCodec, AnEventCodeBeyondTheRfcIsRejected) {
    TelephoneEventCodec codec;

    DtmfEvent ev;
    ev.code        = TelephoneEventCodec::kMaxEventCode + 1;
    ev.duration_ms = 20;
    ev.end         = true;

    EXPECT_TRUE(codec.encode(ev).empty())
        << "an unknown event code must not be encoded";
}

// ---------------------------------------------------------------------------
// Malformed input
// ---------------------------------------------------------------------------

TEST(DtmfCodec, APayloadOfTheWrongLengthIsRejected) {
    TelephoneEventCodec codec;

    DtmfEvent out;
    EXPECT_FALSE(codec.decode("", out));
    EXPECT_FALSE(codec.decode("ab", out));
    EXPECT_FALSE(codec.decode("abcd", out));
    EXPECT_FALSE(codec.decode("abcde", out));
}

TEST(DtmfCodec, AnUnknownEventCodeOnTheWireIsRejected) {
    TelephoneEventCodec codec;

    std::string raw;
    raw.push_back(static_cast<char>(17));      // one past the RFC's range
    raw.push_back(static_cast<char>(0x80));
    raw.push_back(static_cast<char>(0x00));
    raw.push_back(static_cast<char>(0x00));

    DtmfEvent out;
    EXPECT_FALSE(codec.decode(raw, out));
}

TEST(DtmfCodec, TheReservedBitIsIgnoredOnReceive) {
    TelephoneEventCodec codec;

    // RFC 4733 §2.3.3: the receiver ignores the reserved bit.
    std::string raw;
    raw.push_back(static_cast<char>(2));
    raw.push_back(static_cast<char>(0xC0));     // E bit *and* the reserved bit
    raw.push_back(static_cast<char>(0x00));
    raw.push_back(static_cast<char>(0x64));     // 100 units = 12.5 ms

    DtmfEvent out;
    ASSERT_TRUE(codec.decode(raw, out));
    EXPECT_TRUE(out.end);
    EXPECT_EQ(out.code, 2u);
}

// ---------------------------------------------------------------------------
// The plugin surface
// ---------------------------------------------------------------------------

TEST(DtmfCodec, TheCodecIsRegisteredUnderTheDtmfId) {
    TelephoneEventCodec codec;
    EXPECT_STREQ(codec.name(), "NimRTC-DTMF-RFC4733");
    EXPECT_EQ(codec.payload_type(), TelephoneEventCodec::kDefaultPayloadType);
    EXPECT_EQ(codec.payload_type(), 126u)
        << "126 is the payload type Chrome uses";
}

} // namespace
