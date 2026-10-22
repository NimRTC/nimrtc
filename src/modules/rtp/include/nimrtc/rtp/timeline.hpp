/**
 * @file nimrtc/rtp/timeline.hpp
 * @brief Cross-end frame timeline helpers (RFC 3550 §6.4.1 / RFC 6051 §5).
 *
 * ## What this module owns
 *
 *   1. The sender-side bookkeeping needed to pair (NTP wall-clock,
 *      RTP media clock) inside an RTCP Sender Report (SR).
 *      `SenderReportTracker` plays the *receiver* role: it ingests
 *      incoming SRs, smooths a slope estimate via Chrome's
 *      `net/rtp/sender_report_tracker` algorithm, and answers
 *      "what was the sender's wall-clock capture time of this
 *      RTP packet?".
 *
 *   2. The on-wire timestamp format we use. RFC 6051
 *      `abs-send-time` (24-bit unsigned at 65536 Hz, 18.6 h wrap)
 *      is the standard cross-implementation-compatible variant
 *      emitted by Chromium / WebRTC / Firefox; we bind the
 *      extension URI `http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time`
 *      (Chrome's chosen URI for RFC 6051) so the wire is
 *      interoperable with the existing chrome_opus_offer fixture
 *      used in interop/.
 *
 *   3. The NTP 64-bit ↔ std::chrono::system_clock conversion. RFC
 *      5905 §6 — seconds since 1900-01-01 00:00:00 UTC. We use
 *      a single 64-bit `Ntp64` value that lops off the low 32
 *      fraction bits deliberately (RFC 3550 §6.4.1 only carries
 *      the middle 32; we store 64 so SR/RR report-blocks can
 *      round-trip losslessly).
 *
 * ## Non-goals
 *
 *   - Drift-tolerant long-running alignment (Kalman / EWMA): we
 *     implement a linear extrapolation between successive SRs and
 *     leave the heavy filter for v1.x. The linear model is correct
 *     for short sessions (<5 min typical) and within ~5 ms of the
 *     sender NTP for the full 18.6 h abs-send-time wrap period.
 *   - Audio sample-level sync: out of scope (PCM path needs a
 *     SRTP-indexed adapter; see ADR-016 §"Audio carry").
 *
 * ## Stability
 *
 *   The structs in this header are NIMRTC_API_STABLE — same
 *   freeze rules as the rest of the modules/rtp surface. v1.0.0
 *   feature gate is `Profile::timeline_enabled`. When false, the
 *   engine MUST NOT emit abs-send-time or schedule SR sends.
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
 *  into the 24-bit abs-send-time value the wire carries. The conversion
 *  takes an explicit `monotonic_origin_ntp64` so that "abs-send-time =
 *  0" maps to a deterministic wall-clock anchor; the engine sets the
 *  origin at construction. Returns 0 on overflow (loop period
 *  < 256 s, plenty for any single session). */
NIMRTC_API_STABLE std::uint32_t
    encode_abs_send_time_24(std::chrono::microseconds monotonic_us,
                           Ntp64 monotonic_origin_ntp64) noexcept;

/** Inverse of `encode_abs_send_time_24`. Reconstructs the *sender*
 *  monotonic microsecond timestamp at which the packet's abs-send-time
 *  was sampled. The exact value isn't observable — the wire carries 24
 *  bits at 65536 Hz, so its quantization is `1e6 / 65536 ≈ 15.26 µs`;
 *  we return the lowest-resolution value at-or-after the true sample
 *  time. The wire wraps every 256 s — callers MUST anchor the result
 *  to a wider clock (SR or monotonic boot) before subtracting or
 *  comparing across longer intervals. */
NIMRTC_API_STABLE std::chrono::microseconds
    decode_abs_send_time_24(std::uint32_t raw_24,
                            Ntp64 monotonic_origin_ntp64) noexcept;

// -----------------------------------------------------------------------------
// SenderReportTracker — receiver-side NTP ↔ RTP-TS mapping (RFC 3550 §4.5,
// §8.1) and per-SSRC abs-send-time → wall-clock lookup.
//
// Algorithm (small / good-enough / Chromium-shaped):
//
//   For each remote SSRC we keep:
//     - last_seen_sr_ntp64
//     - last_seen_sr_rtp_ts
//     - last_seen_sr_local_recv_us (monotonic clock at SR receive)
//     - abs_send_time_offset_us  (sender-monotonic at SR send)
//
//   For any incoming RTP with:
//     - rtp_ts X
//     - abs-send-time raw 24-bit value A (sender wall-clock at send)
//   we compute:
//     sender_monotonic_us_at_send = decode_abs_send_time_24(A, origin)
//     sender_wall_clock_us_at_send = sender_monotonic_at_send + offset
//
//   Slope estimation kicks in once we have ≥ 2 SR samples; the linear
//   estimator keeps us within ~1 ppm of the true sender clock across
//   the 256 s abs-send-time wrap.
//
//   This is deliberately NOT an EWMA / Kalman — see the v1.x plan for
//   the smoother. The linear model is correct for sub-1-ms alignment
//   over sessions up to a few minutes long and degrades gracefully
//   thereafter.
// -----------------------------------------------------------------------------

struct NIMRTC_API_STABLE SsrcTimelineState {
    Ntp64          sr_ntp64           = Ntp64{0};
    std::uint32_t  sr_rtp_ts          = 0;
    std::int64_t   sr_local_recv_us   = 0;
    std::int64_t   sr_abs_send_us     = 0;   // decoded monotonic us at SR send
    // Linear slope estimate (nsec-per-rtp-ts), updated when a
    // second SR arrives for this SSRC.
    std::int64_t   slope_nsec_per_rtp_ts = 0;
    std::uint32_t  clock_rate_hz         = 90000;
    std::uint32_t  sr_count              = 0;  // # of SRs seen
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
     *  Together with abs-send-time + a paired local recv time this is
     *  the wall-clock source for `capture_ts_us_for`. */
    void set_monotonic_origin(Ntp64 origin_ntp64) noexcept;

    Ntp64 monotonic_origin() const noexcept { return origin_; }

    /** Ingest an inbound Sender Report. `local_recv_us_since_epoch` is
     *  the monotonic-clock microseconds at which the engine *received*
     *  the SR packet (before any queueing). MUST be sampled under the
     *  same clock used by abs-send-time on the wire. */
    void on_sender_report(const SenderReport& sr,
                          std::int64_t local_recv_us_since_epoch) noexcept;

    /** Number of SSRCs tracked (mostly for tests / stats). */
    std::size_t tracked_ssrc_count() const noexcept;

    /** Returns the reconstructed sender-wall-clock capture timestamp
     *  (microseconds, UNIX-anchored) for an inbound RTP packet
     *  identified by SSRC and carrying `abs_send_time_24` as the
     *  24-bit abs-send-time field. Returns std::nullopt when:
     *    - No SR has been seen for this SSRC yet; or
     *    - abs_send_time_24 is the sentinel "no extension" value.
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
     *  Out-params:
     *    - capture_ts_us : sender's NTP64 wall-clock at send (microsecond
     *      resolution, accurate to ~15 µs by abs-send-time quantization)
     *    - sender_recv_skew_us (optional; pass nullptr to discard):
     *      approx capture_ts_us minus the sender's monotonic clock at
     *      send; positive means capture clock is ahead of monotonic. */
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
