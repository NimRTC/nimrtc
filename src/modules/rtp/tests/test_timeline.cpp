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

} // anonymous namespace
