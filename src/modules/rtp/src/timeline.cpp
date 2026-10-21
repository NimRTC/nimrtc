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
    const __int128      frac         =
        (static_cast<__int128>(unix_micros) * kNtpFracMicro) / 1000000LL;
    std::uint64_t       frac_u       = static_cast<std::uint64_t>(frac);
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
    const __int128      frac_us128   =
        (static_cast<__int128>(frac) * 1000000LL) / 4294967296LL;
    return system_clock::time_point{
        seconds{unix_seconds} + microseconds{static_cast<std::int64_t>(frac_us128)}};
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
    const __int128 ticks128 =
        (static_cast<__int128>(monotonic_us.count()) * 65536LL) / 1000000LL;
    return static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(ticks128) & 0xFFFFFFULL);
}

std::chrono::microseconds decode_abs_send_time_24(
        std::uint32_t raw_24,
        Ntp64 monotonic_origin_ntp64) noexcept {
    (void)monotonic_origin_ntp64;
    const __int128 ticks = static_cast<__int128>(raw_24 & 0xFFFFFFu);
    const __int128 us128 = (ticks * 1000000LL) / 65536LL;
    return std::chrono::microseconds{static_cast<std::int64_t>(us128)};
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
// The function returns microseconds since the NTP epoch (i.e. the same
// anchoring the SR's ntp_timestamp carries). Callers convert to a
// monotonic clock (their own or the peer's) by subtracting
// monotonic_origin_us (see `system_clock_from_ntp64` for an end-to-end
// path).

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
        std::int64_t& out_sender_recv_skew_us) noexcept {
    if (!impl_) return std::nullopt;
    (void)abs_send_time_24;
    (void)local_recv_us_since_epoch;
    out_sender_recv_skew_us = 0;

    auto it = impl_->by_ssrc.find(ssrc);
    if (it == impl_->by_ssrc.end() || it->second.sr_count == 0) {
        return std::nullopt;
    }
    const SsrcTimelineState& state = it->second;

    // Convert SR's NTP64 to microseconds since the NTP epoch.
    const std::int64_t  ntp_seconds  = static_cast<std::int64_t>(state.sr_ntp64.seconds());
    const std::uint32_t frac         = state.sr_ntp64.fraction();
    const __int128      ntp_micros128 =
        (static_cast<__int128>(frac) * 1000000LL) / 4294967296LL;
    const std::int64_t  sr_ntp_us =
        ntp_seconds * 1'000'000LL +
        static_cast<std::int64_t>(ntp_micros128);

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
    const __int128      delta_us128 =
        (static_cast<__int128>(rtp_diff_signed) * 1'000'000LL) /
        static_cast<__int128>(clock_rate_hz);
    const std::int64_t  delta_us = static_cast<std::int64_t>(delta_us128);
    const std::int64_t  capture_ntp_us = sr_ntp_us + delta_us;

    // Skew placeholder — v1.0.0 doesn't carry a paired "local
    // monotonic" at the SR instant *and* at the RTP instant; we leave
    // 0 for now. Callers needing skew should sample the local mono
    // clock at every SR + at every captured RTP and compute it out-
    // of-band. See docs/adr/ADR-016 §"Diagnostic skew".
    out_sender_recv_skew_us = 0;
    return capture_ntp_us;
}

} // namespace nimrtc::rtp
