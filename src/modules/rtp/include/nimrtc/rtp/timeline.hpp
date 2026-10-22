/**
 * @file nimrtc/rtp/timeline.hpp
 * @brief Cross-end frame timeline helpers (RFC 3550 §6.4.1).
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
 *   2. The on-wire timestamp format we use: the WebRTC
 *      `abs-send-time` header extension (24-bit unsigned at 65536 Hz,
 *      256 s wrap), the standard cross-implementation-compatible
 *      variant emitted by Chromium / WebRTC / Firefox.  It originates
 *      in the never-published `draft-ietf-avtcore-abs-send-time`, so
 *      it has no RFC number to cite; we bind the extension URI
 *      `http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time`
 *      so the wire is interoperable with the existing
 *      chrome_opus_offer fixture used in interop/.
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
 *   The seam is complete as of v1.0.0: the per-SSRC clock rate, the
 *   RTP-timestamp wrap, the abs-send-time participation and the
 *   origin anchoring all landed.  The *engine* is what turns the
 *   flag into behaviour — see ADR-017 §"Decision 5" for the
 *   per-layer breakdown.
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

/** Canonical WebRTC abs-send-time extension URI.
 *  24-bit unsigned timestamp at `kAbsSendTimeClockRateHz` = 65536 Hz.
 *  Wrap period: 2^24 / 65536 = 256 s (~4.3 min).
 *
 *  This extension has no RFC: it comes from the expired
 *  `draft-ietf-avtcore-abs-send-time` and is a de-facto WebRTC
 *  standard. RFC 6051 ("Rapid Synchronisation of RTP Flows") is a
 *  different document and does not define it. */
NIMRTC_API_STABLE inline constexpr std::string_view kAbsSendTimeUri =
    "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time";

/** Clock rate used to interpret the 24-bit abs-send-time field.
 *  Resolution: 1 / 65536 s ≈ 15.26 µs — finer than any video / audio
 *  pipeline we care about, coarse enough to fit 24 bits in the
 *  extension element. */
NIMRTC_API_STABLE inline constexpr std::uint32_t kAbsSendTimeClockRateHz = 65536;

/** Local extension id we advertise when `timeline_enabled = true` and
 *  no peer id has been negotiated. Chrome uses 4 historically; 4 is
 *  also free of the IDs we use for ssrc-audio-level (1) and
 *  transport-wide CC (5).  Use 4 when we are the offerer and no
 *  id was taken by `a=extmap` parsing. */
NIMRTC_API_STABLE inline constexpr std::uint16_t kAbsSendTimeDefaultLocalId = 4;

/** RFC 5905 NTP epoch offset: NTP seconds = UNIX seconds + kNtpEpochOffset. */
NIMRTC_API_STABLE inline constexpr std::uint64_t kNtpEpochOffset = 2208988800ULL;

/** Default SR send cadence (matches WebRTC default; configurable via
 *  EngineConfig). One second — frequent enough for ~5 ms alignment
 *  even under heavy NTP jitter, cheap enough to ignore on the wire. */
NIMRTC_API_STABLE inline constexpr std::uint32_t kSrPeriodMs = 1000;

/** Maximum disagreement (µs) between the SR-based estimate and the
 *  abs-send-time-derived estimate before the tracker rejects the
 *  abs-send-time refinement and keeps the SR-based value.
 *
 *  1 s is deliberately loose: the two estimates describe the same
 *  quantity (the sender's send instant of the packet) and normally
 *  agree to within the media clock's rate error — a few hundred
 *  microseconds over a minute-long session.  The tolerance exists to
 *  catch a *broken* abs-send-time anchor (e.g. a peer that restarts
 *  its 256 s counter), not to police normal drift. */
