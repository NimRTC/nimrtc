// modules/rtp/src/timeline.cpp
// -----------------------------------------------------------------------------
// Implementation of the cross-end frame timeline helpers declared in
// nimrtc/rtp/timeline.hpp.  See that header for the design notes (WebRTC
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
constexpr std::uint64_t kRtpTsModulus          = 1ULL << 32;

// Default media clock rate assumed for an SSRC whose rate was never
// configured via SenderReportTracker::set_clock_rate().  90000 is the
// video default; audio SSRCs MUST be configured explicitly or the
// extrapolation will be wrong by 90000/clock_rate.
constexpr std::uint32_t kDefaultClockRateHz = 90000;

// WebRTC abs-send-time wraps every 2^24 / 65536 = 256 s.  Expressed in
// microseconds so the wrap-arithmetic below stays in int64.
constexpr std::int64_t  kAbsSendTimeCycleUs = 256LL * 1'000'000LL;

// 24-bit mask / modulus of the abs-send-time counter.
constexpr std::uint32_t kAbsSendTimeMask     = 0x00FFFFFFu;
constexpr std::uint32_t kAbsSendTimeModulus = 1u << 24;

// How many (RTP-TS, abs-send-time) observations the tracker keeps per
// SSRC.  The ring only needs to span the gap between a Sender Report
// and the RTP packet whose RTP timestamp the SR refers to — in
// practice one packet interval (a few tens of ms), so 64 entries is a
// generous margin that also absorbs reordering.
constexpr std::size_t kMaxAstObservations = 64;

// How far (in media time) an observation may sit from the SR's RTP
// timestamp and still be used to derive the abs-send-time anchor.
// 250 ms is far longer than any plausible packet-to-SR gap.
constexpr std::int64_t kAstAnchorWindowMs = 250;

/** Signed modular difference of two 32-bit RTP timestamps.
 *  `a - b` computed modulo 2^32 and read as a signed value — the
 *  "modular difference" of RFC 3550 Appendix A.1. */
std::int64_t rtp_ts_signed_diff(std::uint32_t a, std::uint32_t b) noexcept {
    const std::uint32_t d = a - b;   // mod 2^32
    return (d <= 0x7FFFFFFFu)
               ? static_cast<std::int64_t>(d)
               : static_cast<std::int64_t>(d) -
                     static_cast<std::int64_t>(kRtpTsModulus);
}

/** Convert a 24-bit abs-send-time tick delta to microseconds, rounding
 *  to nearest.  The abs-send-time clock runs at 65536 Hz, so one tick
 *  is 1e6 / 65536 = 15.26 µs.  Rounding (rather than truncating
 *  toward zero) keeps the conversion symmetric around zero, which
 *  matters when the tick delta is negative. */
std::int64_t ast_ticks_to_us(std::int64_t ticks) noexcept {
    const std::int64_t num = ticks * 1'000'000LL;
    if (num >= 0) return (num + 32768LL) / 65536LL;
    return -((-num + 32768LL) / 65536LL);
}

/** Signed modular difference of two 24-bit abs-send-time counter values.
 *  The counter wraps every 2^24 ticks (256 s); the signed reading is
 *  the nearest representable offset, in [-2^23, 2^23) ticks
 *  (± 128 s). */
std::int64_t ast24_signed_diff(std::uint32_t a, std::uint32_t b) noexcept {
    const std::uint32_t d = (a - b) & kAbsSendTimeMask;   // mod 2^24
    return (d <= 0x7FFFFFu)
               ? static_cast<std::int64_t>(d)
               : static_cast<std::int64_t>(d) - (1LL << 24);
}

/** Reduce an NTP 64-bit timestamp to microseconds since the UNIX epoch.
 *
 *  The SR's seconds field is NTP-anchored (since 1900); Decision 1 requires
 *  the capture timestamp to be Unix-anchored (since 1970), so the
 *  `kNtpEpochOffset` is subtracted.  A pre-1970 SR (seconds below
 *  `kNtpEpochOffset` — malformed, but defensively guarded) clamps to 0
 *  seconds with its fraction preserved.
 *
 *  The fractional part is `(frac * 1e6) / 2^32`; frac is 32-bit so both
 *  product (≤ 4.3e15) and quotient fit in int64 without __int128. */
