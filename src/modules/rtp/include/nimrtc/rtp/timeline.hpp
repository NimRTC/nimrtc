/**
 * @file nimrtc/rtp/timeline.hpp
 * @brief Cross-end frame timeline helpers (RFC 3550 §6.4.1 / RFC 6051 §5).
 *
 * ## What this module owns
 *
 *   1. The sender-side bookkeeping needed to pair (NTP wall-clock,
 *      RTP media clock) inside an RTCP Sender Report (SR).
 *      `SenderReportTracker` plays the *receiver* role: it ingests
 *      incoming SRs and answers "what was the sender's wall-clock
 *      capture time of this RTP packet?" by extrapolating linearly
 *      from the most-recent SR.  See ADR-017 for the full design and
 *      for the completion gaps this seam still carries.
 *
 *   2. The on-wire timestamp format we use. RFC 6051
 *      `abs-send-time` (24-bit unsigned at 65536 Hz, 256 s wrap)
 *      is the standard cross-implementation-compatible variant
 *      emitted by Chromium / WebRTC / Firefox; we bind the
 *      extension URI `http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time`
 *      (Chrome's chosen URI for RFC 6051) so the wire is
 *      interoperable with the existing chrome_opus_offer fixture
 *      used in interop/.
 *
 *   3. The NTP 64-bit ↔ std::chrono::system_clock conversion. RFC
 *      5905 §6 — seconds since 1900-01-01 00:00:00 UTC. We use
 *      a single 64-bit `Ntp64` value that carries the full
 *      seconds + 32-bit fraction (RFC 3550 §6.4.1 only carries
 *      the middle 32 of it; we store 64 so SR/RR report-blocks can
 *      round-trip losslessly).
 *
 * ## Non-goals
 *
 *   - Drift-tolerant long-running alignment (Kalman / EWMA) and
 *     multi-SR slope estimation: we extrapolate linearly from a
 *     single SR and leave the filter for v1.x.  The linear model is
 *     correct for short sessions (<5 min typical) and degrades
 *     gracefully beyond that.
 *   - Audio sample-level sync: out of scope (PCM path needs a
 *     SRTP-indexed adapter; see ADR-017 §"Audio carry").
 *
 * ## Status
 *
 *   This header is a *seam*, not a finished feature.  As of v1.0.0
 *   nothing outside the timeline's own tests consumes
 *   `SenderReportTracker`, and no engine path acts on
 *   `Profile::timeline_enabled`.  The remaining work — per-SSRC
 *   clock rate, RTP-TS wrap handling, abs-send-time participation,
 *   extmap negotiation, SR scheduling, engine wiring — is
 *   enumerated in ADR-017 §"Decision 5".
 *
 * ## Stability
 *
 *   The structs in this header are NIMRTC_API_STABLE — same
 *   freeze rules as the rest of the modules/rtp surface. v1.0.0
 *   feature gate is `Profile::timeline_enabled`; when the feature
 *   is completed, the engine MUST NOT emit abs-send-time or
 *   schedule SR sends while the flag is false.
 */
#pragma once

#include <nimrtc/core/api_version.hpp>
#include <cstdint>
#include <optional>
#include <chrono>

#include <nimrtc/rtp/packet.hpp>