NIMRTC_API_STABLE inline constexpr std::int64_t kAstAgreementToleranceUs = 1'000'000LL;

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
 *  wraps modulo 2^24 (every 256 s at 65536 Hz).
 *
 *  `monotonic_origin_ntp64` anchors the monotonic clock to a wall
 *  clock: the encoded value then represents the *absolute* instant
 *  `origin + monotonic_us` on the Unix timeline, so two ends that
 *  share the origin can compare the field directly.  An unset origin
 *  (`Ntp64{0}`) contributes 0 µs, which degrades the encoder to the
 *  pre-anchoring behaviour (raw monotonic ticks) rather than
 *  producing a nonsensical epoch-1900 value.
 *
 *  The field is 24 bits wide, so the absolute instant is only
 *  recoverable modulo 256 s — see `SenderReportTracker` for how the
 *  receiver resolves the wrap cycle against a Sender Report. */
NIMRTC_API_STABLE std::uint32_t
    encode_abs_send_time_24(std::chrono::microseconds monotonic_us,
                           Ntp64 monotonic_origin_ntp64) noexcept;

/** Inverse of `encode_abs_send_time_24`. Converts the 24-bit wire value
 *  back to the *position within one 256 s wrap cycle* (0 .. 256 s).
 *  The origin is not applied here: choosing which cycle the value
 *  belongs to is the caller's job — only the caller knows the
 *  absolute instant to align against.  `SenderReportTracker::
 *  capture_ts_us_for()` does that alignment.
 *
 *  The wire carries 24 bits at 65536 Hz, so the quantisation is
 *  `1e6 / 65536 ≈ 15.26 µs`. */
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
//     - EWMA / Kalman smoothing (v1.1+; the linear model above is the
//       deliberate v1.0.0 trade-off)
//
//   The abs-send-time field participates as a *refinement* (ADR-017
//   §"Decision 2"): the SR supplies the epoch, abs-send-time supplies
//   the sender's own elapsed wall-clock time, which does not inherit
//   the media clock's rate error.  See the 7-argument overload of
//   `capture_ts_us_for()` for the exact rules.
//
//   `local_recv_us_since_epoch` is recorded per SR so callers can
//   measure the estimate's error out-of-band; the tracker itself does
//   not use it.
// -----------------------------------------------------------------------------

struct NIMRTC_API_STABLE SsrcTimelineState {
    Ntp64          sr_ntp64           = Ntp64{0};
    std::uint32_t  sr_rtp_ts          = 0;
    std::int64_t   sr_local_recv_us   = 0;
    // Nanoseconds per RTP timestamp tick.  In v1.0.0 this is a
    // constant derived from `clock_rate_hz`, NOT a fitted slope — there
    // is no multi-SR history.  See ADR-017 §"Decision 5".
    std::int64_t   slope_nsec_per_rtp_ts = 0;
    std::uint32_t  clock_rate_hz         = 90000;
    // Number of SRs ingested for this SSRC (running count).  Only the
    // most-recent SR is *retained* — the count is a diagnostic, not a
    // sample history.  0 means "no SR seen yet".  See ADR-017
    // §"Decision 5".
    std::uint32_t  sr_count              = 0;
    // The abs-send-time anchor for this SSRC: the 24-bit counter value
    // that was current at the instant of the stored Sender Report,
    // decoded to microseconds within the 256 s abs-send-time wrap
    // cycle.  Only meaningful when `sr_abs_send_anchored` is true —
    // the decoded value can legitimately be 0, so the flag (not the
    // value) is the authoritative "anchor available" signal.
    //
    // This is the field ADR-017 §"Decision 2" reserved for the
    // abs-send-time ↔ SR cross-check; it is now populated.
    std::int64_t   sr_abs_send_us        = 0;
    // True when the abs-send-time anchor could be established (i.e. an
    // RTP packet whose RTP timestamp matched the SR's was observed).
    // When false, the abs-send-time refinement does not run and the
    // SR-based estimate is used as-is.
    bool           sr_abs_send_anchored  = false;
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