std::int64_t unix_us_from_ntp64(Ntp64 ntp) noexcept {
    const std::int64_t ntp_seconds = static_cast<std::int64_t>(ntp.seconds());
    const std::int64_t frac_us =
        (static_cast<std::int64_t>(ntp.fraction()) * kMicrosecondsPerSecond) /
        4294967296LL;
    std::int64_t unix_seconds =
        ntp_seconds - static_cast<std::int64_t>(kNtpEpochOffset);
    if (unix_seconds < 0) {
        // Preserve the fraction even when the seconds field clamps.
        return frac_us;
    }
    return unix_seconds * kMicrosecondsPerSecond + frac_us;
}
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
// abs-send-time 24-bit ↔ absolute capture instant (WebRTC extension).
// -----------------------------------------------------------------------------
//
//   abs_send_time_24 ticks at 65536 Hz; resolution 1e6 / 65536 ≈ 15.26 µs.
//   Wrap period 2^24 / 65536 = 256 s.
//
//   Encoding:
//     unix_us = unix_us_from_ntp64(origin) + monotonic_us
//     ticks   = (unix_us * 65536) / 1e6            (24-bit, modular)
//   Decoding:
//     us      = ticks * 1e6 / 65536                (0 .. 256 s — one cycle)
//
// The origin is what makes the value *comparable across ends*: without it
// the field is a bare delta from an arbitrary sender-chosen zero and only
// meaningful to a peer that shares that zero (which is why ADR-017
// Decision 2 keeps the SR authoritative).  Anchoring to Unix epoch lets
// `capture_ts_us_for()` line the field up against its SR-derived estimate.
//
// Backward compatibility: an unset origin (`Ntp64{0}`) contributes 0 µs,
// so the encoder degrades to the pre-anchoring behaviour (raw monotonic
// ticks) rather than producing a nonsense epoch-1900 value.

std::uint32_t encode_abs_send_time_24(
        std::chrono::microseconds monotonic_us,
        Ntp64 monotonic_origin_ntp64) noexcept {
    // An unset origin contributes nothing - see the note above.
    const std::int64_t unix_us =
        unix_us_from_ntp64(monotonic_origin_ntp64) + monotonic_us.count();
    // Negative means "before the origin"; the 24-bit field cannot express
    // that, so clamp to the wrap epoch (ticks 0).
    const std::int64_t us_int = unix_us < 0 ? 0 : unix_us / 1'000'000LL;
    const std::int64_t us_rem = unix_us < 0 ? 0 : unix_us % 1'000'000LL;
    // The largest term is `unix_us / 1e6 * 65536` - about 1.1e14 for a
    // current epoch (int64-safe for any realistic instant); the wrap to
    // 24 bits is applied last.
    const std::uint64_t ticks =
        static_cast<std::uint64_t>(us_int) * kAbsSendTimeClockRateHz +
        (static_cast<std::uint64_t>(us_rem) * kAbsSendTimeClockRateHz) /
            1'000'000ULL;
    return static_cast<std::uint32_t>(ticks & 0xFFFFFFULL);
}

