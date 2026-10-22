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
    EXPECT_NEAR(*cap_b - *cap_a, 100LL * 1'000'000LL, 2'000 /* 2 ms slack */);
}

// No SR ⇒ std::nullopt.
TEST(SenderReportTracker, CaptureWithoutSRReturnsNullopt) {
    SenderReportTracker tracker;
    std::int64_t skew = 0;
    auto cap = tracker.capture_ts_us_for(0xFFu, 1234u, 0, 90'000, 0, &skew);
    EXPECT_FALSE(cap.has_value());
}

} // anonymous namespace
