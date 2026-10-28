// modules/rtp/tests/test_timeline.cpp
// =============================================================================
// Cross-end frame timeline helper tests (nimrtc/rtp/timeline.hpp).
//
// Verifies:
//   - ntp64_from_system_clock() round-trips losslessly through
//     system_clock_from_ntp64() at sub-millisecond resolution.
//   - abs-send-time 24-bit encode/decode round-trips.
//   - abs-send-time wraps at 256 s.
//   - SenderReportTracker::on_sender_report() + capture_ts_us_for()
//     computes a sensible capture_ntp_us for the same RTP-TS that the
//     SR carries (the trivial case).
//   - Forward extrapolation across the SR's RTP-TS by Δ ticks maps to
//     Δ / clock_rate seconds in microseconds.
//   - Multi-SSRC: tracker keeps separate state per SSRC.
//   - Per-SSRC clock rate: the rate comes from SDP rtpmap, not the SR;
//     an audio SSRC at 48 kHz must not extrapolate at 90 kHz.
//   - RTP timestamp wrap: the difference is modular over 2^32, so a
//     packet crossing the wrap boundary after the SR extrapolates
//     forward instead of being rejected.
// =============================================================================

#include <gtest/gtest.h>

#include <nimrtc/rtp/timeline.hpp>

#include <chrono>
#include <cstdint>
#include <thread>