    /** Maximum disagreement (µs) tolerated between the SR-based estimate
     *  and the abs-send-time-derived estimate before the tracker keeps
     *  the SR-based value and discards the abs-send-time refinement.
     *
     *  Defaults to `kAstAgreementToleranceUs` (1 s).  Raise it for
     *  long sessions on links whose media clock drifts noticeably;
     *  lower it to reject abs-send-time refinements aggressively.
     *  A value of 0 disables the refinement entirely (the abs-send-time
     *  field is then accepted but never used).
     */
    void set_ast_agreement_tolerance_us(std::int64_t tolerance_us) noexcept;

    std::int64_t ast_agreement_tolerance_us() const noexcept;

    /** Configure the media clock rate (Hz) for a given SSRC.
     *
     *  The media clock rate is **not** carried in the RTCP Sender
     *  Report — it comes from the SDP `a=rtpmap:` line, so the caller
     *  (the engine, which negotiated the session) must supply it.
     *
     *  Must be called before the first `on_sender_report()` for this
     *  SSRC.  When it has not been called, the tracker assumes 90000 Hz
     *  (the video default), which produces wrong capture timestamps for
     *  48 kHz audio.
     *
     *  A clock rate of 0 is ignored (the previous value is kept).
     */
    void set_ssrc_clock_rate(std::uint32_t ssrc,
                             std::uint32_t clock_rate_hz) noexcept;

    /** Ingest an inbound Sender Report. `local_recv_us_since_epoch` is
     *  the monotonic-clock microseconds at which the engine *received*
     *  the SR packet (before any queueing). Recorded per SSRC so a
     *  caller can measure the estimate's error out-of-band; the
     *  tracker itself does not use it. */
    void on_sender_report(const SenderReport& sr,
                          std::int64_t local_recv_us_since_epoch) noexcept;

    /** Record an observed (RTP timestamp, abs-send-time) pair for an
     *  SSRC.
     *
     *  The tracker keeps a short ring of the most recent observations
     *  per SSRC so it can later pair the SR's RTP timestamp with the
     *  abs-send-time value that was current at that instant.  That
     *  pairing is what lets the abs-send-time field be interpreted as
     *  an absolute instant instead of a bare 256 s counter.
     *
     *  Calling this is optional: the 7-argument `capture_ts_us_for()`
     *  overload records the observation itself.  Engines that want to
     *  feed the tracker without performing a lookup can call this
     *  directly.
     *
     *  `abs_send_time_24` is the raw 24-bit wire value (0 .. 2^24-1).
     *  Values above 2^24-1 are ignored (they cannot come off the wire).
     */
    void observe_abs_send_time(std::uint32_t ssrc,
                               std::uint32_t rtp_ts,
                               std::uint32_t abs_send_time_24) noexcept;

    /** Number of SSRCs tracked (mostly for tests / stats). */
    std::size_t tracked_ssrc_count() const noexcept;

    /** Current timeline state for an SSRC, or `std::nullopt` when the
     *  SSRC is unknown or no SR has been ingested for it yet.
     *
     *  Exposed so a caller (engine, diagnostics) can inspect the SR
     *  epoch and the abs-send-time anchor the tracker settled on. */
    std::optional<SsrcTimelineState> state_for(std::uint32_t ssrc) const noexcept;

