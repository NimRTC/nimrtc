/**
 * @file tests/test_net_feedback.cpp
 * @brief Tests for the receiver-side network-feedback collector (F2 / F4 / F7).
 *
 * Covers:
 *   C1  - a Receiver Report produces a loss rate
 *   C2  - the cumulative loss is carried through
 *   C3  - the RTT comes from the LSR / DLSR pair against our own SR
 *   C4  - a report whose LSR does not match our SR yields no RTT
 *   C5  - the jitter is converted with the stream's clock rate
 *   C6  - a REMB becomes the snapshot's REMB value
 *   C7  - a transport-cc feedback yields a non-empty arrival series (F4)
 *   C8  - the transport-cc receive/loss counts are aggregated
 *   C9  - the source discriminator follows the negotiated policy
 *   C10 - an unknown payload type is ignored
 *   C11 - reset() clears the snapshot
 *
 *   P1  - the automatic default: transport-cc drives only when the peer
 *         advertised it (ADR-018 Decision 2)
 *   P2  - a stale transport-cc path falls back to the loss/RTT path
 *   P3  - the loss/RTT override ignores transport-cc entirely
 *   P4  - the "require transport-cc" policy fails loudly
 *   P5  - the policy configuration round-trips
 *
 * The wire formats are the ones the `rtp` module owns: the Receiver
 * Report of RFC 3550 §6.4.2, the REMB of draft-alvestrand-rmcat-remb-03
 * and the transport-cc of draft-holmer-rmcat-transport-wide-cc-extensions-01.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/net_feedback/net_feedback.hpp>

using nimrtc::core::ByteBuffer;
using nimrtc::core::ByteSpan;
using nimrtc::core::TimePoint;

namespace {

// ---------------------------------------------------------------------------
// Wire-format helpers
// ---------------------------------------------------------------------------

void put_be32(ByteBuffer& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void put_be24(ByteBuffer& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
}

void put_be16(ByteBuffer& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void put_u8(ByteBuffer& out, std::uint8_t v) { out.push_back(v); }

/// One report block of a Receiver Report (RFC 3550 §6.4.3).
struct ReportBlockFields {
    std::uint32_t ssrc = 0;
    std::uint8_t  fraction_lost = 0;
    std::int32_t  cumulative_lost = 0;
    std::uint32_t highest_seq = 0;
    std::uint32_t jitter = 0;
    std::uint32_t lsr = 0;
    std::uint32_t dlsr = 0;
};

/// Build a Receiver Report (PT 201) with one report block.
ByteBuffer make_rr(const ReportBlockFields& rb) {
    ByteBuffer body;
    put_be32(body, 0xDEADBEEFu);        // SSRC of the report sender
    put_be32(body, rb.ssrc);
    put_u8(body, rb.fraction_lost);
    put_be24(body, static_cast<std::uint32_t>(rb.cumulative_lost) & 0xFFFFFFu);
    put_be32(body, rb.highest_seq);
    put_be32(body, rb.jitter);
    put_be32(body, rb.lsr);
    put_be32(body, rb.dlsr);

    ByteBuffer out;
    put_u8(out, 0x81u);                  // V=2, RC=1
    put_u8(out, 201u);
    put_be16(out, static_cast<std::uint16_t>(out.size() / 4u + body.size() / 4u));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

/// Build a REMB message (PSFB, FMT 15).
ByteBuffer make_remb(std::uint64_t bitrate_bps) {
    constexpr std::uint32_t kMaxMantissa = 0x3FFFF;
    std::uint64_t mantissa = bitrate_bps;
    std::uint8_t exponent = 0;
    while (mantissa > kMaxMantissa) {
        mantissa >>= 1;
        ++exponent;
    }

    ByteBuffer body;
    put_be32(body, 0x11223344u);   // sender SSRC
    put_be32(body, 0);             // media SSRC
    put_be32(body, 0x52454D42u);   // 'R' 'E' 'M' 'B'
    put_u8(body, 0);              // no SSRC list
    put_u8(body, static_cast<std::uint8_t>((exponent << 2) | (mantissa >> 16)));
    put_be16(body, static_cast<std::uint16_t>(mantissa & 0xFFFF));

    ByteBuffer out;
    put_u8(out, static_cast<std::uint8_t>(0x80u | 15u));
    put_u8(out, 206u);
    put_be16(out, static_cast<std::uint16_t>(out.size() / 4u + body.size() / 4u));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

/// Build a transport-cc feedback (RTPFB, FMT 15) with one run-length chunk
/// of "received with a small delta" packets.
///
/// The message is padded to a 32-bit word and the padding bit is set in
/// the header, exactly as a real transport-cc feedback is: without it the
/// RTCP `length` field would not describe the whole datagram and the
/// parser would (correctly) refuse the trailing receive deltas.
ByteBuffer make_transport_cc(std::uint16_t base_seq, std::uint16_t count,
                             const std::vector<std::uint8_t>& deltas) {
    ByteBuffer body;
    // Run length chunk: 0 | symbol (2 bit) | run length (13 bit).
    put_be16(body, static_cast<std::uint16_t>(
                       (static_cast<std::uint16_t>(1u & 0x3u) << 13) | (count & 0x1FFFu)));
    for (std::uint8_t d : deltas) {
        put_u8(body, d);
    }

    // 4 B RTCP header + 16 B feedback header + body, padded to a 32-bit word.
    const std::size_t total = 20u + body.size();
    const std::size_t pad = (4u - (total % 4u)) % 4u;
    const std::size_t padded = total + pad;

    ByteBuffer out(padded, 0);
    out[0] = static_cast<std::uint8_t>(0x80u | (pad ? 0x20u : 0x00u) | 15u);
    out[1] = 205u;
    const std::uint16_t length_words = static_cast<std::uint16_t>(padded / 4u - 1u);
    out[2] = static_cast<std::uint8_t>(length_words >> 8);
    out[3] = static_cast<std::uint8_t>(length_words & 0xFFu);
    // sender SSRC / media SSRC
    out[4] = 0x01; out[5] = 0x02; out[6] = 0x03; out[7] = 0x04;
    out[8] = 0x05; out[9] = 0x06; out[10] = 0x07; out[11] = 0x08;
    // base sequence number / packet status count
    out[12] = static_cast<std::uint8_t>(base_seq >> 8);
    out[13] = static_cast<std::uint8_t>(base_seq & 0xFFu);
    out[14] = static_cast<std::uint8_t>(count >> 8);
    out[15] = static_cast<std::uint8_t>(count & 0xFFu);
    // reference time (24 bit) / feedback packet count
    out[16] = 0; out[17] = 0; out[18] = 0; out[19] = 0;
    for (std::size_t i = 0; i < body.size(); ++i) {
        out[20u + i] = body[i];
    }
    if (pad) {
        // The last padding octet carries the count of padding octets.
        out[padded - 1u] = static_cast<std::uint8_t>(pad);
    }
    return out;
}

/// A monotonic instant `ms` milliseconds after the epoch.
TimePoint at_ms(std::int64_t ms) {
    return TimePoint{} + std::chrono::milliseconds{ms};
}

} // namespace

// ---------------------------------------------------------------------------
// Receiver Report
// ---------------------------------------------------------------------------

TEST(NetFeedback, ReceiverReportProducesALossRate) {
    nimrtc::net_feedback::Collector collector;
    ASSERT_EQ(collector.open(), nimrtc::plugins::kOk);

    ByteBuffer rr = make_rr({.fraction_lost = 64});   // 64 / 256 = 25 %
    collector.on_rtcp_block(201, 1, ByteSpan{rr.data(), rr.size()}, at_ms(1000));

    const auto snap = collector.snapshot();
    EXPECT_TRUE(snap.has_loss_rtt);
    EXPECT_DOUBLE_EQ(snap.loss_rate, 0.25);
    EXPECT_EQ(snap.source, nimrtc::plugins::NetFeedbackSource::kLossRtt);
}

TEST(NetFeedback, ReceiverReportCarriesTheCumulativeLoss) {
    nimrtc::net_feedback::Collector collector;
    collector.open();

    ByteBuffer rr = make_rr({.cumulative_lost = 1234});
    collector.on_rtcp_block(201, 1, ByteSpan{rr.data(), rr.size()}, at_ms(1000));

    EXPECT_EQ(collector.snapshot().cumulative_packets_lost, 1234u);
}

TEST(NetFeedback, RttComesFromTheLsrDlsrPair) {
    nimrtc::net_feedback::Collector collector;
    collector.open();

    // We sent a Sender Report whose NTP middle-32 is 0xAABBCCDD at t=0.
    collector.on_local_sender_report(0xAABBCCDDu, at_ms(0));

    // The peer received it after 20 ms and answered 100 ms later, so the
    // round trip is 100 ms - 20 ms = 80 ms.
    // DLSR is in 1/65536 s units: 20 ms = 20 * 65536 / 1000 = 1310.
    ByteBuffer rr = make_rr({.lsr = 0xAABBCCDDu, .dlsr = 1310});
    collector.on_rtcp_block(201, 1, ByteSpan{rr.data(), rr.size()}, at_ms(100));

    const auto snap = collector.snapshot();
    ASSERT_TRUE(snap.has_loss_rtt);
    EXPECT_EQ(snap.rtt.count(), 80);
}

TEST(NetFeedback, AMismatchedLsrYieldsNoRtt) {
    nimrtc::net_feedback::Collector collector;
    collector.open();
    collector.on_local_sender_report(0xAABBCCDDu, at_ms(0));

    // The report refers to a Sender Report we did not send (or one that
    // has already been superseded).
    ByteBuffer rr = make_rr({.lsr = 0x01020304u, .dlsr = 1310});
    collector.on_rtcp_block(201, 1, ByteSpan{rr.data(), rr.size()}, at_ms(100));

    EXPECT_EQ(collector.snapshot().rtt.count(), 0);
}

TEST(NetFeedback, TheJitterUsesTheStreamClockRate) {
    nimrtc::net_feedback::Collector collector;
    collector.open();
    collector.set_stream_clock_rate(90000);

    // 900 ticks at 90 kHz = 10 ms.
    ByteBuffer rr = make_rr({.jitter = 900});
    collector.on_rtcp_block(201, 1, ByteSpan{rr.data(), rr.size()}, at_ms(1000));

    EXPECT_DOUBLE_EQ(collector.snapshot().jitter_ms, 10.0);
}

// ---------------------------------------------------------------------------
// REMB
// ---------------------------------------------------------------------------

TEST(NetFeedback, RembBecomesTheSnapshotValue) {
    nimrtc::net_feedback::Collector collector;
    collector.open();

    ByteBuffer remb = make_remb(500'000);
    collector.on_rtcp_block(206, 15, ByteSpan{remb.data(), remb.size()}, at_ms(1000));

    const auto snap = collector.snapshot();
    ASSERT_TRUE(snap.remb_bps.has_value());
    EXPECT_EQ(*snap.remb_bps, 500'000u);
    EXPECT_EQ(snap.source, nimrtc::plugins::NetFeedbackSource::kRemb);
}

// ---------------------------------------------------------------------------
// transport-cc
// ---------------------------------------------------------------------------

TEST(NetFeedback, TransportCcYieldsAnArrivalSeries) {
    nimrtc::net_feedback::Collector collector;
    collector.open();

    // Three received packets with 8-bit deltas of 4, 8 and 12 ticks
    // (= 1, 2 and 3 milliseconds).
    ByteBuffer cc = make_transport_cc(1000, 3, {4, 8, 12});
    collector.on_rtcp_block(205, 15, ByteSpan{cc.data(), cc.size()}, at_ms(1000));

    const auto snap = collector.snapshot();
    EXPECT_TRUE(snap.has_transport_cc);
    ASSERT_EQ(snap.tc_arrival_deltas_ms.size(), 3u);
    EXPECT_DOUBLE_EQ(snap.tc_arrival_deltas_ms[0], 1.0);
    EXPECT_DOUBLE_EQ(snap.tc_arrival_deltas_ms[1], 2.0);
    EXPECT_DOUBLE_EQ(snap.tc_arrival_deltas_ms[2], 3.0);
    EXPECT_EQ(snap.tc_base_seq, 1000);
    EXPECT_EQ(snap.tc_packet_count, 3);
    // 3 packets spread over 6 ms -> 500 packets per second.
    EXPECT_DOUBLE_EQ(snap.tc_receive_rate_pps, 500.0);
}

TEST(NetFeedback, TransportCcAggregatesTheLostPackets) {
    nimrtc::net_feedback::Collector collector;
    collector.open();

    // A run of four "not received" packets (symbol 0).  Symbol 0 carries
    // no receive delta, so the message ends after the chunk -- padded to
    // a 32-bit word, as the wire format requires.
    ByteBuffer body;
    put_be16(body, static_cast<std::uint16_t>(4u & 0x1FFFu));

    const std::size_t total = 20u + body.size();
    const std::size_t pad = (4u - (total % 4u)) % 4u;
    const std::size_t padded = total + pad;

    ByteBuffer cc(padded, 0);
    cc[0] = static_cast<std::uint8_t>(0x80u | (pad ? 0x20u : 0x00u) | 15u);
    cc[1] = 205u;
    const std::uint16_t length_words = static_cast<std::uint16_t>(padded / 4u - 1u);
    cc[2] = static_cast<std::uint8_t>(length_words >> 8);
    cc[3] = static_cast<std::uint8_t>(length_words & 0xFFu);
    cc[12] = 0x01; cc[13] = 0x00;    // base sequence 256
    cc[14] = 0x00; cc[15] = 0x04;    // four packets
    cc[20] = static_cast<std::uint8_t>(body[0] >> 8);
    cc[21] = static_cast<std::uint8_t>(body[1] & 0xFFu);
    if (pad) {
        cc[padded - 1u] = static_cast<std::uint8_t>(pad);
    }

    collector.on_rtcp_block(205, 15, ByteSpan{cc.data(), cc.size()}, at_ms(1000));

    const auto snap = collector.snapshot();
    EXPECT_TRUE(snap.has_transport_cc);
    EXPECT_EQ(snap.tc_packets_lost, 4u);
    EXPECT_EQ(snap.tc_packets_received, 0u);
}

TEST(NetFeedback, TransportCcDrivesOnlyWhenThePeerNegotiatedIt) {
    nimrtc::net_feedback::Collector collector;
    collector.open();

    ByteBuffer rr = make_rr({.fraction_lost = 10});
    collector.on_rtcp_block(201, 1, ByteSpan{rr.data(), rr.size()}, at_ms(1000));
    EXPECT_EQ(collector.snapshot().source, nimrtc::plugins::NetFeedbackSource::kLossRtt);

    // The peer sends transport-cc, but it never advertised the
    // capability — so the feedback is not authoritative and the
    // loss/RTT path keeps driving (ADR-018 Decision 2).
    ByteBuffer cc = make_transport_cc(1, 1, {4});
    collector.on_rtcp_block(205, 15, ByteSpan{cc.data(), cc.size()}, at_ms(1100));
    EXPECT_EQ(collector.snapshot().source, nimrtc::plugins::NetFeedbackSource::kLossRtt);

    // Once the negotiation says the peer supports transport-cc, the
    // transport-cc path takes over.
    collector.set_peer_capabilities({.transport_cc = true});
    EXPECT_EQ(collector.snapshot().source, nimrtc::plugins::NetFeedbackSource::kTransportCc);
}

// ---------------------------------------------------------------------------
// The signal-selection policy (ADR-018 Decision 2)
// ---------------------------------------------------------------------------

TEST(NetFeedback, TheAutomaticDefaultFollowsTheNegotiation) {
    nimrtc::net_feedback::Collector collector;
    collector.open();

    // The peer advertised transport-cc and sends it: transport-cc drives.
    collector.set_peer_capabilities({.transport_cc = true});
    ByteBuffer cc = make_transport_cc(1, 1, {4});
    collector.on_rtcp_block(205, 15, ByteSpan{cc.data(), cc.size()}, at_ms(1000));
    EXPECT_EQ(collector.snapshot().source, nimrtc::plugins::NetFeedbackSource::kTransportCc);
}

TEST(NetFeedback, AStaleTransportCcPathFallsBackToLossRtt) {
    nimrtc::net_feedback::Collector collector;
    collector.open();
    collector.set_peer_capabilities({.transport_cc = true});

    // The timeout is 1 s.
    nimrtc::plugins::NetFeedbackConfig cfg;
    cfg.policy = nimrtc::plugins::NetFeedbackPolicy::kAuto;
    cfg.transport_cc_timeout = std::chrono::milliseconds{1000};
    collector.set_config(cfg);

    ByteBuffer cc = make_transport_cc(1, 1, {4});
    collector.on_rtcp_block(205, 15, ByteSpan{cc.data(), cc.size()}, at_ms(1000));
    EXPECT_EQ(collector.snapshot().source, nimrtc::plugins::NetFeedbackSource::kTransportCc);

    // A Receiver Report arrives 200 ms later — the transport-cc path is
    // still fresh, so it keeps driving.
    ByteBuffer rr = make_rr({.fraction_lost = 4});
    collector.on_rtcp_block(201, 1, ByteSpan{rr.data(), rr.size()}, at_ms(1200));
    EXPECT_EQ(collector.snapshot().source, nimrtc::plugins::NetFeedbackSource::kTransportCc);

    // Two seconds later the transport-cc feedback is stale: the policy
    // falls back to the loss/RTT path.  `evaluate()` is what detects it
    // — the collector has no clock of its own.
    collector.evaluate(at_ms(3000));
    EXPECT_EQ(collector.snapshot().source, nimrtc::plugins::NetFeedbackSource::kLossRtt);
}

TEST(NetFeedback, TheLossRttOverrideIgnoresTransportCc) {
    nimrtc::net_feedback::Collector collector;
    collector.open();
    collector.set_peer_capabilities({.transport_cc = true});

    nimrtc::plugins::NetFeedbackConfig cfg;
    cfg.policy = nimrtc::plugins::NetFeedbackPolicy::kLossRtt;
    collector.set_config(cfg);

    ByteBuffer cc = make_transport_cc(1, 1, {4});
    collector.on_rtcp_block(205, 15, ByteSpan{cc.data(), cc.size()}, at_ms(1000));
    EXPECT_EQ(collector.snapshot().source, nimrtc::plugins::NetFeedbackSource::kLossRtt);
    EXPECT_FALSE(collector.transport_cc_unavailable());
}

TEST(NetFeedback, TheRequiredTransportCcPolicyFailsLoudly) {
    nimrtc::net_feedback::Collector collector;
    collector.open();

    nimrtc::plugins::NetFeedbackConfig cfg;
    cfg.policy = nimrtc::plugins::NetFeedbackPolicy::kRequireTransportCc;
    collector.set_config(cfg);

    // The peer never advertised transport-cc: the path is reported as
    // unavailable instead of being papered over.
    EXPECT_TRUE(collector.transport_cc_unavailable());

    // The loss/RTT path still drives — a starved estimator would be a
    // worse failure than a degraded one.
    ByteBuffer rr = make_rr({.fraction_lost = 4});
    collector.on_rtcp_block(201, 1, ByteSpan{rr.data(), rr.size()}, at_ms(1000));
    EXPECT_EQ(collector.snapshot().source, nimrtc::plugins::NetFeedbackSource::kLossRtt);
    EXPECT_TRUE(collector.transport_cc_unavailable());

    // With the capability negotiated and the feedback arriving, the
    // path is available and drives the estimate.
    collector.set_peer_capabilities({.transport_cc = true});
    ByteBuffer cc = make_transport_cc(1, 1, {4});
    collector.on_rtcp_block(205, 15, ByteSpan{cc.data(), cc.size()}, at_ms(1100));
    EXPECT_EQ(collector.snapshot().source, nimrtc::plugins::NetFeedbackSource::kTransportCc);
    EXPECT_FALSE(collector.transport_cc_unavailable());
}

TEST(NetFeedback, ThePolicyConfigurationRoundTrips) {
    nimrtc::net_feedback::Collector collector;
    collector.open();

    nimrtc::plugins::NetFeedbackConfig cfg;
    cfg.policy = nimrtc::plugins::NetFeedbackPolicy::kRequireTransportCc;
    cfg.transport_cc_timeout = std::chrono::milliseconds{1500};
    collector.set_config(cfg);

    EXPECT_EQ(collector.config().policy,
              nimrtc::plugins::NetFeedbackPolicy::kRequireTransportCc);
    EXPECT_EQ(collector.config().transport_cc_timeout.count(), 1500);

    nimrtc::plugins::NetFeedbackCapabilities caps{.transport_cc = true};
    collector.set_peer_capabilities(caps);
    EXPECT_TRUE(collector.peer_capabilities().transport_cc);
    EXPECT_FALSE(collector.peer_capabilities().remb);
}

// ---------------------------------------------------------------------------
// Robustness
// ---------------------------------------------------------------------------

TEST(NetFeedback, AnUnknownPayloadTypeIsIgnored) {
    nimrtc::net_feedback::Collector collector;
    collector.open();

    ByteBuffer sr;
    put_u8(sr, 0x80u);
    put_u8(sr, 200u);
    put_be16(sr, 1);
    put_be32(sr, 0);

    collector.on_rtcp_block(200, 0, ByteSpan{sr.data(), sr.size()}, at_ms(1000));

    const auto snap = collector.snapshot();
    EXPECT_FALSE(snap.has_loss_rtt);
    EXPECT_FALSE(snap.has_transport_cc);
    EXPECT_FALSE(snap.remb_bps.has_value());
}

TEST(NetFeedback, ResetClearsTheSnapshot) {
    nimrtc::net_feedback::Collector collector;
    collector.open();

    ByteBuffer rr = make_rr({.fraction_lost = 64});
    collector.on_rtcp_block(201, 1, ByteSpan{rr.data(), rr.size()}, at_ms(1000));
    ASSERT_TRUE(collector.snapshot().has_loss_rtt);

    collector.reset();

    const auto snap = collector.snapshot();
    EXPECT_FALSE(snap.has_loss_rtt);
    EXPECT_DOUBLE_EQ(snap.loss_rate, 0.0);
}