std::chrono::microseconds decode_abs_send_time_24(
        std::uint32_t raw_24,
        Ntp64 monotonic_origin_ntp64) noexcept {
    // The origin is deliberately NOT applied here: the decode returns a
    // position *within one 256 s wrap cycle*, and choosing which cycle is
    // the caller's job - only the caller knows the absolute instant to
    // align against.  capture_ts_us_for() does that alignment.
    (void)monotonic_origin_ntp64;
    // raw_24 ≤ 2^24  ⇒  raw_24 * 1e6 ≤ 1.7e13  ⇒  fits in int64 without __int128.
    const std::int64_t us64 =
        (static_cast<std::int64_t>(raw_24 & 0xFFFFFFu) * 1'000'000LL) /
        static_cast<std::int64_t>(kAbsSendTimeClockRateHz);
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

// One observed (RTP timestamp, abs-send-time) pair, as seen on the wire.
struct AstObservation {
    std::uint32_t rtp_ts = 0;   // the packet's RTP timestamp
    std::uint32_t ast    = 0;   // the packet's raw 24-bit abs-send-time
};

// The abs-send-time anchor for an SSRC: the 24-bit counter value that
// was current at the instant of the stored Sender Report.
struct AstAnchor {
    bool        valid = false;
    std::uint32_t ticks = 0;   // 24-bit counter value at the SR instant
};

struct SenderReportTracker::Impl {
    std::unordered_map<std::uint32_t, SsrcTimelineState> by_ssrc;
    // Per-SSRC ring of observed (RTP-TS, abs-send-time) pairs.
    std::unordered_map<std::uint32_t, std::vector<AstObservation>> ast_obs;
    // Per-SSRC abs-send-time anchor derived from the stored SR.
    std::unordered_map<std::uint32_t, AstAnchor> ast_anchor;
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
    : origin_(other.origin_),
      ast_tolerance_us_(other.ast_tolerance_us_),
      impl_(other.impl_) {
    other.impl_ = nullptr;
}

SenderReportTracker& SenderReportTracker::operator=(
        SenderReportTracker&& other) noexcept {
    if (this != &other) {
        if (impl_) delete impl_;
        origin_            = other.origin_;
        ast_tolerance_us_  = other.ast_tolerance_us_;
        impl_              = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

void SenderReportTracker::set_monotonic_origin(Ntp64 origin_ntp64) noexcept {
    origin_ = origin_ntp64;
}

void SenderReportTracker::set_ast_agreement_tolerance_us(
        std::int64_t tolerance_us) noexcept {
    ast_tolerance_us_ = tolerance_us < 0 ? 0 : tolerance_us;
}

std::int64_t SenderReportTracker::ast_agreement_tolerance_us() const noexcept {
    return ast_tolerance_us_;
}

std::size_t SenderReportTracker::tracked_ssrc_count() const noexcept {
    return impl_ ? impl_->by_ssrc.size() : 0;
}

void SenderReportTracker::set_ssrc_clock_rate(
        std::uint32_t ssrc,
        std::uint32_t clock_rate_hz) noexcept {
    if (!impl_ || clock_rate_hz == 0) return;
    auto& state = impl_->by_ssrc[ssrc];
    state.clock_rate_hz = clock_rate_hz;
    state.slope_nsec_per_rtp_ts =
        1'000'000'000LL / static_cast<std::int64_t>(clock_rate_hz);
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
    // The media clock rate is *not* carried by the SR — it comes from
    // the SDP `a=rtpmap` line and must be supplied by the caller via
    // set_ssrc_clock_rate().  Only fall back to the 90 kHz video
    // default when the caller never configured this SSRC; hardcoding
    // 90000 unconditionally (the pre-v1.0.0 behaviour) produced wrong
    // capture times for 48 kHz audio.
    if (state.clock_rate_hz == 0) state.clock_rate_hz = kDefaultClockRateHz;
    state.slope_nsec_per_rtp_ts =
        1'000'000'000LL / static_cast<std::int64_t>(state.clock_rate_hz);
    // Running count of SRs seen for this SSRC.  Only the most-recent
    // SR is *retained* (the state is overwritten above) — the count is
    // a diagnostic, not a sample history.
    ++state.sr_count;

    // A new SR invalidates the previous abs-send-time anchor (it was
    // computed against the old SR's RTP timestamp).  Re-derive it from
    // the observation ring; the packet whose RTP timestamp matches the
    // new SR's may already be in the ring.
    try_anchor_ast(sr.ssrc);
}

std::optional<SsrcTimelineState> SenderReportTracker::state_for(
        std::uint32_t ssrc) const noexcept {
    if (!impl_) return std::nullopt;
    auto it = impl_->by_ssrc.find(ssrc);
    if (it == impl_->by_ssrc.end() || it->second.sr_count == 0) {
        return std::nullopt;
    }
    return it->second;
}

void SenderReportTracker::observe_abs_send_time(
        std::uint32_t ssrc,
        std::uint32_t rtp_ts,
        std::uint32_t abs_send_time_24) noexcept {
    if (!impl_) return;
    // A value above 2^24-1 cannot have come off the wire; ignore it
    // rather than poisoning the observation ring.
    if (abs_send_time_24 > kAbsSendTimeMask) return;

    auto& ring = impl_->ast_obs[ssrc];
    ring.push_back(AstObservation{rtp_ts, abs_send_time_24});
    if (ring.size() > kMaxAstObservations) {
        ring.erase(ring.begin());
    }
    // The observation may be the one whose RTP timestamp matches the
    // stored SR — in which case it establishes the anchor.
    try_anchor_ast(ssrc);
}

void SenderReportTracker::try_anchor_ast(std::uint32_t ssrc) noexcept {
    if (!impl_) return;

    auto sit = impl_->by_ssrc.find(ssrc);
    auto oit = impl_->ast_obs.find(ssrc);

    const bool have_sr =
        sit != impl_->by_ssrc.end() && sit->second.sr_count > 0;
    if (!have_sr || oit == impl_->ast_obs.end() || oit->second.empty()) {
        impl_->ast_anchor[ssrc] = AstAnchor{};
        if (sit != impl_->by_ssrc.end()) {
            sit->second.sr_abs_send_anchored = false;
            sit->second.sr_abs_send_us       = 0;
        }
        return;
    }

    SsrcTimelineState& st = sit->second;
    const std::vector<AstObservation>& obs = oit->second;
    const std::uint32_t rate =
        st.clock_rate_hz ? st.clock_rate_hz : kDefaultClockRateHz;

    // Preferred: an observation whose RTP timestamp is exactly the
    // SR's.  That packet was sent at the SR instant, so its
    // abs-send-time counter value *is* the anchor — no media-clock
    // correction needed.
    for (auto it = obs.rbegin(); it != obs.rend(); ++it) {
        if (it->rtp_ts == st.sr_rtp_ts) {
            impl_->ast_anchor[ssrc] = AstAnchor{true, it->ast};
            st.sr_abs_send_anchored = true;
            st.sr_abs_send_us =
                decode_abs_send_time_24(it->ast, Ntp64{0}).count();
            return;
        }
    }

    // Fallback: the nearest observation in *media time*, corrected by
    // the media-clock distance to the SR instant.  The correction spans
    // at most 250 ms of media time, so the media clock's rate error
    // contributes only a few tens of microseconds to the anchor.
    const std::int64_t window_ticks =
        (static_cast<std::int64_t>(rate) * kAstAnchorWindowMs) / 1000;
    const AstObservation* best = nullptr;
    std::int64_t best_dist = 0;
    for (const AstObservation& o : obs) {
        const std::int64_t d = rtp_ts_signed_diff(o.rtp_ts, st.sr_rtp_ts);
        const std::int64_t ad = d < 0 ? -d : d;
        if (ad > window_ticks) continue;
        if (!best || ad < best_dist) {
            best      = &o;
            best_dist = ad;
        }
    }
    if (!best) {
        impl_->ast_anchor[ssrc] = AstAnchor{};
        st.sr_abs_send_anchored = false;
        st.sr_abs_send_us       = 0;
        return;
    }

    // Media ticks from the observation to the SR instant, converted to
    // abs-send-time ticks (65536 Hz).  |media_ticks| <= 22500 for a
    // 90 kHz stream, so the product stays comfortably inside int64.
    const std::int64_t media_ticks =
        rtp_ts_signed_diff(st.sr_rtp_ts, best->rtp_ts);
    const std::int64_t ast_ticks_delta =
        (media_ticks * static_cast<std::int64_t>(kAbsSendTimeClockRateHz)) / rate;
    std::int64_t anchor_ticks =
        static_cast<std::int64_t>(best->ast) + ast_ticks_delta;
    anchor_ticks %= static_cast<std::int64_t>(kAbsSendTimeModulus);
    if (anchor_ticks < 0) {
        anchor_ticks += static_cast<std::int64_t>(kAbsSendTimeModulus);
    }

    impl_->ast_anchor[ssrc] =
        AstAnchor{true, static_cast<std::uint32_t>(anchor_ticks)};
    st.sr_abs_send_anchored = true;
    st.sr_abs_send_us =
        decode_abs_send_time_24(static_cast<std::uint32_t>(anchor_ticks),
                                Ntp64{0}).count();
}

// The 6-argument overload keeps the v1.0.0 semantics: the
// abs-send-time argument is accepted for wire compatibility and
// ignored (no observation is recorded, no refinement is applied).
std::optional<std::int64_t> SenderReportTracker::capture_ts_us_for(
        std::uint32_t ssrc,
        std::uint32_t rtp_ts,
        std::uint32_t abs_send_time_24,
        std::uint32_t clock_rate_hz,
        std::int64_t local_recv_us_since_epoch,
        std::int64_t* out_sender_recv_skew_us) noexcept {
    return capture_ts_us_for(ssrc, rtp_ts, abs_send_time_24,
                             /*has_abs_send_time=*/false, clock_rate_hz,
                             local_recv_us_since_epoch, out_sender_recv_skew_us);
}

// The 7-argument overload makes the abs-send-time field participate in
// the estimate (ADR-017 "Decision 2").
std::optional<std::int64_t> SenderReportTracker::capture_ts_us_for(
        std::uint32_t ssrc,
        std::uint32_t rtp_ts,
        std::uint32_t abs_send_time_24,
        bool has_abs_send_time,
        std::uint32_t clock_rate_hz,
        std::int64_t local_recv_us_since_epoch,
        std::int64_t* out_sender_recv_skew_us) noexcept {
    if (!impl_) return std::nullopt;
    (void)local_recv_us_since_epoch;
    if (out_sender_recv_skew_us) *out_sender_recv_skew_us = 0;

    // Record the (rtp_ts, abs_send_time_24) pair *before* reading the
    // per-SSRC state: recording may establish (or re-establish) the
    // abs-send-time anchor against the stored SR, and the refinement
    // below reads that anchor.  The 6-argument overload passes
    // `has_abs_send_time = false`, so it never records anything and
    // therefore keeps the exact v1.0.0 behaviour.
    if (has_abs_send_time) {
        observe_abs_send_time(ssrc, rtp_ts, abs_send_time_24);
    }

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

    // RFC 3550 §8.1 / Appendix A.1: the RTP timestamp is a 32-bit
    // modulo counter.  The difference between two timestamps must be
    // computed modulo 2^32 and then read as a signed value in
    // [-2^31, 2^31) — the "modular difference".  Subtracting the raw
    // uint32 values in 64-bit arithmetic (the pre-v1.0.0 behaviour)
    // gets the wrap case wrong: an SR at rtp_ts = 0xFFFFFF00 and a
    // packet at rtp_ts = 0x00000100 differ by -4294966784 in that
    // arithmetic, so the old code rejected outright a packet that is
    // in fact only 512 ticks *after* the SR.
    const std::uint32_t diff_u32 = rtp_ts - state.sr_rtp_ts;   // mod 2^32
    const std::int64_t  rtp_diff_signed =
        (diff_u32 <= 0x7FFFFFFFu)
            ? static_cast<std::int64_t>(diff_u32)
            : static_cast<std::int64_t>(diff_u32) -
                  static_cast<std::int64_t>(kRtpTsModulus);

    // Resolve the media clock rate: an explicit per-call value
    // overrides the configured per-SSRC rate; 0 means "use the one
    // configured via set_ssrc_clock_rate()".
    const std::int64_t rate_hz =
        (clock_rate_hz != 0)
            ? static_cast<std::int64_t>(clock_rate_hz)
            : static_cast<std::int64_t>(state.clock_rate_hz);
    if (rate_hz <= 0) return std::nullopt;

    std::int64_t forward_ticks = rtp_diff_signed;
    if (forward_ticks < 0) {
        // The packet was captured *before* the SR we hold.  We keep only
        // the most recent SR per SSRC, so there is no anchor for a
        // pre-SR packet.  Tolerate ordinary jitter-buffer reordering
        // (up to one second of media time) by clamping to the SR
        // instant; refuse anything older.  A large negative difference
        // is either a genuinely stale packet or the far side of the
        // 2^31 ambiguity window, and extrapolating from it would
        // fabricate a timestamp.
        const std::int64_t reorder_ticks = rate_hz;   // 1 s of media time
        if (-forward_ticks > reorder_ticks) {
            return std::nullopt;
        }
        forward_ticks = 0;
    }
    // delta_us = forward_ticks * 1e6 / rate_hz.
    // forward_ticks ≤ 2^31 ⇒ product ≤ 2.1e15; fits in int64.
    const std::int64_t  delta_us =
        (forward_ticks * 1'000'000LL) / rate_hz;
    std::int64_t  capture_unix_us = sr_unix_us + delta_us;

    // -----------------------------------------------------------------
    // abs-send-time refinement (ADR-017 §"Decision 2").
    // -----------------------------------------------------------------
    //
    // The estimate above is a *media-clock* extrapolation: it inherits
    // the media clock's rate error, so it drifts over a long session.
    // The abs-send-time counter is driven by the sender's own wall
    // clock; the difference between two of its readings is therefore a
    // direct measurement of the sender's elapsed time that does *not*
    // inherit that error.
    //
    // The refinement works in the *delta* domain:
    //
    //   anchor  = the abs-send-time counter value that was current at
    //             the stored SR's RTP timestamp (see try_anchor_ast)
    //   elapsed = current abs-send-time value − anchor   (mod 2^24)
    //   refined = sr_unix_us + elapsed
    //
    // The modular difference is read as a signed value in
    // [-2^23, 2^23) ticks (± 128 s), which comfortably covers the
    // gap between an SR and any packet we still hold that SR for — the
    // 256 s wrap is therefore resolved without ambiguity.
    //
    // The refinement is applied only when:
    //   (a) the caller told us the packet really carries the field,
    //   (b) the tolerance is non-zero (0 disables refinement entirely),
    //   (c) the wire value is a legal 24-bit value,
    //   (d) the tracker could pair the SR's RTP timestamp with an
    //       observed abs-send-time value (the anchor), and
    //   (e) the two estimates agree within the tolerance.
    //
    // Condition (e) is the safety net: when a peer restarts its
    // 256 s counter the anchor becomes suspect, the two estimates
    // disagree wildly, and the SR-based value wins.
    //
    // Note on reordered packets: for a packet that predates the SR the
    // base estimate is *clamped* to the SR instant (see above), while
    // the abs-send-time delta recovers the true pre-SR instant.  The
    // two therefore disagree by the reordering distance, which is
    // normally far below the 1 s tolerance, so the refinement wins and
    // the packet gets its real capture time instead of a clamped one.
    if (has_abs_send_time && ast_tolerance_us_ > 0 &&
        abs_send_time_24 <= kAbsSendTimeMask && state.sr_abs_send_anchored) {
        auto ait = impl_->ast_anchor.find(ssrc);
        if (ait != impl_->ast_anchor.end() && ait->second.valid) {
            const std::int64_t elapsed_ticks =
                ast24_signed_diff(abs_send_time_24, ait->second.ticks);
            const std::int64_t elapsed_us = ast_ticks_to_us(elapsed_ticks);
            const std::int64_t refined_us = sr_unix_us + elapsed_us;
            const std::int64_t disagreement = refined_us - capture_unix_us;
            const std::int64_t abs_disagreement =
                disagreement < 0 ? -disagreement : disagreement;
            if (abs_disagreement <= ast_tolerance_us_) {
                capture_unix_us = refined_us;
            }
        }
    }

    // Skew placeholder — v1.0.0 doesn't carry a paired "local
    // monotonic" at the SR instant *and* at the RTP instant; we leave
    // 0 for now. Callers needing skew should sample the local mono
    // clock at every SR + at every captured RTP and compute it out-
    // of-band. See ADR-017 §"Decision 4".
    if (out_sender_recv_skew_us) *out_sender_recv_skew_us = 0;
    return capture_unix_us;
}

} // namespace nimrtc::rtp
