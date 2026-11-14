/**
 * @file nimrtc/video_rtcp_feedback/video_rtcp_feedback.hpp
 * @brief Video RTCP feedback messages — PLI / FIR / SLI / NACK.
 *
 * Per design document §6 (Video Quality Feedback) + §11.6 (self-written RTCP):
 *   - PLI (RFC 4585 §6.3.1)  — Picture Loss Indication. No FCI.
 *   - FIR (RFC 5104 §4.3.1)  — Full Intra Request. Coalesced at ~4 Hz / SSRC.
 *   - SLI (RFC 4585 §6.3.2)  — Slice Loss Indication. Parsed, not generated.
 *   - NACK (RFC 4585 §6.2.1) — Generic NACK. Re-request specific RTP packets.
 *
 * All builders take a caller-owned output span and return the number of bytes
 * actually written. If the destination is smaller than the wire size the
 * builder writes nothing and returns 0 — callers are expected to size the
 * destination up-front using the `k*P*Size` constants below.
 *
 * All parsers return a `core::Result<T>` and treat the input as an immutable
 * view (zero-copy where possible). Malformed input produces a
 * `core::ErrorCode::ProtocolError`.
 *
 * @note Standalone module. No vendor libraries, no global state.
 */

#pragma once


// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/error.hpp>