namespace nimrtc::rtp {

// -----------------------------------------------------------------------------
// Protocol constants
// -----------------------------------------------------------------------------

/** Canonical RFC 6051 abs-send-time extension URI (Chrome / WebRTC).
 *  24-bit unsigned timestamp at `kAbsSendTimeClockRateHz` = 65536 Hz.
 *  Wrap period: 2^24 / 65536 = 256 s (~4.3 min). */
inline constexpr std::string_view kAbsSendTimeUri =
    "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time";

/** Clock rate used to interpret the 24-bit abs-send-time field.
 *  Resolution: 1 / 65536 s ≈ 15.26 µs — finer than any video / audio
 *  pipeline we care about, coarse enough to fit 24 bits in the
 *  extension element. */
inline constexpr std::uint32_t kAbsSendTimeClockRateHz = 65536;

/** Local extension id we advertise when `timeline_enabled = true` and
 *  no peer id has been negotiated. Chrome uses 4 historically; 4 is
 *  also free of the IDs we use for ssrc-audio-level (1) and
 *  transport-wide CC (5).  Use 4 when we are the offerer and no
 *  id was taken by `a=extmap` parsing. */
inline constexpr std::uint16_t kAbsSendTimeDefaultLocalId = 4;

/** RFC 5905 NTP epoch offset: NTP seconds = UNIX seconds + kNtpEpochOffset. */
inline constexpr std::uint64_t kNtpEpochOffset = 2208988800ULL;

/** Default SR send cadence (matches WebRTC default; configurable via
 *  EngineConfig). One second — frequent enough for ~5 ms alignment
 *  even under heavy NTP jitter, cheap enough to ignore on the wire. */
inline constexpr std::uint32_t kSrPeriodMs = 1000;

// -----------------------------------------------------------------------------
// Ntp64 — 64-bit NTP timestamp (RFC 5905 §6).
// -----------------------------------------------------------------------------
//   The upper 32 bits are integer seconds since 1900-01-01 UTC.
//   The lower 32 bits are fractional seconds (units of 1 / 2^32 s).
//   RTCP SR carries the full 64; the middle 32 (i.e. bits 16..47)
//   is what RR report-blocks copy for round-trip measurement.

struct NIMRTC_API_STABLE Ntp64 {
    std::uint64_t value = 0;     ///< raw 64-bit NTP timestamp

    constexpr Ntp64() noexcept = default;
    constexpr explicit Ntp64(std::uint64_t v) noexcept : value(v) {}

    /// Integer seconds since NTP epoch.
    constexpr std::uint32_t seconds() const noexcept {
        return static_cast<std::uint32_t>(value >> 32);
    }

    /// Fractional seconds in 1 / 2^32 units.
    constexpr std::uint32_t fraction() const noexcept {
        return static_cast<std::uint32_t>(value & 0xFFFFFFFFULL);
    }

