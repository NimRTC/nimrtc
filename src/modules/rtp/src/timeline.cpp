// modules/rtp/src/timeline.cpp
// -----------------------------------------------------------------------------
// Implementation of the cross-end frame timeline helpers declared in
// nimrtc/rtp/timeline.hpp.  See that header for the design notes (RFC 6051
// abs-send-time + RFC 3550 §6.4.1 RTCP SR + a Chrome-shaped linear slope).
// -----------------------------------------------------------------------------

#include <nimrtc/rtp/timeline.hpp>
#include <nimrtc/core/log.hpp>

#include <chrono>
#include <cstdint>
#include <vector>
#include <unordered_map>

namespace nimrtc::rtp {

// -----------------------------------------------------------------------------
// NTP 64-bit ↔ system_clock conversion (RFC 5905 §6).
// -----------------------------------------------------------------------------
// NTP epoch is 1900-01-01 00:00:00 UTC; UNIX is 1970-01-01.  We work in
// microseconds to avoid losing precision when the result is fed back into
// std::chrono.

namespace {
constexpr std::int64_t  kMicrosecondsPerSecond = 1'000'000LL;
constexpr std::uint64_t kNtpFracMicro          = 4294967296ULL;
constexpr std::uint64_t kRtpTsHalfRange        = 1ULL << 31;
} // namespace

Ntp64 ntp64_from_system_clock() noexcept {
    return ntp64_from(std::chrono::system_clock::now());
}

Ntp64 ntp64_from(std::chrono::system_clock::time_point t) noexcept {
    using namespace std::chrono;
    const auto since_epoch_us =
        duration_cast<microseconds>(t.time_since_epoch()).count();
    if (since_epoch_us < 0) {
        return Ntp64{0};
    }
    const std::int64_t  unix_seconds = since_epoch_us / kMicrosecondsPerSecond;
    const std::int64_t  unix_micros  = since_epoch_us % kMicrosecondsPerSecond;
    const std::uint64_t ntp_seconds  =
        static_cast<std::uint64_t>(unix_seconds) + kNtpEpochOffset;
    // Pack the sub-second fraction.  (unix_micros * 2^32) / 1e6 ≤ ~4.3e15,
    // which comfortably fits in uint64 — no __int128 needed.
    std::uint64_t frac_u =
        (static_cast<std::uint64_t>(unix_micros) * kNtpFracMicro) /
            static_cast<std::uint64_t>(kMicrosecondsPerSecond);
    if (frac_u >= (1ULL << 32)) frac_u = (1ULL << 32) - 1;
    return Ntp64{(ntp_seconds << 32) | frac_u};
}

std::chrono::system_clock::time_point
system_clock_from_ntp64(Ntp64 ntp) noexcept {
    using namespace std::chrono;
    const std::int64_t  ntp_seconds  = static_cast<std::int64_t>(ntp.seconds());
    const std::uint32_t frac         = ntp.fraction();
    std::int64_t        unix_seconds =
        ntp_seconds - static_cast<std::int64_t>(kNtpEpochOffset);
    if (unix_seconds < 0) unix_seconds = 0;
    // (frac * 1e6) / 2^32  — frac is 32-bit, so the product (≤ 4.3e15)
    // and quotient both fit in int64 even without __int128.
    const std::int64_t      frac_us   =
        (static_cast<std::int64_t>(frac) * 1000000LL) / 4294967296LL;
    return system_clock::time_point{
        seconds{unix_seconds} + microseconds{frac_us}};
}

// -----------------------------------------------------------------------------
// abs-send-time 24-bit ↔ monotonic microseconds (RFC 6051 §5).
// -----------------------------------------------------------------------------
//
//   abs_send_time_24 ticks at 65536 Hz; resolution 1e6 / 65536 ≈ 15.26 µs.
//   Wrap period 2^24 / 65536 = 256 s.
//
//   Encoding:
//     ticks = (monotonic_us * 65536) / 1e6            (24-bit, modular)
//   Decoding:
//     us    = ticks * 1e6 / 65536

std::uint32_t encode_abs_send_time_24(
        std::chrono::microseconds monotonic_us,
        Ntp64 monotonic_origin_ntp64) noexcept {
    (void)monotonic_origin_ntp64;   // origin kept in signature for callers
    // ticks = (monotonic_us / 1e6) * 65536
    //       + (monotonic_us % 1e6) * 65536 / 1e6   [via int64, not __int128]
    // The largest term is `unix_seconds * 65536` ≈ 6.5e13 (int64-safe for
    // any realistic monotonic duration); the wrap to 24 bits is applied
    // last.  Negative monotonic_us is clamped to 0 (pre-boot is not
    // representable on the wire anyway).
    const std::int64_t us64   = monotonic_us.count() < 0 ? 0 : monotonic_us.count();
    const std::int64_t us_int = us64 / 1000000LL;
    const std::int64_t us_rem = us64 % 1000000LL;
    const std::uint64_t ticks =
        static_cast<std::uint64_t>(us_int) * 65536ULL +
        (static_cast<std::uint64_t>(us_rem) * 65536ULL) / 1000000ULL;
    return static_cast<std::uint32_t>(ticks & 0xFFFFFFULL);
}

std::chrono::microseconds decode_abs_send_time_24(
        std::uint32_t raw_24,
        Ntp64 monotonic_origin_ntp64) noexcept {
    (void)monotonic_origin_ntp64;
    // raw_24 ≤ 2^24  ⇒  raw_24 * 1e6 ≤ 1.7e13  ⇒  fits in int64 without __int128.
    const std::int64_t us64 =
        (static_cast<std::int64_t>(raw_24 & 0xFFFFFFu) * 1000000LL) / 65536LL;
    return std::chrono::microseconds{us64};
}

// -----------------------------------------------------------------------------
// SenderReportTracker — implementation.
// -----------------------------------------------------------------------------
//
// Per-SSRC state carries:
//   - sr_ntp64            (RFC 3550 §6.4.1 sender info)
//   - sr_rtp_ts           (paired RTP-TS at the SR instant)
//   - clock_rate_hz
//
// We key per-SSRC state by the actual SSRC. The expected v1.0.0 working
// set is ≤ 4 SSRCs (audio + video per direction + a few renegotiation
// variants), so a small std::unordered_map is more than adequate.
//
// capture_ts_us_for() uses a *linear extrapolation* from the most-recent
// SR:
//   capture_ntp_us = sr_ntp_us
//                  + (rtp_ts - sr_rtp_ts) / clock_rate * 1e6
//
// The abs-send-time 24-bit value is exposed in the call signature so
// future v1.x can also produce a "this is the packet at sender wire-
// time T" lookup; for v1.0.0 we keep it informational.
//
// The function returns microseconds since the Unix epoch (1970-01-01),
// matching the on-the-wire NTP64 timestamp reduced by kNtpEpochOffset
// for the seconds field and added to the rtp_ts-derived delta.  This
// is the same anchoring convention WebRTC's RtpDepacketizer / Chrome's
// NetEq use; it lets callers subtract their own monotonic clock
// (cast to Unix-microseconds via boot-time) directly.

struct SenderReportTracker::Impl {
    std::unordered_map<std::uint32_t, SsrcTimelineState> by_ssrc;
};

SenderReportTracker::SenderReportTracker() noexcept : origin_(Ntp64{0}) {
    impl_ = new Impl{};
    impl_->by_ssrc.reserve(4);
}

SenderReportTracker::~SenderReportTracker() {
    if (impl_) {
        delete impl_;
        impl_ = nullptr;
    }
}

// Move-ctor is non-trivial because the heap-owned Impl needs to be
// transferred; the inline free-function `swap` provides the underlying
// mechanics.
SenderReportTracker::SenderReportTracker(SenderReportTracker&& other) noexcept
    : origin_(other.origin_), impl_(other.impl_) {
    other.impl_ = nullptr;
}

SenderReportTracker& SenderReportTracker::operator=(
        SenderReportTracker&& other) noexcept {
    if (this != &other) {
        if (impl_) delete impl_;
        origin_ = other.origin_;
        impl_   = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

void SenderReportTracker::set_monotonic_origin(Ntp64 origin_ntp64) noexcept {
    origin_ = origin_ntp64;
}

std::size_t SenderReportTracker::tracked_ssrc_count() const noexcept {
    return impl_ ? impl_->by_ssrc.size() : 0;
}

void SenderReportTracker::on_sender_report(
        const SenderReport& sr,
        std::int64_t local_recv_us_since_epoch) noexcept {
    if (!impl_) return;
    auto& state = impl_->by_ssrc[sr.ssrc];   // inserts default if new
    state.sr_ntp64           = Ntp64{sr.ntp_timestamp};
    state.sr_rtp_ts          = sr.rtp_timestamp;
    state.sr_local_recv_us   = local_recv_us_since_epoch;
    state.sr_abs_send_us     = 0;
    state.clock_rate_hz      = 90000;       // placeholder; a future v1.x
                                           //   revision will let the caller
                                           //   supply per-SSRC clock rate.
    if (state.clock_rate_hz == 0) state.clock_rate_hz = 90000;
    state.slope_nsec_per_rtp_ts =
        1'000'000'000LL / static_cast<std::int64_t>(state.clock_rate_hz);
    if (state.sr_count == 0) state.sr_count = 1;
}

std::optional<std::int64_t> SenderReportTracker::capture_ts_us_for(
        std::uint32_t ssrc,
        std::uint32_t rtp_ts,
        std::uint32_t abs_send_time_24,
        std::uint32_t clock_rate_hz,
        std::int64_t local_recv_us_since_epoch,
        std::int64_t* out_sender_recv_skew_us) noexcept {
    if (!impl_) return std::nullopt;
    (void)abs_send_time_24;
    (void)local_recv_us_since_epoch;
    if (out_sender_recv_skew_us) *out_sender_recv_skew_us = 0;

    auto it = impl_->by_ssrc.find(ssrc);
    if (it == impl_->by_ssrc.end() || it->second.sr_count == 0) {
        return std::nullopt;
    }
    const SsrcTimelineState& state = it->second;

    // Convert SR's NTP64 to microseconds since the UNIX epoch.
    // (frac * 1e6) / 2^32  fits in int64 without __int128 (frac ≤ 2^32).
    const std::int64_t  ntp_seconds  = static_cast<std::int64_t>(state.sr_ntp64.seconds());
    const std::uint32_t frac         = state.sr_ntp64.fraction();
    const std::int64_t  ntpf_micros  =
        (static_cast<std::int64_t>(frac) * 1000000LL) / 4294967296LL;
    // Convert NTP-anchored seconds to Unix-anchored seconds.  Clamp
    // pre-1970 SRs to 0 (callers needing pre-1970 should anchor against
    // an NTP-origin via system_clock_from_ntp64()).
    std::int64_t        unix_seconds_at_sr =
        ntp_seconds - static_cast<std::int64_t>(kNtpEpochOffset);
    if (unix_seconds_at_sr < 0) unix_seconds_at_sr = 0;
    const std::int64_t  sr_unix_us =
        unix_seconds_at_sr * 1'000'000LL + ntpf_micros;

    // Wrap detection: if rtp_ts < sr_rtp_ts by more than 2^31 we treat
    // it as a previous cycle; if the gap is <2^31 the SR is recent and
    // we extrapolate forward only. Refusing very old "previous-cycle"
    // SRs avoids stale-SR contamination under robust jitter buffer.
    std::int64_t rtp_diff_signed =
        static_cast<std::int64_t>(rtp_ts) -
        static_cast<std::int64_t>(state.sr_rtp_ts);
    if (rtp_diff_signed < 0) {
        if (static_cast<std::uint64_t>(-rtp_diff_signed) > kRtpTsHalfRange) {
            // Forward-truncating: rtp_ts is in this cycle, far ahead of
            // (a stale) SR — refuse.
            return std::nullopt;
        }
        // else: reasonable backward step (rtp_ts in prev cycle); clamp
        // to 0 to avoid returning times earlier than the SR.
        rtp_diff_signed = 0;
    }
    // delta_us = rtp_diff_signed * 1e6 / clock_rate_hz.
    // rtp_diff_signed ≤ 2^31 ⇒ product ≤ 2.1e15; fits in int64.
    const std::int64_t  delta_us =
        (rtp_diff_signed * 1'000'000LL) /
        static_cast<std::int64_t>(clock_rate_hz);
    const std::int64_t  capture_unix_us = sr_unix_us + delta_us;

    // Skew placeholder — v1.0.0 doesn't carry a paired "local
    // monotonic" at the SR instant *and* at the RTP instant; we leave
    // 0 for now. Callers needing skew should sample the local mono
    // clock at every SR + at every captured RTP and compute it out-
    // of-band. See ADR-017 §"Decision 4".
    if (out_sender_recv_skew_us) *out_sender_recv_skew_us = 0;
    return capture_unix_us;
}

} // namespace nimrtc::rtp