namespace nimrtc::video_rtcp_feedback {

// ---------------------------------------------------------------------------
// Wire-format constants (RFC 3550 §6.1 + RFC 4585 / RFC 5104).
// ---------------------------------------------------------------------------
// Fixed RTCP header (4) + sender-SSRC (4) + media-SSRC (4) = 12 bytes
// is the minimum size of any feedback packet (PLI has no FCI).
NIMRTC_API_STABLE inline constexpr std::size_t kRtcpHeaderSize   = 4;
NIMRTC_API_STABLE inline constexpr std::size_t kSsrcBlockSize    = 8;   // sender + media
NIMRTC_API_STABLE inline constexpr std::size_t kPliPacketSize    = kRtcpHeaderSize + kSsrcBlockSize; // 12
NIMRTC_API_STABLE inline constexpr std::size_t kFirMinSize       = kPliPacketSize;                    // 12
NIMRTC_API_STABLE inline constexpr std::size_t kSliMinSize       = kPliPacketSize;                    // 12
NIMRTC_API_STABLE inline constexpr std::size_t kNackMinSize      = kPliPacketSize;                    // 12

// FCI sizes per entry:
NIMRTC_API_STABLE inline constexpr std::size_t kFirEntrySize     = 8;   // SSRC(4) + reserved(1) + seq_nr(1) + reserved(2)
NIMRTC_API_STABLE inline constexpr std::size_t kSliEntrySize     = 8;   // first(2) + number(2) + picture_id(4) [see note]
NIMRTC_API_STABLE inline constexpr std::size_t kNackEntrySize    = 4;   // PID(2) + BLP(2)
NIMRTC_API_STABLE inline constexpr std::size_t kNackFciSize      = kNackEntrySize;  // alias for naming parity

// FCI sizes for a packet containing N entries:
NIMRTC_API_STABLE inline constexpr std::size_t kFirFciSize(std::size_t n) noexcept { return n * kFirEntrySize; }
inline constexpr std::size_t kSliFciSize(std::size_t n) noexcept { return n * kSliEntrySize; }
inline constexpr std::size_t kNackFciSizeN(std::size_t n) noexcept { return n * kNackEntrySize; }

// Default FIR rate-limit: one FIR per ~250 ms per media SSRC (RFC 5104 §4.3.1.1
// recommends ~4 Hz). Expressed in microseconds.
inline constexpr std::int64_t kFirMinIntervalUs = 250'000;

// RTCP payload type for PSFB (RFC 4585 §6): 206.
inline constexpr std::uint8_t kPsfbPt = 206;
// RTCP payload type for RTPFB (RFC 4585 §6.2): 205.
inline constexpr std::uint8_t kRtpfbPt = 205;

// RTCP payload type for APP (RFC 3550 §6.1): 204.
NIMRTC_API_STABLE inline constexpr std::uint8_t kAppPt = 204;

// FCI kinds (the value in the FMT field for each feedback type).
inline constexpr std::uint8_t kFmtPli     = 1;
inline constexpr std::uint8_t kFmtFir     = 4;
inline constexpr std::uint8_t kFmtSli     = 2;
inline constexpr std::uint8_t kFmtNack    = 1;

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

/** Picture Loss Indication (RFC 4585 §6.3.1). */
struct PliPacket {
    std::uint32_t sender_ssrc = 0;   // SSRC of the feedback sender (the receiver)
    std::uint32_t media_ssrc  = 0;   // SSRC of the media source needing a key frame
};

/** One Full Intra Request entry (RFC 5104 §4.3.1.1). */
struct FirEntry {
    std::uint32_t ssrc       = 0;    // which stream to refresh
    std::uint8_t  seq_nr     = 0;    // sequence number (coalescing key)
};

/** Full Intra Request packet — one or more entries in a single feedback. */
struct FirPacket {
    std::uint32_t            sender_ssrc = 0;
    std::vector<FirEntry>    entries;
};

/** One Slice Loss Indication entry (RFC 4585 §6.3.2).
 *  RFC packs (first:13, number:13, picture_id:6) into 4 bytes; this module
 *  stores them as plain integers so the wire format we emit/parse is one
 *  8-byte entry — see note below the SLI builder for the bit-layout decision. */
struct SliEntry {
    std::uint16_t first      = 0;    // first macroblock index
    std::uint16_t number     = 0;    // number of macroblocks lost
    std::uint32_t picture_id = 0;    // 6-bit picture id (RFC); full 32 stored
};

/** Slice Loss Indication packet. */
struct SliPacket {
    std::uint32_t            sender_ssrc = 0;
    std::uint32_t            media_ssrc  = 0;
    std::vector<SliEntry>    entries;
};

/** One NACK FCI entry (RFC 4585 §6.2.1). */
struct NackEntry {
    std::uint16_t pid        = 0;    // first lost packet sequence number
    std::uint16_t blp        = 0;    // bitmask of subsequent losses (PID+1 .. PID+16)
};

/** Generic NACK packet. */
struct NackPacket {
    std::uint32_t            sender_ssrc = 0;
    std::uint32_t            media_ssrc  = 0;
    std::vector<NackEntry>   entries;
};

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------
// Each returns the number of bytes written to `output`. If `output.size()` is
// less than the wire size of the packet the function writes nothing and
// returns 0. Callers should pre-size the buffer using kPliPacketSize +
// entries * kXxxEntrySize.

// ---------------------------------------------------------------------------
// Region refresh — the "loss-region refresh" of remote-desktop plan B9
// ---------------------------------------------------------------------------
//
// ## Why this exists
//
// Plan B9: "the encoder encodes only the changed region and signals the
// region to the peer (a region-invalidation / loss-region refresh
// mechanism)".
//
// The existing PLI / FIR / SLI messages cannot carry a *rectangle*:
//  - a PLI has no FCI at all (RFC 4585 §6.3.1),
//  - a FIR's FCI entry carries an SSRC and a sequence number
//    (RFC 5104 §4.3.1.1),
//  - an SLI carries macroblock indices of *lost* slices and travels in
//    the receiver -> sender direction (RFC 4585 §6.3.2).
//
// So the region needs its own message, and the RFC-sanctioned way to
// carry application-defined payload in RTCP is the **APP** packet
// (RFC 3550 §6.1).  The name field is "NRGN" (NimRTC ReGiOn), and the
// application data is the media SSRC followed by the rectangles.
//
// The message travels **sender -> peer**: the sender has coded only the
// changed region, and the peer needs to know *which* region to
// invalidate in its decoder.
//
// Wire layout (all values big-endian):
//
//   0                   1                   2                   3
//   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |V=2|P| subtype |  PT=204 (APP) |          length               |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |                     sender SSRC (32)                          |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |                       name = "NRGN"                            |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |                     media  SSRC (32)                          |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |  x (32) | y (32) | width (32) | height (32)   [per region]    |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

/** The RTCP APP name used by the region-refresh message ("NRGN"). */
NIMRTC_API_STABLE inline constexpr std::uint8_t kRegionRefreshName[4] = {
    'N', 'R', 'G', 'N'
};

/** The size of the region-refresh preamble: RTCP header (4) + sender SSRC
 *  (4) + name (4) + media SSRC (4). */
NIMRTC_API_STABLE inline constexpr std::size_t kRegionRefreshHeaderSize = 16;

/** The size of one region in the region-refresh FCI: x, y, width, height
 *  as four 32-bit big-endian values. */
NIMRTC_API_STABLE inline constexpr std::size_t kRegionRefreshEntrySize = 16;

/** One rectangle of the changed region, in source pixels. */
struct NIMRTC_API_STABLE RegionRect {
    std::uint32_t x      = 0;
    std::uint32_t y      = 0;
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
};

/** A region-refresh message: "these rectangles of `media_ssrc` changed". */
struct NIMRTC_API_STABLE RegionRefreshPacket {
    std::uint32_t             sender_ssrc = 0;
    std::uint32_t             media_ssrc  = 0;
    std::vector<RegionRect>   regions;
};

/** The number of bytes `build_region_refresh()` needs for `region_count`
 *  rectangles. */
NIMRTC_API_STABLE inline constexpr std::size_t region_refresh_size(
        std::size_t region_count) noexcept {
    return kRegionRefreshHeaderSize + region_count * kRegionRefreshEntrySize;
}

/** Build a region-refresh message.
 *
 *  @param output  Destination buffer (size it with `region_refresh_size()`).
 *  @param p       The message to encode.
 *  @return The number of bytes written; 0 when the destination is too
 *          small or the message carries no region. */
NIMRTC_API_STABLE std::size_t build_region_refresh(
    core::MutableByteSpan output, const RegionRefreshPacket& p) noexcept;

/** Parse a region-refresh message.
 *
 *  @param input  The received RTCP bytes (one APP packet).
 *  @return The parsed message, or a ProtocolError when the input is not
 *          a well-formed region-refresh APP packet. */
NIMRTC_API_STABLE core::Result<RegionRefreshPacket> parse_region_refresh(
    core::ByteSpan input) noexcept;

/** True iff `input` looks like a region-refresh APP packet (PT=APP and
 *  the "NRGN" name).  A cheap pre-check, so a demuxer can skip the other
 *  APP packets without a full parse. */
NIMRTC_API_STABLE bool is_region_refresh(core::ByteSpan input) noexcept;

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------
// Each returns the number of bytes written to `output`. If `output.size()`
// is less than the wire size of the packet the function writes nothing and
// returns 0. Callers should pre-size the buffer using kPliPacketSize +
// entries * kXxxEntrySize.

NIMRTC_API_STABLE std::size_t build_pli (core::MutableByteSpan output, const PliPacket&  p) noexcept;
NIMRTC_API_STABLE std::size_t build_fir (core::MutableByteSpan output, const FirPacket&  p) noexcept;
NIMRTC_API_STABLE std::size_t build_sli (core::MutableByteSpan output, const SliPacket&  p) noexcept;
NIMRTC_API_STABLE std::size_t build_nack(core::MutableByteSpan output, const NackPacket& p) noexcept;

// ---------------------------------------------------------------------------
// Parsers
// ---------------------------------------------------------------------------
// Each consumes exactly one RTCP feedback packet from `input`. `input.size()`
// must be at least kPliPacketSize (12). The returned objects reference the
// caller's buffer where possible (e.g. string views), but the structured
// fields (NackEntry, FirEntry, ...) are returned by value as the wire
// representation is a copy of fixed-size integers anyway.

NIMRTC_API_STABLE core::Result<PliPacket>  parse_pli (core::ByteSpan input) noexcept;
NIMRTC_API_STABLE core::Result<FirPacket>  parse_fir (core::ByteSpan input) noexcept;
NIMRTC_API_STABLE core::Result<SliPacket>  parse_sli (core::ByteSpan input) noexcept;
NIMRTC_API_STABLE core::Result<NackPacket> parse_nack(core::ByteSpan input) noexcept;

// ---------------------------------------------------------------------------
// NACK FCI packing helpers (RFC 4585 §6.2.1)
// ---------------------------------------------------------------------------
// A single NACK FCI entry covers 17 sequence numbers: the PID itself plus
// the 16 sequence numbers flagged in the BLP bitmask (bit i → PID + i + 1).
// The two helpers below translate between the plain list of lost sequence
// numbers (what a receiver detects) and the compact FCI form (what goes on
// the wire).
//
// Sequence-number arithmetic is wrap-aware: the differences are computed as
// signed 16-bit deltas relative to the first element of the input list, so
// a loss set that straddles the 65535 → 0 rollover still packs into a
// single entry.

/** Pack a list of lost sequence numbers into NACK FCI entries.
 *
 *  @param seqs  The lost sequence numbers (duplicates are ignored).
 *  @return The FCI entries, in ascending order.  Empty when `seqs` is empty. */
NIMRTC_API_STABLE std::vector<NackEntry> pack_nack_entries(
    const std::vector<std::uint16_t>& seqs) noexcept;

/** Expand the FCI entries of a NACK back into the list of lost sequence
 *  numbers (the inverse of `pack_nack_entries`). */
NIMRTC_API_STABLE std::vector<std::uint16_t> unpack_nack_entries(
    const NackPacket& p) noexcept;

// ---------------------------------------------------------------------------
// FIR coalescer — rate-limit FIR per media SSRC (RFC 5104 §4.3.1.1).
// ---------------------------------------------------------------------------
// The coalescer is thread-confined (NOT thread-safe). One coalescer per
// sender; it remembers the last send time per media SSRC and a single
// monotonic `seq_nr` counter used to populate FIR entries.

class NIMRTC_API_STABLE FirCoalescer {
public:
    explicit FirCoalescer(std::uint32_t sender_ssrc) noexcept;