    /// RFC 3550 §6.4.1 "middle 32" — bits 16..47 of the 64-bit value.
    constexpr std::uint32_t mid_32() const noexcept {
        return static_cast<std::uint32_t>((value >> 16) & 0xFFFFFFFFULL);
    }
};

/** Convert std::chrono::system_clock::now() into NTP 64-bit. */
NIMRTC_API_STABLE Ntp64 ntp64_from_system_clock() noexcept;

/** Convert an explicit system_clock time_point into NTP 64-bit. */
NIMRTC_API_STABLE Ntp64 ntp64_from(std::chrono::system_clock::time_point t) noexcept;

/** Convert NTP 64-bit back to system_clock::time_point. Used in tests;
 *  production receivers should use `SenderReportTracker::capture_ts_us_*`
 *  to avoid leap-second handling for active sessions. */
NIMRTC_API_STABLE std::chrono::system_clock::time_point
    system_clock_from_ntp64(Ntp64 ntp) noexcept;

/** Pack a `SteadyClock::time_point` (monotonic microseconds since boot)
 *  into the 24-bit abs-send-time value the wire carries. The value
 *  wraps modulo 2^24 (every 256 s at 65536 Hz) — it is deliberately
 *  not an epoch, so it must be anchored to a wider clock before use.
 *
 *  NOTE: `monotonic_origin_ntp64` is currently accepted but not
 *  applied — the encoder emits raw monotonic ticks and the origin is
 *  reserved for the v1.1 origin-anchored form.  See ADR-017
 *  §"Decision 5". */
NIMRTC_API_STABLE std::uint32_t
    encode_abs_send_time_24(std::chrono::microseconds monotonic_us,
                           Ntp64 monotonic_origin_ntp64) noexcept;

/** Inverse of `encode_abs_send_time_24`. Converts the 24-bit wire value
 *  back to sender monotonic microseconds *relative to the abs-send-time
 *  wrap epoch* — the origin is not applied, so the result is only
 *  meaningful modulo 256 s. The exact value isn't observable — the wire
 *  carries 24 bits at 65536 Hz, so its quantization is
 *  `1e6 / 65536 ≈ 15.26 µs`.
 *
 *  NOTE: `monotonic_origin_ntp64` is currently accepted but not
 *  applied.  See ADR-017 §"Decision 5". */
NIMRTC_API_STABLE std::chrono::microseconds
    decode_abs_send_time_24(std::uint32_t raw_24,
                            Ntp64 monotonic_origin_ntp64) noexcept;

// -----------------------------------------------------------------------------
// SenderReportTracker — receiver-side NTP ↔ RTP-TS mapping (RFC 3550 §4.5,
// §8.1) and per-SSRC abs-send-time → wall-clock lookup.
//
// Algorithm (v1.0.0 — single-SR linear extrapolation):
//
//   For each remote SSRC we keep the most-recent SR only:
//     - sr_ntp64          (RFC 3550 §6.4.1 sender info)
//     - sr_rtp_ts         (paired RTP-TS at the SR instant)
//
//   For any incoming RTP with rtp_ts X:
//     capture_us = sr_ntp_us + (X − sr_rtp_ts) × 1e6 / clock_rate_hz
//
//   NOT implemented in v1.0.0 — see ADR-017 §"Decision 5":
//     - slope estimation across ≥ 2 SR samples (there is no SR history;
//       `slope_nsec_per_rtp_ts` is a derived constant, not a fit)
//     - combining abs-send-time with the SR (the field is accepted for
//       wire compatibility but currently unused; the 24-bit value wraps
//       every 256 s so it cannot anchor a wall clock on its own)
//     - EWMA / Kalman smoothing (v1.1+; the linear model above is the
//       deliberate v1.0.0 trade-off)
//
//   `local_recv_us_since_epoch` is recorded per SR so callers can
//   measure the estimate's error out-of-band; the tracker itself does
//   not use it.
// -----------------------------------------------------------------------------

struct NIMRTC_API_STABLE SsrcTimelineState {
    Ntp64          sr_ntp64           = Ntp64{0};
    std::uint32_t  sr_rtp_ts          = 0;
    std::int64_t   sr_local_recv_us   = 0;
    // Not populated in v1.0.0 — reserved for the abs-send-time ↔ SR
    // cross-check.  See ADR-017 §"Decision 5".
    std::int64_t   sr_abs_send_us     = 0;
    // Nanoseconds per RTP timestamp tick.  In v1.0.0 this is a
    // constant derived from `clock_rate_hz`, NOT a fitted slope — there
    // is no multi-SR history.  See ADR-017 §"Decision 5".
    std::int64_t   slope_nsec_per_rtp_ts = 0;
    std::uint32_t  clock_rate_hz         = 90000;
    // Number of SRs ingested for this SSRC.  In v1.0.0 this saturates
    // at 1 (the tracker keeps only the most-recent SR) — it is not a
    // running count.  See ADR-017 §"Decision 5".
    std::uint32_t  sr_count              = 0;
};

class NIMRTC_API_STABLE SenderReportTracker {
public:
    SenderReportTracker() noexcept;
    ~SenderReportTracker();

    SenderReportTracker(const SenderReportTracker&) = delete;
    SenderReportTracker& operator=(const SenderReportTracker&) = delete;
    SenderReportTracker(SenderReportTracker&& other) noexcept;
    SenderReportTracker& operator=(SenderReportTracker&& other) noexcept;