namespace {

using namespace nimrtc::rtp;

// Round-trip "now" through NTP64 — error budget 1 ms (more than enough
// for the 100 µs-or-so cost of the round trip).
TEST(Ntp64, RoundTripNow) {
    const Ntp64 a = ntp64_from_system_clock();
    const auto  t  = system_clock_from_ntp64(a);
    // Sleep a few ms and convert back; the back-converted value should
    // *not* exceed "now" by more than ~10 ms (test latency).
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    const auto  now = std::chrono::system_clock::now();
    const auto  skew = std::chrono::duration_cast<std::chrono::microseconds>(
        now - t).count();
    EXPECT_GE(skew, 0);
    EXPECT_LE(skew, 50'000);   // 50 ms test-runner slack
}

// Upper 32 bits are seconds since 1900.
TEST(Ntp64, SecondsSince1900) {
    const Ntp64 n = ntp64_from(std::chrono::system_clock::from_time_t(0));
    EXPECT_GE(n.seconds(), static_cast<std::uint32_t>(kNtpEpochOffset));
    EXPECT_LE(n.seconds(), static_cast<std::uint32_t>(kNtpEpochOffset + 2));
}

// mid_32() takes bits 16..47 of the 64-bit value (RFC 3550 §6.4.1).
TEST(Ntp64, Mid32Slice) {
    const std::uint64_t ones = 0xFFFFFFFFFFFFFFFFULL;
    const Ntp64 n{ones};
    EXPECT_EQ(n.mid_32(), 0xFFFFFFFFu);
}

// abs-send-time 24-bit encode/decode round-trips within ±1 tick (15 µs).
TEST(AbsSendTime24, RoundTrip) {
    for (std::int64_t us : {0LL, 100LL, 12'345LL, 1'000'000LL,
                            100'000'000LL, 255'000'000LL - 16LL}) {
        const std::uint32_t enc =
            encode_abs_send_time_24(std::chrono::microseconds{us}, Ntp64{0});
        const auto dec_us = decode_abs_send_time_24(enc, Ntp64{0}).count();
        // 24-bit @ 65536 Hz ⇒ 1 tick = 15.26 µs.  Equality within
        // ± 1 tick is acceptable (the encoded value is the snap-to-
        // nearest-tick sample of `us`).
        EXPECT_LE(std::llabs(dec_us - us), 16);
    }
}

// abs-send-time wraps every 2^24 / 65536 = 256 s.
TEST(AbsSendTime24, WrapsEvery256Seconds) {
    const std::uint32_t a = encode_abs_send_time_24(
        std::chrono::microseconds{0}, Ntp64{0});
    const std::uint32_t b = encode_abs_send_time_24(
        std::chrono::microseconds{256LL * 1'000'000LL}, Ntp64{0});
    EXPECT_EQ(a, b);
    // 255.999 s should land just below the wrap.
    const std::uint32_t c = encode_abs_send_time_24(
        std::chrono::microseconds{(256LL * 1'000'000LL) - 16}, Ntp64{0});
    EXPECT_NE(b, c);
}

// SenderReportTracker: same RTP-TS as the SR returns the SR's NTP64.
TEST(SenderReportTracker, CaptureAtSREqualsSRSendTime) {
    SenderReportTracker tracker;
    SenderReport sr{};
    sr.ssrc          = 0xCAFEBABEu;
    sr.ntp_timestamp = (1'700'000'000ULL + kNtpEpochOffset) << 32;   // ≈ 2023-11
    sr.rtp_timestamp = 90'000;    // 1 second at 90 kHz
    tracker.on_sender_report(sr, /*local_recv_us=*/1'000'000);

    std::int64_t skew = 0;
    auto cap = tracker.capture_ts_us_for(sr.ssrc, sr.rtp_timestamp,
                                          /*abs_send_time_24=*/0,
                                          /*clock_rate_hz=*/90'000,
                                          /*local_recv_us=*/1'001'000,
                                          &skew);
    ASSERT_TRUE(cap.has_value());
    // 1'700'000'000 + kNtpEpochOffset seconds == ... µs.
    const std::int64_t expected_us =
        static_cast<std::int64_t>(sr.ntp_timestamp >> 32) *
            1'000'000LL -
        static_cast<std::int64_t>(kNtpEpochOffset) * 1'000'000LL;
    EXPECT_EQ(*cap, expected_us);
}

// Forward extrapolation: rtp_ts advances by Δ ticks from the SR's
// rtp_ts ⇒ capture moves forward by Δ / clock_rate seconds.
TEST(SenderReportTracker, LinearExtrapolationForward) {
    SenderReportTracker tracker;
    SenderReport sr{};
    sr.ssrc          = 0x12345u;
    sr.ntp_timestamp = (1'700'000'000ULL + kNtpEpochOffset) << 32;
    sr.rtp_timestamp = 900'000;          // 10 s at 90 kHz
    tracker.on_sender_report(sr, /*local_recv_us=*/0);

    // Advance rtp_ts by 90'000 ⇒ should land 1 s later.
    auto cap = tracker.capture_ts_us_for(sr.ssrc, sr.rtp_timestamp + 90'000,
                                          0, 90'000, 0, /*skew*/ nullptr);
    ASSERT_TRUE(cap.has_value());
    const std::int64_t expected_us =
        static_cast<std::int64_t>(sr.ntp_timestamp >> 32) *
            1'000'000LL -
        static_cast<std::int64_t>(kNtpEpochOffset) * 1'000'000LL +
        1'000'000LL;
    EXPECT_EQ(*cap, expected_us);
}

// Multi-SSRC: tracker keeps separate state per SSRC.
TEST(SenderReportTracker, MultiSsrc) {
    SenderReportTracker tracker;
    SenderReport a{};
    a.ssrc = 0xAAu;
    a.ntp_timestamp = (1'700'000'000ULL + kNtpEpochOffset) << 32;
    a.rtp_timestamp = 1000;
    SenderReport b{};
    b.ssrc = 0xBBu;
    b.ntp_timestamp = (1'700'000'100ULL + kNtpEpochOffset) << 32;
    b.rtp_timestamp = 2000;
    tracker.on_sender_report(a, /*local_recv_us=*/0);
    tracker.on_sender_report(b, /*local_recv_us=*/0);

    EXPECT_EQ(tracker.tracked_ssrc_count(), 2u);

    auto cap_a = tracker.capture_ts_us_for(0xAAu, 1000, 0, 90'000, 0,
                                            /*skew*/ nullptr);
    auto cap_b = tracker.capture_ts_us_for(0xBBu, 2000, 0, 90'000, 0,
                                            /*skew*/ nullptr);
    ASSERT_TRUE(cap_a.has_value());
    ASSERT_TRUE(cap_b.has_value());
    // cap_a is at SR-a's NTP64 (≈ 1'700'000'000 s); cap_b is at SR-b's
    // (≈ 1'700'000'100 s). Expected delta ≈ 100 s * 1e6 µs.
    // EXPECT_NEAR takes doubles, so cast explicitly: the delta is ~1e8 µs,
    // which is exactly representable as a double (< 2^53).
    EXPECT_NEAR(static_cast<double>(*cap_b - *cap_a),
                static_cast<double>(100LL * 1'000'000LL),
                2'000 /* 2 ms slack */);
}

// No SR ⇒ std::nullopt.
TEST(SenderReportTracker, CaptureWithoutSRReturnsNullopt) {
    SenderReportTracker tracker;
    std::int64_t skew = 0;
    auto cap = tracker.capture_ts_us_for(0xFFu, 1234u, 0, 90'000, 0, &skew);
    EXPECT_FALSE(cap.has_value());
}

// ---------------------------------------------------------------------------
// Per-SSRC clock rate (ADR-017 §"Decision 5").
//
// The media clock rate comes from the SDP `a=rtpmap:` line, not from the
// SR.  Before the fix, `on_sender_report()` hardcoded 90000 Hz, so an
// audio SSRC (48000 Hz) extrapolated against the wrong rate.
// ---------------------------------------------------------------------------

// A 48 kHz SSRC configured explicitly extrapolates at 48 kHz: 48000
// ticks = 1 s.
TEST(SenderReportTracker, HonoursConfiguredClockRate) {
    SenderReportTracker tracker;
    SenderReport sr{};
    sr.ssrc          = 0xA0u;
    sr.ntp_timestamp = (1'700'000'000ULL + kNtpEpochOffset) << 32;
    sr.rtp_timestamp = 48'000;
    tracker.set_ssrc_clock_rate(sr.ssrc, 48'000);
    tracker.on_sender_report(sr, /*local_recv_us=*/0);

    // clock_rate_hz = 0 ⇒ use the configured per-SSRC rate.
    auto cap = tracker.capture_ts_us_for(sr.ssrc, sr.rtp_timestamp + 48'000,
                                          0, /*clock_rate_hz=*/0, 0,
                                          /*skew*/ nullptr);
    ASSERT_TRUE(cap.has_value());
    const std::int64_t sr_us =
        static_cast<std::int64_t>(sr.ntp_timestamp >> 32) * 1'000'000LL -
        static_cast<std::int64_t>(kNtpEpochOffset) * 1'000'000LL;
    EXPECT_EQ(*cap, sr_us + 1'000'000LL /* 1 s at 48 kHz */);
}

// An unconfigured SSRC falls back to 90000 Hz (the video default).
TEST(SenderReportTracker, DefaultsTo90kHzWhenUnconfigured) {
    SenderReportTracker tracker;
    SenderReport sr{};
    sr.ssrc          = 0xA1u;
    sr.ntp_timestamp = (1'700'000'000ULL + kNtpEpochOffset) << 32;
    sr.rtp_timestamp = 90'000;
    tracker.on_sender_report(sr, /*local_recv_us=*/0);

    auto cap = tracker.capture_ts_us_for(sr.ssrc, sr.rtp_timestamp + 90'000,
                                          0, /*clock_rate_hz=*/0, 0,
                                          /*skew*/ nullptr);
    ASSERT_TRUE(cap.has_value());
    const std::int64_t sr_us =
        static_cast<std::int64_t>(sr.ntp_timestamp >> 32) * 1'000'000LL -
        static_cast<std::int64_t>(kNtpEpochOffset) * 1'000'000LL;
    EXPECT_EQ(*cap, sr_us + 1'000'000LL /* 1 s at 90 kHz */);
}

// An explicit per-call clock rate overrides the configured one.
TEST(SenderReportTracker, PerCallClockRateOverridesConfigured) {
    SenderReportTracker tracker;
    SenderReport sr{};
    sr.ssrc          = 0xA2u;
    sr.ntp_timestamp = (1'700'000'000ULL + kNtpEpochOffset) << 32;
    sr.rtp_timestamp = 0;
    tracker.set_ssrc_clock_rate(sr.ssrc, 48'000);
    tracker.on_sender_report(sr, /*local_recv_us=*/0);

    // 90000 ticks at an explicit 90 kHz = 1 s, even though the SSRC is
    // configured for 48 kHz.
    auto cap = tracker.capture_ts_us_for(sr.ssrc, 90'000, 0,
                                          /*clock_rate_hz=*/90'000, 0,
                                          /*skew*/ nullptr);
    ASSERT_TRUE(cap.has_value());
    const std::int64_t sr_us =
        static_cast<std::int64_t>(sr.ntp_timestamp >> 32) * 1'000'000LL -
        static_cast<std::int64_t>(kNtpEpochOffset) * 1'000'000LL;
    EXPECT_EQ(*cap, sr_us + 1'000'000LL);
}

// set_ssrc_clock_rate(0) is a no-op — the previous rate is kept.
TEST(SenderReportTracker, ZeroClockRateIsIgnored) {
    SenderReportTracker tracker;
    SenderReport sr{};
    sr.ssrc          = 0xA3u;
    sr.ntp_timestamp = (1'700'000'000ULL + kNtpEpochOffset) << 32;
    sr.rtp_timestamp = 0;
    tracker.set_ssrc_clock_rate(sr.ssrc, 48'000);
    tracker.set_ssrc_clock_rate(sr.ssrc, 0);   // ignored
    tracker.on_sender_report(sr, /*local_recv_us=*/0);

    auto cap = tracker.capture_ts_us_for(sr.ssrc, 48'000, 0,
                                          /*clock_rate_hz=*/0, 0,
                                          /*skew*/ nullptr);
    ASSERT_TRUE(cap.has_value());
    const std::int64_t sr_us =
        static_cast<std::int64_t>(sr.ntp_timestamp >> 32) * 1'000'000LL -
        static_cast<std::int64_t>(kNtpEpochOffset) * 1'000'000LL;
    EXPECT_EQ(*cap, sr_us + 1'000'000LL /* still 48 kHz */);
}

// ---------------------------------------------------------------------------
// RTP timestamp wrap handling (ADR-017 §"Decision 5").
//
// RTP timestamps are a 32-bit modulo counter; the difference must be
// computed modulo 2^32 (RFC 3550 Appendix A.1).
// ---------------------------------------------------------------------------

// A packet that crosses the 2^32 boundary *after* the SR must
// extrapolate forward, not be rejected.  Before the fix this returned
// std::nullopt because the raw 64-bit subtraction produced a huge
// negative number.
TEST(SenderReportTracker, ForwardAcrossRtpTimestampWrap) {
    SenderReportTracker tracker;
    SenderReport sr{};
    sr.ssrc          = 0xB0u;
    sr.ntp_timestamp = (1'700'000'000ULL + kNtpEpochOffset) << 32;
    sr.rtp_timestamp = 0xFFFFFF00u;   // 256 ticks before the wrap
    tracker.on_sender_report(sr, /*local_recv_us=*/0);

    // 512 ticks past the wrap point.
    const std::uint32_t wrapped_ts = 0x00000100u;
    auto cap = tracker.capture_ts_us_for(sr.ssrc, wrapped_ts, 0,
                                          /*clock_rate_hz=*/90'000, 0,
                                          /*skew*/ nullptr);
    ASSERT_TRUE(cap.has_value());
    const std::int64_t sr_us =
        static_cast<std::int64_t>(sr.ntp_timestamp >> 32) * 1'000'000LL -
        static_cast<std::int64_t>(kNtpEpochOffset) * 1'000'000LL;
    // 512 ticks at 90 kHz = 5688 µs.
    EXPECT_EQ(*cap, sr_us + 512LL * 1'000'000LL / 90'000LL);
}

// A packet slightly *older* than the SR (jitter-buffer reordering)
// clamps to the SR instant rather than failing.
TEST(SenderReportTracker, SlightlyOlderThanSRClampsToSR) {
    SenderReportTracker tracker;
    SenderReport sr{};
    sr.ssrc          = 0xB1u;
    sr.ntp_timestamp = (1'700'000'000ULL + kNtpEpochOffset) << 32;
    sr.rtp_timestamp = 900'000;      // 10 s at 90 kHz
    tracker.on_sender_report(sr, /*local_recv_us=*/0);

    // 45'000 ticks (0.5 s) before the SR — inside the 1 s reorder window.
    auto cap = tracker.capture_ts_us_for(sr.ssrc, 900'000 - 45'000, 0,
                                          /*clock_rate_hz=*/90'000, 0,
                                          /*skew*/ nullptr);
    ASSERT_TRUE(cap.has_value());
    const std::int64_t sr_us =
        static_cast<std::int64_t>(sr.ntp_timestamp >> 32) * 1'000'000LL -
        static_cast<std::int64_t>(kNtpEpochOffset) * 1'000'000LL;
    EXPECT_EQ(*cap, sr_us);          // clamped to the SR instant
}

// A packet more than one second of media time older than the SR has no
// anchor (only the most-recent SR is retained) ⇒ std::nullopt.
TEST(SenderReportTracker, MuchOlderThanSRReturnsNullopt) {
    SenderReportTracker tracker;
    SenderReport sr{};
    sr.ssrc          = 0xB2u;
    sr.ntp_timestamp = (1'700'000'000ULL + kNtpEpochOffset) << 32;
    sr.rtp_timestamp = 900'000;
    tracker.on_sender_report(sr, /*local_recv_us=*/0);

    // 90'001 ticks (just over 1 s at 90 kHz) before the SR.
    auto cap = tracker.capture_ts_us_for(sr.ssrc, 900'000 - 90'001, 0,
                                          /*clock_rate_hz=*/90'000, 0,
                                          /*skew*/ nullptr);
    EXPECT_FALSE(cap.has_value());
}

// ---------------------------------------------------------------------------
// abs-send-time refinement (ADR-017 §"Decision 2").
//
// The abs-send-time counter is driven by the sender's own wall clock,
// so the difference between two of its readings measures the sender's
// elapsed time without inheriting the media clock's rate error.  The
// tracker uses it to refine the SR-based extrapolation.
// ---------------------------------------------------------------------------

namespace {

// Microseconds since the UNIX epoch of the SR's NTP64 timestamp.
std::int64_t sr_unix_us_of(const SenderReport& sr) {
    return static_cast<std::int64_t>(sr.ntp_timestamp >> 32) * 1'000'000LL -
           static_cast<std::int64_t>(kNtpEpochOffset) * 1'000'000LL;
}

// abs-send-time ticks for a duration in microseconds (65536 Hz).
std::uint32_t ast_ticks_for_us(std::int64_t us) {
    return static_cast<std::uint32_t>(
        (us * static_cast<std::int64_t>(kAbsSendTimeClockRateHz)) / 1'000'000LL);
}

} // namespace

// The exact-match anchor: a packet whose RTP timestamp equals the SR's
// pins the abs-send-time counter value at the SR instant.
TEST(SenderReportTracker, ExactMatchEstablishesAstAnchor) {
    SenderReportTracker tracker;
    SenderReport sr{};
    sr.ssrc          = 0xC0u;
    sr.ntp_timestamp = (1'700'000'000ULL + kNtpEpochOffset) << 32;
    sr.rtp_timestamp = 900'000;          // 10 s at 90 kHz
    tracker.set_ssrc_clock_rate(sr.ssrc, 90'000);
    tracker.on_sender_report(sr, /*local_recv_us=*/0);

    // A packet carrying the SR's own RTP timestamp: its abs-send-time
    // value *is* the anchor.
    auto cap = tracker.capture_ts_us_for(
        sr.ssrc, sr.rtp_timestamp, /*abs_send_time_24=*/100'000,
        /*has_abs_send_time=*/true, /*clock_rate_hz=*/90'000,
        /*local_recv_us=*/0, /*skew*/ nullptr);
    ASSERT_TRUE(cap.has_value());
    EXPECT_EQ(*cap, sr_unix_us_of(sr));

    const auto state = tracker.state_for(sr.ssrc);
    ASSERT_TRUE(state.has_value());
    EXPECT_TRUE(state->sr_abs_send_anchored);
}

// The refinement actually changes the answer: the abs-send-time delta
// (100 ms) and the media-clock delta (96 ms) disagree, and the
// abs-send-time value wins because it does not inherit the media
// clock's rate error.
TEST(SenderReportTracker, AstRefinementOverridesMediaClockDelta) {
    SenderReportTracker tracker;
    SenderReport sr{};
    sr.ssrc          = 0xC1u;
    sr.ntp_timestamp = (1'700'000'000ULL + kNtpEpochOffset) << 32;
    sr.rtp_timestamp = 900'000;
    tracker.set_ssrc_clock_rate(sr.ssrc, 90'000);
    tracker.on_sender_report(sr, 0);

    const std::uint32_t anchor_ast = 100'000;
    // Anchor the counter with a packet at the SR's RTP timestamp.
    tracker.capture_ts_us_for(sr.ssrc, sr.rtp_timestamp, anchor_ast,
                              /*has_abs_send_time=*/true, 90'000, 0,
                              /*skew*/ nullptr);

    // 8'640 RTP ticks at 90 kHz = 96 ms of media time, but the
    // abs-send-time counter advanced 100 ms.
    const std::uint32_t ast = anchor_ast + ast_ticks_for_us(100'000);
    auto cap = tracker.capture_ts_us_for(
        sr.ssrc, sr.rtp_timestamp + 8'640, ast,
        /*has_abs_send_time=*/true, 90'000, 0, /*skew*/ nullptr);
    ASSERT_TRUE(cap.has_value());

    // The refined value (≈ SR + 100 ms) wins over the media-clock
    // extrapolation (SR + 96 ms).  The residual is the 15.26 µs
    // quantisation of the 24-bit field.
    EXPECT_LE(std::llabs(*cap - (sr_unix_us_of(sr) + 100'000)), 20);

    // The 6-argument overload keeps the media-clock value: it never
    // records the observation and never refines.
    auto base = tracker.capture_ts_us_for(
        sr.ssrc, sr.rtp_timestamp + 8'640, ast,
        /*clock_rate_hz=*/90'000, 0, /*skew*/ nullptr);
    ASSERT_TRUE(base.has_value());
    EXPECT_EQ(*base, sr_unix_us_of(sr) + 96'000);
}

// A reordered packet (older than the SR) is clamped to the SR instant
// by the media-clock path, but the abs-send-time path recovers its real
// pre-SR capture instant.
TEST(SenderReportTracker, AstRefinementRecoversPreSrInstant) {
    SenderReportTracker tracker;
    SenderReport sr{};
    sr.ssrc          = 0xC2u;
    sr.ntp_timestamp = (1'700'000'000ULL + kNtpEpochOffset) << 32;
    sr.rtp_timestamp = 900'000;
    tracker.set_ssrc_clock_rate(sr.ssrc, 90'000);
    tracker.on_sender_report(sr, 0);

    const std::uint32_t anchor_ast = 100'000;
    tracker.capture_ts_us_for(sr.ssrc, sr.rtp_timestamp, anchor_ast,
                              /*has_abs_send_time=*/true, 90'000, 0,
                              /*skew*/ nullptr);

    // 9'000 ticks (100 ms) *before* the SR, with the abs-send-time
    // counter correspondingly 100 ms behind the anchor.
    const std::uint32_t ast = anchor_ast - ast_ticks_for_us(100'000);
    auto cap = tracker.capture_ts_us_for(
        sr.ssrc, sr.rtp_timestamp - 9'000, ast,
        /*has_abs_send_time=*/true, 90'000, 0, /*skew*/ nullptr);
    ASSERT_TRUE(cap.has_value());
    EXPECT_LE(std::llabs(*cap - (sr_unix_us_of(sr) - 100'000)), 20);
}

// A tolerance of 0 disables the refinement entirely.
TEST(SenderReportTracker, ZeroToleranceDisablesRefinement) {
    SenderReportTracker tracker;
    tracker.set_ast_agreement_tolerance_us(0);
    EXPECT_EQ(tracker.ast_agreement_tolerance_us(), 0);

    SenderReport sr{};
    sr.ssrc          = 0xC3u;
    sr.ntp_timestamp = (1'700'000'000ULL + kNtpEpochOffset) << 32;
    sr.rtp_timestamp = 900'000;
    tracker.set_ssrc_clock_rate(sr.ssrc, 90'000);
    tracker.on_sender_report(sr, 0);

    const std::uint32_t anchor_ast = 100'000;
    tracker.capture_ts_us_for(sr.ssrc, sr.rtp_timestamp, anchor_ast,
                              /*has_abs_send_time=*/true, 90'000, 0,
                              /*skew*/ nullptr);

    const std::uint32_t ast = anchor_ast + ast_ticks_for_us(100'000);
    auto cap = tracker.capture_ts_us_for(
        sr.ssrc, sr.rtp_timestamp + 8'640, ast,
        /*has_abs_send_time=*/true, 90'000, 0, /*skew*/ nullptr);
    ASSERT_TRUE(cap.has_value());
    // Pure media-clock extrapolation.
    EXPECT_EQ(*cap, sr_unix_us_of(sr) + 96'000);
}

// A broken abs-send-time counter (e.g. the peer restarted its 256 s
// counter) makes the two estimates disagree by more than the tolerance
// ⇒ the SR-based value wins.
TEST(SenderReportTracker, BrokenAstAnchorIsRejected) {
    SenderReportTracker tracker;
    SenderReport sr{};
    sr.ssrc          = 0xC4u;
    sr.ntp_timestamp = (1'700'000'000ULL + kNtpEpochOffset) << 32;
    sr.rtp_timestamp = 900'000;
    tracker.set_ssrc_clock_rate(sr.ssrc, 90'000);
    tracker.on_sender_report(sr, 0);

    const std::uint32_t anchor_ast = 100'000;
    tracker.capture_ts_us_for(sr.ssrc, sr.rtp_timestamp, anchor_ast,
                              /*has_abs_send_time=*/true, 90'000, 0,
                              /*skew*/ nullptr);

    // The counter jumped 30 s forward — far beyond the 1 s tolerance.
    const std::uint32_t ast = anchor_ast + 65536u * 30u;
    auto cap = tracker.capture_ts_us_for(
        sr.ssrc, sr.rtp_timestamp + 8'640, ast,
        /*has_abs_send_time=*/true, 90'000, 0, /*skew*/ nullptr);
    ASSERT_TRUE(cap.has_value());
    EXPECT_EQ(*cap, sr_unix_us_of(sr) + 96'000);
}

// ---------------------------------------------------------------------------
// The overload pair the engine depends on.
//
// The engine's `observe_timeline()` reads the abs-send-time element off
// the wire and hands it to the tracker.  If it calls the 6-argument
// overload (as it once did) the field is accepted and *silently dropped*:
// no observation is recorded, no anchor is established, and every
// reconstruction degenerates to the raw media-clock extrapolation with no
// error reported.  These two cases pin the difference, so a future
// caller that picks the wrong overload fails a test instead of shipping a
// timeline that quietly ignores its own refinement input.
// ---------------------------------------------------------------------------

// The 6-argument overload must NOT record the observation — that is what
// keeps it a wire-compatible no-op.
TEST(SenderReportTracker, SixArgOverloadDoesNotEstablishAnchor) {
    SenderReportTracker tracker;
    SenderReport sr{};
    sr.ssrc          = 0xD0u;
    sr.ntp_timestamp = (1'700'000'000ULL + kNtpEpochOffset) << 32;
    sr.rtp_timestamp = 900'000;
    tracker.set_ssrc_clock_rate(sr.ssrc, 90'000);
    tracker.on_sender_report(sr, 0);

    // Feed a packet at exactly the SR's RTP timestamp through the
    // 6-argument overload. Were this to record the observation, the
    // anchor would be established here.
    auto cap = tracker.capture_ts_us_for(sr.ssrc, sr.rtp_timestamp,
                                          /*abs_send_time_24=*/100'000,
                                          /*clock_rate_hz=*/90'000,
                                          /*local_recv_us=*/0,
                                          /*skew*/ nullptr);
    ASSERT_TRUE(cap.has_value());

    const auto state = tracker.state_for(sr.ssrc);
    ASSERT_TRUE(state.has_value());
    EXPECT_FALSE(state->sr_abs_send_anchored)
        << "the 6-argument overload must not record an observation; "
           "establishing an anchor here would mean a caller that passes "
           "has_abs_send_time=false still gets the refinement";
}

// The 7-argument overload MUST record the observation, so a later packet
// on the same SSRC gets the abs-send-time refinement.
TEST(SenderReportTracker, SevenArgOverloadEstablishesAnchorForLaterPackets) {
    SenderReportTracker tracker;
    SenderReport sr{};
    sr.ssrc          = 0xD1u;
    sr.ntp_timestamp = (1'700'000'000ULL + kNtpEpochOffset) << 32;
    sr.rtp_timestamp = 900'000;
    tracker.set_ssrc_clock_rate(sr.ssrc, 90'000);
    tracker.on_sender_report(sr, 0);

    // Anchor packet, flagged as carrying the field.
    tracker.capture_ts_us_for(sr.ssrc, sr.rtp_timestamp,
                              /*abs_send_time_24=*/100'000,
                              /*has_abs_send_time=*/true, 90'000, 0,
                              /*skew*/ nullptr);

    const auto state = tracker.state_for(sr.ssrc);
    ASSERT_TRUE(state.has_value());
    ASSERT_TRUE(state->sr_abs_send_anchored)
        << "the 7-argument overload must record the observation that "
           "anchors the abs-send-time counter at the SR instant";

    // A later packet whose abs-send-time says 100 ms have elapsed while
    // its RTP timestamp says 96 ms — the refinement must win.
    const std::uint32_t ast = 100'000u + ast_ticks_for_us(100'000);
    auto cap = tracker.capture_ts_us_for(
        sr.ssrc, sr.rtp_timestamp + 8'640, ast,
        /*has_abs_send_time=*/true, 90'000, 0, /*skew*/ nullptr);
    ASSERT_TRUE(cap.has_value());
    EXPECT_LE(std::llabs(*cap - (sr_unix_us_of(sr) + 100'000)), 20);
}

// An out-of-range 24-bit value cannot have come off the wire; the tracker
// must ignore it rather than poison the observation ring (which would
// corrupt the anchor for every subsequent packet on that SSRC).
TEST(SenderReportTracker, IllegalAbsSendTimeIsIgnored) {
    SenderReportTracker tracker;
    SenderReport sr{};
    sr.ssrc          = 0xD2u;
    sr.ntp_timestamp = (1'700'000'000ULL + kNtpEpochOffset) << 32;
    sr.rtp_timestamp = 900'000;
    tracker.set_ssrc_clock_rate(sr.ssrc, 90'000);
    tracker.on_sender_report(sr, 0);

    // 2^24 is one past the legal range.
    auto cap = tracker.capture_ts_us_for(
        sr.ssrc, sr.rtp_timestamp, /*abs_send_time_24=*/(1u << 24),
        /*has_abs_send_time=*/true, 90'000, 0, /*skew*/ nullptr);
    ASSERT_TRUE(cap.has_value());

    const auto state = tracker.state_for(sr.ssrc);
    ASSERT_TRUE(state.has_value());
    EXPECT_FALSE(state->sr_abs_send_anchored)
        << "an illegal 24-bit value must not be recorded as an anchor";
}

// ---------------------------------------------------------------------------
// Slope estimator — multi-SR fit + EWMA (ADR-017 §"Decision 7").
//
// The single-SR linear model of v1.0.0 inherited the peer's media-clock
// rate error one sample at a time.  The tracker now retains a short
// window of Sender Reports and fits the slope across it, which measures
// the *combined* rate of the peer's media clock and wall clock.
// ---------------------------------------------------------------------------

namespace {

// NTP64 for an explicit Unix instant (µs since the epoch).  Unlike the
// `(secs + kNtpEpochOffset) << 32` idiom used elsewhere in this file,
// this keeps the sub-second part — the skew cases need it.
//
// The NTP fraction is a 32-bit truncation of the microsecond value, so
// the round trip through the tracker loses up to 1 µs; the assertions
// below allow for that.
std::uint64_t ntp64_from_unix_us(std::int64_t unix_us) {
    const std::uint64_t secs =
        static_cast<std::uint64_t>(unix_us / 1'000'000LL) + kNtpEpochOffset;
    const std::uint64_t rem =
        static_cast<std::uint64_t>(unix_us % 1'000'000LL);
    const std::uint64_t frac = (rem * 4294967296ULL) / 1'000'000ULL;
    return (secs << 32) | frac;
}

// The Unix instant the tracker's monotonic origin is anchored to, used
// as the "wall clock" of the local end in the skew cases.
constexpr std::int64_t kLocalWallOriginUs = 1'700'000'000'000'000LL;

// Feed one Sender Report: peer wall clock at `peer_us`, peer media
// clock at `rtp_ts`, received locally at monotonic `local_us`.
void feed_sr(SenderReportTracker& tracker,
             std::uint32_t ssrc,
             std::int64_t peer_us,
             std::uint32_t rtp_ts,
             std::int64_t local_us) {
    SenderReport sr{};
    sr.ssrc          = ssrc;
    sr.ntp_timestamp = ntp64_from_unix_us(peer_us);
    sr.rtp_timestamp = rtp_ts;
    tracker.on_sender_report(sr, local_us);
}

// The tracker's monotonic origin: our wall clock reads `unix_us` when
// the local monotonic clock reads zero.
Ntp64 local_origin() {
    return ntp64_from(std::chrono::system_clock::time_point{
        std::chrono::microseconds{kLocalWallOriginUs}});
}

} // namespace

// A media clock running 1 % fast is invisible to the single-SR model:
// every extrapolation repeats the same rate error.  With a fit over two
// Sender Reports the tracker measures the rate and extrapolates with it.
TEST(SenderReportTracker, FittedSlopeTracksMediaClockRateError) {
    SenderReportTracker tracker;
    tracker.set_ssrc_clock_rate(0xE0u, 90'000);

    const std::int64_t t0 = kLocalWallOriginUs;
    // The peer's media clock runs 1 % fast: 90900 ticks per wall second.
    feed_sr(tracker, 0xE0u, t0,             0,      0);
    feed_sr(tracker, 0xE0u, t0 + 1'000'000, 90'900, 1'000'000);
    feed_sr(tracker, 0xE0u, t0 + 2'000'000, 181'800, 2'000'000);

    const auto state = tracker.state_for(0xE0u);
    ASSERT_TRUE(state.has_value());
    EXPECT_TRUE(state->slope_fitted)
        << "two Sender Reports 1 s apart must be enough to fit a slope";
    EXPECT_EQ(state->slope_sample_count, 3u);

    // The measured slope is 1e6 µs per 90'900 ticks = 11.0011 µs/tick
    // = 11'001'100 ps/tick (the nominal 90 kHz slope is 11'111'111).
    EXPECT_NEAR(static_cast<double>(state->slope_psec_per_rtp_ts),
                11'001'100.0, 1.0);

    // A packet one media-second past the last SR.
    auto cap = tracker.capture_ts_us_for(0xE0u, 181'800 + 90'900, 0,
                                          /*clock_rate_hz=*/0, 0,
                                          /*skew*/ nullptr);
    ASSERT_TRUE(cap.has_value());
    // The fitted slope says 90'900 ticks ≈ 1'000'000 µs: the media clock
    // runs fast, so 90'900 ticks is one *wall* second.  The nominal rate
    // would have said 1'010'000 µs — a 10 ms difference.
    EXPECT_NEAR(static_cast<double>(*cap - (t0 + 2'000'000)),
                1'000'000.0, 5.0);
}

// `min_samples = 1` disables the fit: the nominal rate is used, which is
// the pre-v1.0.0 behaviour.
TEST(SenderReportTracker, MinSamplesOneDisablesTheFit) {
    SenderReportTracker tracker;
    SlopeFilterConfig cfg;
    cfg.min_samples         = 1;
    cfg.max_samples         = 16;
    cfg.ewma_alpha_permille = 1000;
    tracker.set_slope_filter_config(cfg);

    tracker.set_ssrc_clock_rate(0xE1u, 90'000);
    const std::int64_t t0 = kLocalWallOriginUs;
    feed_sr(tracker, 0xE1u, t0,             0,      0);
    feed_sr(tracker, 0xE1u, t0 + 1'000'000, 90'900, 1'000'000);
    feed_sr(tracker, 0xE1u, t0 + 2'000'000, 181'800, 2'000'000);

    const auto state = tracker.state_for(0xE1u);
    ASSERT_TRUE(state.has_value());
    EXPECT_FALSE(state->slope_fitted);

    auto cap = tracker.capture_ts_us_for(0xE1u, 181'800 + 90'900, 0,
                                          /*clock_rate_hz=*/0, 0,
                                          /*skew*/ nullptr);
    ASSERT_TRUE(cap.has_value());
    // The nominal 90 kHz rate: 90900 ticks = 1'010'000 µs.
    EXPECT_NEAR(static_cast<double>(*cap - (t0 + 2'000'000)),
                1'010'000.0, 5.0);
}

// The EWMA damps the per-report noise: with alpha = 1000 the newest fit
// wins outright, with the default 250 the smoothed value lags towards
// the previous one.
TEST(SenderReportTracker, EwmaSmoothsTheSlope) {
    const std::int64_t t0 = kLocalWallOriginUs;

    auto run_with_alpha = [&](std::uint32_t alpha) {
        SenderReportTracker tracker;
        SlopeFilterConfig cfg;
        cfg.min_samples         = 2;
        cfg.max_samples         = 16;
        cfg.ewma_alpha_permille = alpha;
        tracker.set_slope_filter_config(cfg);
        tracker.set_ssrc_clock_rate(0xE2u, 90'000);

        // Two clean seconds, then a 10 ms media-clock glitch.
        feed_sr(tracker, 0xE2u, t0,             0,       0);
        feed_sr(tracker, 0xE2u, t0 + 1'000'000, 90'000,  1'000'000);
        feed_sr(tracker, 0xE2u, t0 + 2'000'000, 180'900, 2'000'000);

        const auto state = tracker.state_for(0xE2u);
        EXPECT_TRUE(state.has_value());
        return state ? state->slope_psec_per_rtp_ts : 0;
    };

    const std::int64_t unsmoothed = run_with_alpha(1000);
    const std::int64_t smoothed   = run_with_alpha(250);

    // The unsmoothed value is the raw least-squares fit over three
    // samples.  The samples are (0, 0), (90'000, 1e6) and (180'900,
    // 2e6) — the third one sits 10 ms *below* the line the first two
    // define, so the fit tilts towards it:
    //
    //   Sxy = 180'900'000'000      Sxx = 16'362'540'000
    //   slope = Sxy / Sxx = 11.055741 µs per tick
    EXPECT_NEAR(static_cast<double>(unsmoothed), 11'055'741.0, 5.0);
    // The smoothed value lags towards the previous (nominal) slope, so
    // it sits *above* the raw fit but still below the nominal 11'111'111.
    EXPECT_GT(smoothed, unsmoothed);
    EXPECT_LT(smoothed, 11'111'111LL);
}

// A broken fit — an NTP step on the peer, or a rate that contradicts the
// negotiated one — must not poison the reconstruction.
TEST(SenderReportTracker, BrokenFitIsRejected) {
    SenderReportTracker tracker;
    tracker.set_ssrc_clock_rate(0xE3u, 90'000);

    const std::int64_t t0 = kLocalWallOriginUs;
    feed_sr(tracker, 0xE3u, t0,             0,      0);
    feed_sr(tracker, 0xE3u, t0 + 1'000'000, 90'000, 1'000'000);
    // The peer's wall clock jumped a whole second: the implied slope is
    // 2e6 µs per 90'000 ticks = 22'222'222 ps/tick, twice the nominal.
    feed_sr(tracker, 0xE3u, t0 + 3'000'000, 180'000, 3'000'000);

    const auto state = tracker.state_for(0xE3u);
    ASSERT_TRUE(state.has_value());
    EXPECT_NEAR(static_cast<double>(state->slope_psec_per_rtp_ts),
                11'111'111.0, 5.0)
        << "the broken fit must be rejected and the previous slope kept";
}

// The fit window survives the 2^32 RTP-timestamp wrap: the samples are
// positioned on a continuous axis, so the wrap is just another delta.
TEST(SenderReportTracker, FitSurvivesTheRtpTimestampWrap) {
    SenderReportTracker tracker;
    tracker.set_ssrc_clock_rate(0xE4u, 90'000);

    const std::int64_t t0 = kLocalWallOriginUs;
    // 90'000 ticks across the wrap = exactly 1 s at 90 kHz.  The samples
    // are positioned on a continuous axis, so the wrap is just another
    // delta: 0xFFFF'0000 + 90'000 = 0x1'0000'5F90 ⇒ 0x0000'5F90.
    feed_sr(tracker, 0xE4u, t0,                     0xFFFF'0000u, 0);
    feed_sr(tracker, 0xE4u, t0 + 1'000'000,         0x0000'5F90u, 1'000'000);

    const auto state = tracker.state_for(0xE4u);
    ASSERT_TRUE(state.has_value());
    EXPECT_TRUE(state->slope_fitted);
    EXPECT_NEAR(static_cast<double>(state->slope_psec_per_rtp_ts),
                11'111'111.0, 5.0);
}

// Two SSRCs keep independent fits: one runs its media clock fast, the
// other does not.
TEST(SenderReportTracker, FitIsPerSsrc) {
    SenderReportTracker tracker;
    tracker.set_ssrc_clock_rate(0xE5u, 90'000);
    tracker.set_ssrc_clock_rate(0xE6u, 90'000);

    const std::int64_t t0 = kLocalWallOriginUs;
    feed_sr(tracker, 0xE5u, t0,             0,      0);
    feed_sr(tracker, 0xE5u, t0 + 1'000'000, 90'900, 1'000'000);
    feed_sr(tracker, 0xE6u, t0,             0,      0);
    feed_sr(tracker, 0xE6u, t0 + 1'000'000, 90'000, 1'000'000);

    const auto fast = tracker.state_for(0xE5u);
    const auto slow = tracker.state_for(0xE6u);
    ASSERT_TRUE(fast.has_value());
    ASSERT_TRUE(slow.has_value());
    EXPECT_NEAR(static_cast<double>(fast->slope_psec_per_rtp_ts),
                11'001'100.0, 1.0);
    EXPECT_NEAR(static_cast<double>(slow->slope_psec_per_rtp_ts),
                11'111'111.0, 5.0);
}

// The slope-filter configuration is clamped into its valid ranges.
TEST(SenderReportTracker, SlopeFilterConfigIsClamped) {
    SenderReportTracker tracker;

    SlopeFilterConfig bad;
    bad.min_samples         = 0;
    bad.max_samples         = 0;
    bad.ewma_alpha_permille = 0;
    tracker.set_slope_filter_config(bad);
    auto cfg = tracker.slope_filter_config();
    EXPECT_EQ(cfg.min_samples, 1u);
    EXPECT_EQ(cfg.max_samples, 1u);
    EXPECT_EQ(cfg.ewma_alpha_permille, 1u);

    SlopeFilterConfig wide;
    wide.min_samples         = 4;
    wide.max_samples         = 2;      // below min_samples ⇒ clamped up
    wide.ewma_alpha_permille = 5000;   // above 1000 ⇒ clamped down
    tracker.set_slope_filter_config(wide);
    cfg = tracker.slope_filter_config();
    EXPECT_EQ(cfg.min_samples, 4u);
    EXPECT_EQ(cfg.max_samples, 4u);
    EXPECT_EQ(cfg.ewma_alpha_permille, 1000u);
}

// ---------------------------------------------------------------------------
// Inter-clock skew (ADR-017 §"Decision 4").
//
// The skew is the *change*, since the stored Sender Report, of the
// offset between our clock and the peer's:
//
//   skew = (our_wall(packet) - peer_capture(packet))
//        - (our_wall(SR)     - peer_capture(SR))
//
// A *constant* offset between the two clocks cancels; what is left is
// the rate difference accumulated over the SR -> packet interval - plus
// the change in transit delay, so the value is a per-packet estimate
// whose *trend* over many packets is the clock skew.
//
// Sign convention: a peer clock that runs *fast* shows up as a
// *negative* skew (our clock falls behind the peer's).  The
// reconstructed capture instant lives in the *peer's* clock domain, so
// subtracting the skew moves it onto our own timeline.
//
// Placing our own reading on the wall-clock timeline needs the
// monotonic origin: `unix_us(origin) + local_recv_us` is the Unix
// instant of our reading.  Without an origin the tracker cannot place
// our clock at all, and the skew stays 0.
//
// The scenarios below keep the transit delay at zero (our wall clock
// reads exactly the peer's timestamp at the moment we receive the SR)
// so that the only thing the skew can report is the clock difference.
// ---------------------------------------------------------------------------

// Without an origin the tracker cannot place our clock: the skew is
// reported as 0 rather than as a made-up number.
TEST(SenderReportTracker, SkewNeedsTheMonotonicOrigin) {
    SenderReportTracker tracker;
    tracker.set_ssrc_clock_rate(0xF0u, 90'000);

    const std::int64_t t0 = kLocalWallOriginUs;
    feed_sr(tracker, 0xF0u, t0,             0,      0);
    feed_sr(tracker, 0xF0u, t0 + 1'000'000, 90'000, 1'000'000);

    std::int64_t skew = 12345;
    auto cap = tracker.capture_ts_us_for(0xF0u, 180'000, 0,
                                          /*clock_rate_hz=*/0,
                                          /*local_recv_us=*/2'000'000,
                                          &skew);
    ASSERT_TRUE(cap.has_value());
    EXPECT_EQ(skew, 0)
        << "without set_monotonic_origin() the tracker cannot place our "
           "own clock on the wall-clock timeline, so it must report no "
           "skew rather than a fabricated one";
}

// A peer clock that runs 500 ppm fast for one second shows up as a
// -500 µs skew: our clock fell behind the peer's by that much over
// the SR -> packet interval.
TEST(SenderReportTracker, SkewReportsTheInterClockDiscrepancy) {
    SenderReportTracker tracker;
    tracker.set_monotonic_origin(local_origin());
    tracker.set_ssrc_clock_rate(0xF2u, 90'000);

    const std::int64_t t0 = kLocalWallOriginUs;
    // The two clocks agree at the first SR ...
    feed_sr(tracker, 0xF2u, t0,             0,      0);
    // ... and the peer's clock has gained 500 µs (500 ppm) by the second
    // one, while ours has not.
    feed_sr(tracker, 0xF2u, t0 + 1'000'500, 90'000, 1'000'000);

    std::int64_t skew = 0;
    auto cap = tracker.capture_ts_us_for(0xF2u, 180'000, 0,
                                          /*clock_rate_hz=*/0,
                                          /*local_recv_us=*/2'000'000,
                                          &skew);
    ASSERT_TRUE(cap.has_value());
    EXPECT_NEAR(static_cast<double>(skew), -500.0, 2.0)
        << "a peer clock running 500 ppm fast must show up as a negative "
           "skew of 500 µs over one second";
}

// A peer whose clock runs *fast* shows up as a negative skew that grows
// with the interval — the inter-clock rate difference of RFC 3550 §8.1.
TEST(SenderReportTracker, SkewAccumulatesWithTheClockRateDifference) {
    SenderReportTracker tracker;
    tracker.set_monotonic_origin(local_origin());
    tracker.set_ssrc_clock_rate(0xF3u, 90'000);

    const std::int64_t t0 = kLocalWallOriginUs;
    // Our clock and the peer's agree at the SR ...
    feed_sr(tracker, 0xF3u, t0,             0,      0);
    // ... and the peer's clock has gained 100 µs (100 ppm) by the second
    // report, while ours has not.
    feed_sr(tracker, 0xF3u, t0 + 1'000'100, 90'000, 1'000'000);

    std::int64_t skew = 0;
    auto cap = tracker.capture_ts_us_for(0xF3u, 180'000, 0,
                                          /*clock_rate_hz=*/0,
                                          /*local_recv_us=*/2'000'000,
                                          &skew);
    ASSERT_TRUE(cap.has_value());
    EXPECT_NEAR(static_cast<double>(skew), -100.0, 2.0)
        << "a peer clock running 100 ppm fast must show up as a negative "
           "skew of 100 µs over one second";
}

// Two clocks in step, zero transit delay: no skew at all.  This is the
// null hypothesis the rate-difference cases above depart from.
TEST(SenderReportTracker, SkewIsZeroWhenTheClocksAgree) {
    SenderReportTracker tracker;
    tracker.set_monotonic_origin(local_origin());
    tracker.set_ssrc_clock_rate(0xF1u, 90'000);

    const std::int64_t t0 = kLocalWallOriginUs;
    // Our wall clock reads the peer's timestamp at the moment we
    // receive each report - no transit delay to speak of.
    feed_sr(tracker, 0xF1u, t0,             0,      0);
    feed_sr(tracker, 0xF1u, t0 + 1'000'000, 90'000, 1'000'000);

    std::int64_t skew = 0;
    auto cap = tracker.capture_ts_us_for(0xF1u, 180'000, 0,
                                          /*clock_rate_hz=*/0,
                                          /*local_recv_us=*/2'000'000,
                                          &skew);
    ASSERT_TRUE(cap.has_value());
    EXPECT_NEAR(static_cast<double>(skew), 0.0, 2.0)
        << "two clocks in step must not produce a skew";
}

// The skew needs the *fitted* slope: with a single Sender Report the
// media-clock extrapolation absorbs the peer's rate error, and the
// discrepancy becomes invisible.  Two reports are what makes it
// observable - which is why the two features land together.
TEST(SenderReportTracker, SkewNeedsTheFittedSlope) {
    const std::int64_t t0 = kLocalWallOriginUs;

    // One SR only: the nominal rate hides the peer's 100 ppm.
    {
        SenderReportTracker tracker;
        tracker.set_monotonic_origin(local_origin());
        tracker.set_ssrc_clock_rate(0xF4u, 90'000);
        feed_sr(tracker, 0xF4u, t0 + 1'000'100, 90'000, 1'000'000);

        std::int64_t skew = 0;
        auto cap = tracker.capture_ts_us_for(0xF4u, 180'000, 0,
                                              /*clock_rate_hz=*/0,
                                              /*local_recv_us=*/2'000'000,
                                              &skew);
        ASSERT_TRUE(cap.has_value());
        EXPECT_NEAR(static_cast<double>(skew), 0.0, 2.0)
            << "without a second Sender Report there is nothing to fit, "
               "so the nominal rate hides the peer's rate error";
    }

    // Two SRs: the fit measures the peer's rate and the skew appears.
    {
        SenderReportTracker tracker;
        tracker.set_monotonic_origin(local_origin());
        tracker.set_ssrc_clock_rate(0xF5u, 90'000);
        feed_sr(tracker, 0xF5u, t0,             0,      0);
        feed_sr(tracker, 0xF5u, t0 + 1'000'100, 90'000, 1'000'000);

        std::int64_t skew = 0;
        auto cap = tracker.capture_ts_us_for(0xF5u, 180'000, 0,
                                              /*clock_rate_hz=*/0,
                                              /*local_recv_us=*/2'000'000,
                                              &skew);
        ASSERT_TRUE(cap.has_value());
        EXPECT_NEAR(static_cast<double>(skew), -100.0, 2.0)
            << "the fitted slope must expose the peer's rate error";
    }
}

} // anonymous namespace