    /** A coalescer with an explicit minimum interval.
     *
     *  The default interval is `kFirMinIntervalUs` (250 ms — RFC 5104
     *  §4.3.1.1's "about 4 Hz").  The keyframe-storm limiter (remote-
     *  desktop plan B14) needs the same mechanism with a *configurable*
     *  interval, so the interval is a constructor argument rather than a
     *  compile-time constant.
     *
     *  @param sender_ssrc     The SSRC this coalescer sends as.
     *  @param min_interval_us The minimum interval between two grants,
     *                         in microseconds.  0 disables the gate. */
    FirCoalescer(std::uint32_t sender_ssrc, std::int64_t min_interval_us) noexcept;

    /** Returns true iff a FIR may be sent for `media_ssrc` at `now_us`.
     *  The first call for a given SSRC always returns true. Subsequent calls
     *  within `kFirMinIntervalUs` microseconds return false.
     *  `now_us` is a monotonic timestamp in microseconds. */
    bool should_send(std::uint32_t media_ssrc, std::int64_t now_us) noexcept;

    /** Records that a FIR was just sent to `media_ssrc` at `now_us`. */
    void mark_sent(std::uint32_t media_ssrc, std::int64_t now_us) noexcept;

    /** Build a fresh FIR entry for `media_ssrc` with the auto-incrementing
     *  per-sender sequence number. Does NOT mark the coalescer as sent —
     *  call `mark_sent()` after the packet has actually been transmitted. */
    FirEntry make_entry(std::uint32_t media_ssrc) noexcept;

    /** The SSRC this coalescer sends as the packet sender. */
    std::uint32_t sender_ssrc() const noexcept { return sender_ssrc_; }

    /** The next sequence number that will be issued by `make_entry()`. */
    std::uint8_t  next_seq_nr() const noexcept { return next_seq_nr_; }

    /** The minimum interval between two grants, in microseconds. */
    std::int64_t  min_interval_us() const noexcept { return min_interval_us_; }

private:
    std::uint32_t                                       sender_ssrc_;
    std::uint8_t                                        next_seq_nr_;
    std::int64_t                                        min_interval_us_;
    std::unordered_map<std::uint32_t, std::int64_t>     last_sent_us_;
};

} // namespace nimrtc::video_rtcp_feedback