    /** Origin the sender's monotonic clock is anchored to. Engine passes
     *  its own boot-time-equivalent (SteadyClock::now() at construction).
     *
     *  NOTE: in v1.0.0 this value is stored but not consumed by any
     *  lookup — `capture_ts_us_for` resolves capture time from the SR
     *  alone.  Reserved for the v1.1 origin-anchored form.  See
     *  ADR-017 §"Decision 5". */
    void set_monotonic_origin(Ntp64 origin_ntp64) noexcept;

    Ntp64 monotonic_origin() const noexcept { return origin_; }

    /** Ingest an inbound Sender Report. `local_recv_us_since_epoch` is
     *  the monotonic-clock microseconds at which the engine *received*
     *  the SR packet (before any queueing). Recorded per SSRC so a
     *  caller can measure the estimate's error out-of-band; the
     *  tracker itself does not use it. */
    void on_sender_report(const SenderReport& sr,
                          std::int64_t local_recv_us_since_epoch) noexcept;

    /** Number of SSRCs tracked (mostly for tests / stats). */
    std::size_t tracked_ssrc_count() const noexcept;

    /** Returns the reconstructed sender-wall-clock capture timestamp
     *  (microseconds, UNIX-anchored) for an inbound RTP packet
     *  identified by SSRC and carrying `abs_send_time_24` as the
     *  24-bit abs-send-time field. Returns std::nullopt when:
     *    - No SR has been seen for this SSRC yet; or
     *    - the RTP timestamp falls in a previous wrap cycle relative
     *      to the stored SR (see ADR-017 §"Decision 5" — the wrap
     *      handling is known-incomplete in v1.0.0).
     *
     *  IMPORTANT: this is a *microsecond* timestamp on the sender's
     *  wall clock (1970 Unix epoch). Callers anchor it to their own
     *  monotonic clock via `set_monotonic_origin()`; for end-to-end
     *  capture-vs-display alignment in agent pipelines, prefer storing
     *  the *delta* (sender - local_recv_mono) rather than the absolute
     *  Unix number.
     *
     *  SRs with ntp_timestamp seconds below `kNtpEpochOffset` (pre-1970
     *  sender clock — should not happen on the wire but defensively
     *  guarded here) are clamped to 0 in the seconds field; the
     *  fractional microsecond component is preserved.
     *
     *  Parameters:
     *    - ssrc / rtp_ts : identify the packet and its media-clock
     *      position; the timestamp is extrapolated from the stored SR.
     *    - abs_send_time_24 : accepted for wire compatibility; NOT
     *      used by the v1.0.0 estimate (see ADR-017 §"Decision 2").
     *    - clock_rate_hz : the media clock rate to extrapolate against.
     *    - local_recv_us_since_epoch : accepted for symmetry with
     *      `on_sender_report`; not used by the estimate.
     *
     *  Out-params:
     *    - sender_recv_skew_us (optional; pass nullptr to discard):
     *      reserved for v1.1 — always 0 in v1.0.0.  See ADR-017
     *      §"Decision 4". */
    std::optional<std::int64_t> capture_ts_us_for(
        std::uint32_t ssrc,
        std::uint32_t rtp_ts,
        std::uint32_t abs_send_time_24,
        std::uint32_t clock_rate_hz,
        std::int64_t local_recv_us_since_epoch,
        std::int64_t* out_sender_recv_skew_us) noexcept;

private:
    Ntp64 origin_;
    struct Impl;
    Impl* impl_ = nullptr;

    // Free swap() needs to be a friend so it can read/write the pImpl
    // pointer and the NTP origin.  The body lives below the class as
    // a namespace-scope inline function (matches the rest of the
    // modules/rtp API style for free helpers).
    friend inline void swap(SenderReportTracker& a,
                            SenderReportTracker& b) noexcept;
};

// Free function (trivially movable) — defined for the move-ctor.
inline void swap(SenderReportTracker& a,
                 SenderReportTracker& b) noexcept {
    using std::swap;
    swap(a.origin_, b.origin_);
    swap(a.impl_,   b.impl_);
}

} // namespace nimrtc::rtp
