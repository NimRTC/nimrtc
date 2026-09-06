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
inline constexpr std::size_t kRtcpHeaderSize   = 4;
inline constexpr std::size_t kSsrcBlockSize    = 8;   // sender + media
inline constexpr std::size_t kPliPacketSize    = kRtcpHeaderSize + kSsrcBlockSize; // 12
inline constexpr std::size_t kFirMinSize       = kPliPacketSize;                    // 12
inline constexpr std::size_t kSliMinSize       = kPliPacketSize;                    // 12
inline constexpr std::size_t kNackMinSize      = kPliPacketSize;                    // 12

// FCI sizes per entry:
inline constexpr std::size_t kFirEntrySize     = 8;   // SSRC(4) + reserved(1) + seq_nr(1) + reserved(2)
inline constexpr std::size_t kSliEntrySize     = 8;   // first(2) + number(2) + picture_id(4) [see note]
inline constexpr std::size_t kNackEntrySize    = 4;   // PID(2) + BLP(2)
inline constexpr std::size_t kNackFciSize      = kNackEntrySize;  // alias for naming parity

// FCI sizes for a packet containing N entries:
inline constexpr std::size_t kFirFciSize(std::size_t n) noexcept { return n * kFirEntrySize; }
inline constexpr std::size_t kSliFciSize(std::size_t n) noexcept { return n * kSliEntrySize; }
inline constexpr std::size_t kNackFciSizeN(std::size_t n) noexcept { return n * kNackEntrySize; }

// Default FIR rate-limit: one FIR per ~250 ms per media SSRC (RFC 5104 §4.3.1.1
// recommends ~4 Hz). Expressed in microseconds.
inline constexpr std::int64_t kFirMinIntervalUs = 250'000;

// RTCP payload type for PSFB (RFC 4585 §6): 206.
inline constexpr std::uint8_t kPsfbPt = 206;
// RTCP payload type for RTPFB (RFC 4585 §6.2): 205.
inline constexpr std::uint8_t kRtpfbPt = 205;

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

std::size_t build_pli (core::MutableByteSpan output, const PliPacket&  p) noexcept;
std::size_t build_fir (core::MutableByteSpan output, const FirPacket&  p) noexcept;
std::size_t build_sli (core::MutableByteSpan output, const SliPacket&  p) noexcept;
std::size_t build_nack(core::MutableByteSpan output, const NackPacket& p) noexcept;

// ---------------------------------------------------------------------------
// Parsers
// ---------------------------------------------------------------------------
// Each consumes exactly one RTCP feedback packet from `input`. `input.size()`
// must be at least kPliPacketSize (12). The returned objects reference the
// caller's buffer where possible (e.g. string views), but the structured
// fields (NackEntry, FirEntry, ...) are returned by value as the wire
// representation is a copy of fixed-size integers anyway.

core::Result<PliPacket>  parse_pli (core::ByteSpan input) noexcept;
core::Result<FirPacket>  parse_fir (core::ByteSpan input) noexcept;
core::Result<SliPacket>  parse_sli (core::ByteSpan input) noexcept;
core::Result<NackPacket> parse_nack(core::ByteSpan input) noexcept;

// ---------------------------------------------------------------------------
// FIR coalescer — rate-limit FIR per media SSRC (RFC 5104 §4.3.1.1).
// ---------------------------------------------------------------------------
// The coalescer is thread-confined (NOT thread-safe). One coalescer per
// sender; it remembers the last send time per media SSRC and a single
// monotonic `seq_nr` counter used to populate FIR entries.

class FirCoalescer {
public:
    explicit FirCoalescer(std::uint32_t sender_ssrc) noexcept;

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

private:
    std::uint32_t                                       sender_ssrc_;
    std::uint8_t                                        next_seq_nr_;
    std::unordered_map<std::uint32_t, std::int64_t>     last_sent_us_;
};

} // namespace nimrtc::video_rtcp_feedback