    /** Returns the reconstructed sender-wall-clock capture timestamp
     *  (microseconds, UNIX-anchored) for an inbound RTP packet
     *  identified by SSRC and carrying `abs_send_time_24` as the
     *  24-bit abs-send-time field. Returns std::nullopt when:
     *    - No SR has been seen for this SSRC yet; or
     *    - the packet's RTP timestamp predates the stored SR by more
     *      than one second of media time (only the most-recent SR is
     *      retained, so an older packet has no anchor).
     *
     *  RTP timestamp arithmetic is modular over 2^32 (RFC 3550
     *  Appendix A.1), so a packet that crosses the wrap boundary after
     *  the SR still extrapolates forward correctly.
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
     *      Pass 0 to use the rate configured via
     *      `set_ssrc_clock_rate()`; a non-zero value overrides it for
     *      this call only.
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

    /** Same as the 6-argument overload, but with an explicit
     *  "the packet carries an abs-send-time field" flag.
     *
     *  The 6-argument overload keeps the v1.0.0 semantics: the
     *  abs-send-time argument is accepted for wire compatibility and
     *  ignored — it neither records an observation nor refines the
     *  estimate.  This overload makes the field *participate* in the
     *  estimate, as ADR-017 §"Decision 2" requires.
     *
     *  How it participates:
     *
     *    1. The (rtp_ts, abs_send_time_24) pair is recorded in the
     *       per-SSRC observation ring.
     *    2. The base estimate is computed exactly as in the
     *       6-argument overload: SR instant + RTP-timestamp delta at
     *       the configured media clock rate.
     *    3. If the tracker could pair the stored SR's RTP timestamp
     *       with an observed abs-send-time value (the *anchor*), the
     *       difference between the packet's abs-send-time value and
     *       the anchor gives the sender's own elapsed time — which,
     *       unlike the media-clock delta, does not inherit the media
     *       clock's rate error.  That elapsed time is added to the
     *       SR instant and used as the capture instant, provided the
     *       two estimates agree within `kAstAgreementToleranceUs`.
     *       When they disagree by more, the base estimate wins (the
     *       abs-send-time anchor is then suspect — e.g. a peer that
     *       re-initialises its 256 s counter).
     *
     *  The refinement is skipped entirely when the tolerance is 0
     *  (see `set_ast_agreement_tolerance_us()`), when the wire value
     *  is not a legal 24-bit value, or when no anchor could be
     *  established.
     *
     *  Note on reordered packets: for a packet that predates the SR
     *  the base estimate is *clamped* to the SR instant (the SR-based
     *  path has no anchor for pre-SR packets), while the
     *  abs-send-time delta recovers the packet's real pre-SR instant.
     *  The refinement therefore restores the true capture time for
     *  jitter-reordered packets.
     *
     *  `has_abs_send_time` must be false when the packet carries no
     *  abs-send-time element; passing a stale `abs_send_time_24`
     *  value in that case would poison the observation ring.
     *
     *  Out-params: identical to the 6-argument overload.
     */
    std::optional<std::int64_t> capture_ts_us_for(
        std::uint32_t ssrc,
        std::uint32_t rtp_ts,
        std::uint32_t abs_send_time_24,
        bool has_abs_send_time,
        std::uint32_t clock_rate_hz,
        std::int64_t local_recv_us_since_epoch,
        std::int64_t* out_sender_recv_skew_us) noexcept;

private:
    Ntp64 origin_;
    std::int64_t ast_tolerance_us_ = kAstAgreementToleranceUs;
    struct Impl;
    Impl* impl_ = nullptr;

    /** (Re)derive the abs-send-time anchor for an SSRC from the
     *  observation ring and the currently stored Sender Report.
     *
     *  Called after every SR ingestion and after every observation;
     *  clears the anchor when no observation can be paired with the
     *  SR's RTP timestamp. */
    void try_anchor_ast(std::uint32_t ssrc) noexcept;

    // Free swap() needs to be a friend so it can read/write the pImpl
    // pointer and the NTP origin.  The body lives below the class as
    // a namespace-scope inline function (matches the rest of the
    // modules/rtp API style for free helpers).
    friend inline void swap(SenderReportTracker& a,
                            SenderReportTracker& b) noexcept;
};

// Free function (trivially movable) — defined for the move-ctor.
NIMRTC_API_STABLE inline void swap(SenderReportTracker& a,
                                 SenderReportTracker& b) noexcept {
    using std::swap;
    swap(a.origin_, b.origin_);
    swap(a.ast_tolerance_us_, b.ast_tolerance_us_);
    swap(a.impl_,   b.impl_);
}

} // namespace nimrtc::rtp
