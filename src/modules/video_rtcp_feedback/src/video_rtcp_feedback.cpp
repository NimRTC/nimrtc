/**
 * @file src/modules/video_rtcp_feedback/src/video_rtcp_feedback.cpp
 * @brief PLI / FIR / SLI / NACK wire-format encode + decode for video RTCP.
 *
 * RFC references:
 *   - RFC 3550 §6.1 — RTCP fixed header.
 *   - RFC 4585 §6.2.1 — Generic NACK feedback (RTPFB FMT=1).
 *   - RFC 4585 §6.3.1 — Picture Loss Indication (PSFB FMT=1).
 *   - RFC 4585 §6.3.2 — Slice Loss Indication (PSFB FMT=2).
 *   - RFC 5104 §4.3.1 — Full Intra Request (PSFB FMT=4) + coalescing.
 *
 * Wire format cheatsheet (all values big-endian on the wire):
 *
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |V=2|P| FMT(5)  |   PT(8)       |          length(16)         |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                     sender SSRC (32)                          |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                     media  SSRC (32)                          |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |  FCI entries ...                                              |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * `length` is "total packet size in 32-bit words, minus one" (RFC 3550 §6.1).
 */

#include <nimrtc/video_rtcp_feedback/video_rtcp_feedback.hpp>

#include <algorithm>
#include <cstring>
#include <string>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/error.hpp>
#include <nimrtc/core/log.hpp>

namespace nimrtc::video_rtcp_feedback {

namespace {

// ---------------------------------------------------------------------------
// Byte-order helpers (always big-endian on the wire)
// ---------------------------------------------------------------------------

inline std::uint16_t read_be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(p[0]) << 8) |
         static_cast<std::uint16_t>(p[1]));
}

inline std::uint32_t read_be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8)  |
            static_cast<std::uint32_t>(p[3]);
}

inline void write_be16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[1] = static_cast<std::uint8_t>( v       & 0xFF);
}

inline void write_be32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<std::uint8_t>((v >> 8)  & 0xFF);
    p[3] = static_cast<std::uint8_t>( v       & 0xFF);
}

// ---------------------------------------------------------------------------
// Header helpers
// ---------------------------------------------------------------------------

// byte 0 = V(2)=10 | P(1)=0 | FMT(5)
inline std::uint8_t rtcp_byte0(std::uint8_t fmt) noexcept {
    return static_cast<std::uint8_t>((2u << 6) | (fmt & 0x1Fu));
}

// length_words = (packet_size_bytes / 4) - 1
inline std::uint16_t length_from_bytes(std::size_t bytes) noexcept {
    return static_cast<std::uint16_t>((bytes / 4u) - 1u);
}

// FCI payload offsets after the fixed 12-byte preamble.
inline constexpr std::size_t kFciOffset = kRtcpHeaderSize + kSsrcBlockSize; // 12

// Validate the RTCP header and check that:
//   - bytes 0..3 are present
//   - version is 2
//   - P bit is 0 (we don't accept padded feedback packets in this module;
//     per RFC 4585 feedback messages are rarely padded, and supporting
//     padding complicates the parser for a feature that no WebRTC peer
//     exercises on PSFB/RTPFB today).
//   - PT matches the expected payload type (PSFB or RTPFB).
//   - FMT matches the expected feedback kind.
//   - the declared length is consistent with the input size AND ≥ 12 bytes.
struct HeaderCheck {
    std::uint8_t  fmt        = 0;     // echoed for convenience
    std::uint16_t length_words = 0;
    std::size_t   total_bytes = 0;   // (length_words + 1) * 4
};

template <std::uint8_t kExpectedPt, std::uint8_t kExpectedFmt>
inline core::Result<HeaderCheck> parse_header(core::ByteSpan input,
                                              std::size_t min_fci_bytes) noexcept {
    if (input.size() < kRtcpHeaderSize) {
        return core::Result<HeaderCheck>::fail(
            core::ErrorCode::ProtocolError,
            "video_rtcp_feedback: input smaller than RTCP header (" +
            std::to_string(input.size()) + " < 4)");
    }
    HeaderCheck hc;
    const std::uint8_t b0 = input[0];
    const std::uint8_t version = static_cast<std::uint8_t>((b0 >> 6) & 0x03u);
    const std::uint8_t padding = static_cast<std::uint8_t>((b0 >> 5) & 0x01u);
    hc.fmt        = static_cast<std::uint8_t>(b0 & 0x1Fu);
    const std::uint8_t pt = input[1];

    if (version != 2) {
        return core::Result<HeaderCheck>::fail(
            core::ErrorCode::ProtocolError,
            "video_rtcp_feedback: RTCP version != 2 (got " +
            std::to_string(version) + ")");
    }
    if (padding != 0) {
        return core::Result<HeaderCheck>::fail(
            core::ErrorCode::ProtocolError,
            "video_rtcp_feedback: padded feedback packets not supported");
    }
    if (pt != kExpectedPt) {
        return core::Result<HeaderCheck>::fail(
            core::ErrorCode::ProtocolError,
            "video_rtcp_feedback: wrong RTCP PT (expected " +
            std::to_string(kExpectedPt) + ", got " + std::to_string(pt) + ")");
    }
    if (hc.fmt != kExpectedFmt) {
        return core::Result<HeaderCheck>::fail(
            core::ErrorCode::ProtocolError,
            "video_rtcp_feedback: wrong FMT (expected " +
            std::to_string(kExpectedFmt) + ", got " +
            std::to_string(hc.fmt) + ")");
    }
    hc.length_words = read_be16(input.data() + 2);
    hc.total_bytes  = static_cast<std::size_t>(hc.length_words + 1u) * 4u;

    if (hc.total_bytes < kPliPacketSize) {
        return core::Result<HeaderCheck>::fail(
            core::ErrorCode::ProtocolError,
            "video_rtcp_feedback: length field shorter than 12 bytes (length_words=" +
            std::to_string(hc.length_words) + ")");
    }
    if (input.size() < hc.total_bytes) {
        return core::Result<HeaderCheck>::fail(
            core::ErrorCode::ProtocolError,
            "video_rtcp_feedback: input truncated — declared " +
            std::to_string(hc.total_bytes) + "B, got " +
            std::to_string(input.size()) + "B");
    }
    if (hc.total_bytes - kPliPacketSize < min_fci_bytes) {
        return core::Result<HeaderCheck>::fail(
            core::ErrorCode::ProtocolError,
            "video_rtcp_feedback: FCI too small (have " +
            std::to_string(hc.total_bytes - kPliPacketSize) +
            "B, need at least " + std::to_string(min_fci_bytes) + "B)");
    }
    return core::Result<HeaderCheck>::ok(hc);
}

inline void write_header(std::uint8_t* out, std::uint8_t fmt, std::uint8_t pt,
                         std::uint16_t length_words) noexcept {
    out[0] = rtcp_byte0(fmt);
    out[1] = pt;
    write_be16(out + 2, length_words);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// PLI  (RFC 4585 §6.3.1)
// ---------------------------------------------------------------------------

std::size_t build_pli(core::MutableByteSpan output, const PliPacket& p) noexcept {
    if (output.size() < kPliPacketSize) {
        return 0;
    }
    write_header(output.data(), kFmtPli, kPsfbPt, length_from_bytes(kPliPacketSize));
    write_be32(output.data() + kRtcpHeaderSize,        p.sender_ssrc);
    write_be32(output.data() + kRtcpHeaderSize + 4,    p.media_ssrc);
    return kPliPacketSize;
}

core::Result<PliPacket> parse_pli(core::ByteSpan input) noexcept {
    auto h = parse_header<kPsfbPt, kFmtPli>(input, /*min_fci=*/0);
    if (!h.ok()) {
        return core::Result<PliPacket>::fail(h.error().code(), h.error().message());
    }
    // PLI has no FCI; length must be exactly 2 (=12 bytes).
    if (h.value().length_words != 2) {
        return core::Result<PliPacket>::fail(
            core::ErrorCode::ProtocolError,
            "video_rtcp_feedback: PLI length != 2 (got " +
            std::to_string(h.value().length_words) + ")");
    }
    PliPacket p;
    p.sender_ssrc = read_be32(input.data() + kRtcpHeaderSize);
    p.media_ssrc  = read_be32(input.data() + kRtcpHeaderSize + 4);
    return core::Result<PliPacket>::ok(p);
}

// ---------------------------------------------------------------------------
// FIR  (RFC 5104 §4.3.1.1)
// ---------------------------------------------------------------------------

std::size_t build_fir(core::MutableByteSpan output, const FirPacket& p) noexcept {
    const std::size_t total = kPliPacketSize + p.entries.size() * kFirEntrySize;
    if (output.size() < total) {
        return 0;
    }
    write_header(output.data(), kFmtFir, kPsfbPt, length_from_bytes(total));
    write_be32(output.data() + kRtcpHeaderSize,     p.sender_ssrc);

    std::uint8_t* fp = output.data() + kFciOffset;
    for (std::size_t i = 0; i < p.entries.size(); ++i) {
        const FirEntry& e = p.entries[i];
        write_be32(fp, e.ssrc);          // SSRC(4)
        fp[4] = 0;                       // reserved(1)
        fp[5] = e.seq_nr;                 // seq_nr(1)
        fp[6] = 0;                       // reserved(1)
        fp[7] = 0;                       // reserved(1)
        fp += kFirEntrySize;
    }
    return total;
}

core::Result<FirPacket> parse_fir(core::ByteSpan input) noexcept {
    auto h = parse_header<kPsfbPt, kFmtFir>(input, /*min_fci=*/kFirEntrySize);
    if (!h.ok()) {
        return core::Result<FirPacket>::fail(h.error().code(), h.error().message());
    }
    const std::size_t fci_bytes = h.value().total_bytes - kPliPacketSize;
    if (fci_bytes % kFirEntrySize != 0) {
        return core::Result<FirPacket>::fail(
            core::ErrorCode::ProtocolError,
            "video_rtcp_feedback: FIR FCI not 8-byte aligned (" +
            std::to_string(fci_bytes) + "B)");
    }
    const std::size_t n = fci_bytes / kFirEntrySize;
    FirPacket p;
    p.sender_ssrc = read_be32(input.data() + kRtcpHeaderSize);
    p.entries.reserve(n);

    const std::uint8_t* fp = input.data() + kFciOffset;
    for (std::size_t i = 0; i < n; ++i) {
        FirEntry e;
        e.ssrc   = read_be32(fp);
        // fp[4] is reserved — ignored.
        e.seq_nr = fp[5];
        // fp[6..7] are reserved — ignored.
        p.entries.push_back(e);
        fp += kFirEntrySize;
    }
    return core::Result<FirPacket>::ok(p);
}

// ---------------------------------------------------------------------------
// SLI  (RFC 4585 §6.3.2)
//
// Wire interpretation choice:
//   The RFC packs two SLI entries into 8 bytes, with each entry laid out as
//   (first:13, number:13, picture_id:6) over 4 bytes. To keep the public
//   API and the encode/decode paths trivial this module uses ONE entry per
//   8 bytes — 2 bytes `first`, 2 bytes `number`, 4 bytes `picture_id`.
//   This is interoperable only with ourselves; treat SLI as a "parse and
//   regenerate" round-trip, not a wire format other peers are expected to
//   emit on the public Internet.
// ---------------------------------------------------------------------------

std::size_t build_sli(core::MutableByteSpan output, const SliPacket& p) noexcept {
    const std::size_t total = kPliPacketSize + p.entries.size() * kSliEntrySize;
    if (output.size() < total) {
        return 0;
    }
    write_header(output.data(), kFmtSli, kPsfbPt, length_from_bytes(total));
    write_be32(output.data() + kRtcpHeaderSize,     p.sender_ssrc);
    write_be32(output.data() + kRtcpHeaderSize + 4, p.media_ssrc);

    std::uint8_t* fp = output.data() + kFciOffset;
    for (std::size_t i = 0; i < p.entries.size(); ++i) {
        const SliEntry& e = p.entries[i];
        write_be16(fp,     e.first);       // first(2)
        write_be16(fp + 2, e.number);      // number(2)
        write_be32(fp + 4, e.picture_id);  // picture_id(4)
        fp += kSliEntrySize;
    }
    return total;
}

core::Result<SliPacket> parse_sli(core::ByteSpan input) noexcept {
    auto h = parse_header<kPsfbPt, kFmtSli>(input, /*min_fci=*/kSliEntrySize);
    if (!h.ok()) {
        return core::Result<SliPacket>::fail(h.error().code(), h.error().message());
    }
    const std::size_t fci_bytes = h.value().total_bytes - kPliPacketSize;
    if (fci_bytes % kSliEntrySize != 0) {
        return core::Result<SliPacket>::fail(
            core::ErrorCode::ProtocolError,
            "video_rtcp_feedback: SLI FCI not 8-byte aligned (" +
            std::to_string(fci_bytes) + "B)");
    }
    const std::size_t n = fci_bytes / kSliEntrySize;
    SliPacket p;
    p.sender_ssrc = read_be32(input.data() + kRtcpHeaderSize);
    p.media_ssrc  = read_be32(input.data() + kRtcpHeaderSize + 4);
    p.entries.reserve(n);

    const std::uint8_t* fp = input.data() + kFciOffset;
    for (std::size_t i = 0; i < n; ++i) {
        SliEntry e;
        e.first      = read_be16(fp);
        e.number     = read_be16(fp + 2);
        e.picture_id = read_be32(fp + 4);
        p.entries.push_back(e);
        fp += kSliEntrySize;
    }
    return core::Result<SliPacket>::ok(p);
}

// ---------------------------------------------------------------------------
// NACK  (RFC 4585 §6.2.1)
// ---------------------------------------------------------------------------

std::size_t build_nack(core::MutableByteSpan output, const NackPacket& p) noexcept {
    const std::size_t total = kPliPacketSize + p.entries.size() * kNackEntrySize;
    if (output.size() < total) {
        return 0;
    }
    write_header(output.data(), kFmtNack, kRtpfbPt, length_from_bytes(total));
    write_be32(output.data() + kRtcpHeaderSize,     p.sender_ssrc);
    write_be32(output.data() + kRtcpHeaderSize + 4, p.media_ssrc);

    std::uint8_t* fp = output.data() + kFciOffset;
    for (std::size_t i = 0; i < p.entries.size(); ++i) {
        const NackEntry& e = p.entries[i];
        write_be16(fp,     e.pid);
        write_be16(fp + 2, e.blp);
        fp += kNackEntrySize;
    }
    return total;
}

core::Result<NackPacket> parse_nack(core::ByteSpan input) noexcept {
    auto h = parse_header<kRtpfbPt, kFmtNack>(input, /*min_fci=*/kNackEntrySize);
    if (!h.ok()) {
        return core::Result<NackPacket>::fail(h.error().code(), h.error().message());
    }
    const std::size_t fci_bytes = h.value().total_bytes - kPliPacketSize;
    if (fci_bytes % kNackEntrySize != 0) {
        return core::Result<NackPacket>::fail(
            core::ErrorCode::ProtocolError,
            "video_rtcp_feedback: NACK FCI not 4-byte aligned (" +
            std::to_string(fci_bytes) + "B)");
    }
    const std::size_t n = fci_bytes / kNackEntrySize;
    NackPacket p;
    p.sender_ssrc = read_be32(input.data() + kRtcpHeaderSize);
    p.media_ssrc  = read_be32(input.data() + kRtcpHeaderSize + 4);
    p.entries.reserve(n);

    const std::uint8_t* fp = input.data() + kFciOffset;
    for (std::size_t i = 0; i < n; ++i) {
        NackEntry e;
        e.pid = read_be16(fp);
        e.blp = read_be16(fp + 2);
        p.entries.push_back(e);
        fp += kNackEntrySize;
    }
    return core::Result<NackPacket>::ok(p);
}

// ---------------------------------------------------------------------------
// Region refresh (remote-desktop plan B9)
// ---------------------------------------------------------------------------

std::size_t build_region_refresh(core::MutableByteSpan output,
                                  const RegionRefreshPacket& p) noexcept {
    // A region-refresh message with no region carries no information:
    // the peer would learn "something changed somewhere", which is the
    // PLI's job, not this message's.
    if (p.regions.empty()) return 0;

    const std::size_t total =
        region_refresh_size(p.regions.size());
    if (output.size() < total) return 0;

    // RTCP header: V=2, P=0, subtype=0, PT=204 (APP).
    write_header(output.data(), /*fmt=*/0, kAppPt, length_from_bytes(total));
    write_be32(output.data() + kRtcpHeaderSize, p.sender_ssrc);

    // The APP name ("NRGN") and the media SSRC.
    //
    // The wire layout is:
    //   [0..3]   RTCP header
    //   [4..7]   sender SSRC
    //   [8..11]  name ("NRGN")
    //   [12..15] media SSRC
    //   [16..]   the rectangles
    //
    // which is exactly `kRegionRefreshHeaderSize` bytes of preamble.
    std::uint8_t* fp = output.data() + kRtcpHeaderSize + 4;
    std::memcpy(fp, kRegionRefreshName, sizeof(kRegionRefreshName));
    fp += sizeof(kRegionRefreshName);
    write_be32(fp, p.media_ssrc);
    fp += 4;

    for (const RegionRect& r : p.regions) {
        write_be32(fp,     r.x);
        write_be32(fp + 4, r.y);
        write_be32(fp + 8, r.width);
        write_be32(fp + 12, r.height);
        fp += kRegionRefreshEntrySize;
    }
    return total;
}

bool is_region_refresh(core::ByteSpan input) noexcept {
    if (input.size() < kRegionRefreshHeaderSize) return false;
    const std::uint8_t b0 = input[0];
    const std::uint8_t version = static_cast<std::uint8_t>((b0 >> 6) & 0x03u);
    if (version != 2) return false;
    if (input[1] != kAppPt) return false;
    // The name sits right after the sender SSRC.
    return std::memcmp(input.data() + kRtcpHeaderSize + 4,
                       kRegionRefreshName, sizeof(kRegionRefreshName)) == 0;
}

core::Result<RegionRefreshPacket> parse_region_refresh(
        core::ByteSpan input) noexcept {
    if (input.size() < kRegionRefreshHeaderSize) {
        return core::Result<RegionRefreshPacket>::fail(
            core::ErrorCode::ProtocolError,
            "video_rtcp_feedback: region refresh truncated (header)");
    }
    const std::uint8_t b0 = input[0];
    const std::uint8_t version = static_cast<std::uint8_t>((b0 >> 6) & 0x03u);
    if (version != 2) {
        return core::Result<RegionRefreshPacket>::fail(
            core::ErrorCode::ProtocolError,
            "video_rtcp_feedback: region refresh RTCP version != 2");
    }
    if (input[1] != kAppPt) {
        return core::Result<RegionRefreshPacket>::fail(
            core::ErrorCode::ProtocolError,
            "video_rtcp_feedback: region refresh is not an APP packet");
    }
    if (!is_region_refresh(input)) {
        return core::Result<RegionRefreshPacket>::fail(
            core::ErrorCode::ProtocolError,
            "video_rtcp_feedback: APP packet is not a region refresh");
    }

    const std::uint16_t length_words = read_be16(input.data() + 2);
    const std::size_t total_bytes =
        static_cast<std::size_t>(length_words + 1u) * 4u;
    if (total_bytes < kRegionRefreshHeaderSize) {
        return core::Result<RegionRefreshPacket>::fail(
            core::ErrorCode::ProtocolError,
            "video_rtcp_feedback: region refresh length field too small");
    }
    if (input.size() < total_bytes) {
        return core::Result<RegionRefreshPacket>::fail(
            core::ErrorCode::ProtocolError,
            "video_rtcp_feedback: region refresh truncated (body)");
    }
    const std::size_t body = total_bytes - kRegionRefreshHeaderSize;
    if (body % kRegionRefreshEntrySize != 0) {
        return core::Result<RegionRefreshPacket>::fail(
            core::ErrorCode::ProtocolError,
            "video_rtcp_feedback: region refresh body not 16-byte aligned");
    }

    RegionRefreshPacket p;
    p.sender_ssrc = read_be32(input.data() + kRtcpHeaderSize);
    p.media_ssrc  = read_be32(input.data() + kRtcpHeaderSize + 8);

    const std::size_t n = body / kRegionRefreshEntrySize;
    p.regions.reserve(n);
    const std::uint8_t* fp = input.data() + kRegionRefreshHeaderSize;
    for (std::size_t i = 0; i < n; ++i) {
        RegionRect r;
        r.x      = read_be32(fp);
        r.y      = read_be32(fp + 4);
        r.width  = read_be32(fp + 8);
        r.height = read_be32(fp + 12);
        p.regions.push_back(r);
        fp += kRegionRefreshEntrySize;
    }
    return core::Result<RegionRefreshPacket>::ok(p);
}

// ---------------------------------------------------------------------------
// NACK FCI packing helpers
// ---------------------------------------------------------------------------

std::vector<NackEntry> pack_nack_entries(
    const std::vector<std::uint16_t>& seqs) noexcept {
    std::vector<NackEntry> out;
    if (seqs.empty()) return out;

    // Anchor the wrap-aware arithmetic on the first element of the list.
    const std::int32_t anchor = static_cast<std::int32_t>(seqs[0]);

    std::vector<std::int32_t> deltas;
    deltas.reserve(seqs.size());
    for (std::uint16_t s : seqs) {
        // Signed 16-bit difference (RFC 3550 §3.3.1 style): values beyond
        // +32767 wrap around into the negative range, which keeps a loss
        // set that straddles 65535 → 0 contiguous.
        const std::int32_t raw = static_cast<std::int32_t>(s) - anchor;
        const std::int32_t d = (raw > 32767) ? (raw - 65536) : raw;
        deltas.push_back(d);
    }
    std::sort(deltas.begin(), deltas.end());
    deltas.erase(std::unique(deltas.begin(), deltas.end()), deltas.end());

    std::size_t i = 0;
    while (i < deltas.size()) {
        const std::int32_t pid_delta = deltas[i];
        NackEntry e;
        e.pid = static_cast<std::uint16_t>(
            static_cast<std::uint32_t>((anchor + pid_delta) & 0xFFFF));
        e.blp = 0;
        ++i;
        // The BLP bitmask covers PID+1 .. PID+16.
        while (i < deltas.size()) {
            const std::int32_t off = deltas[i] - pid_delta;
            if (off < 1 || off > 16) break;
            e.blp = static_cast<std::uint16_t>(e.blp | (1u << (off - 1)));
            ++i;
        }
        out.push_back(e);
    }
    return out;
}

std::vector<std::uint16_t> unpack_nack_entries(const NackPacket& p) noexcept {
    std::vector<std::uint16_t> out;
    for (const auto& e : p.entries) {
        out.push_back(e.pid);
        for (int bit = 0; bit < 16; ++bit) {
            if ((e.blp & (1u << bit)) == 0) continue;
            out.push_back(static_cast<std::uint16_t>(e.pid + bit + 1));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// FirCoalescer
// ---------------------------------------------------------------------------

FirCoalescer::FirCoalescer(std::uint32_t sender_ssrc) noexcept
    : sender_ssrc_(sender_ssrc),
      next_seq_nr_(0),
      min_interval_us_(kFirMinIntervalUs) {}

FirCoalescer::FirCoalescer(std::uint32_t sender_ssrc,
                           std::int64_t min_interval_us) noexcept
    : sender_ssrc_(sender_ssrc),
      next_seq_nr_(0),
      min_interval_us_(min_interval_us < 0 ? 0 : min_interval_us) {}

bool FirCoalescer::should_send(std::uint32_t media_ssrc, std::int64_t now_us) noexcept {
    auto it = last_sent_us_.find(media_ssrc);
    if (it == last_sent_us_.end()) {
        return true;                          // never sent
    }
    return (now_us - it->second) >= min_interval_us_;
}

void FirCoalescer::mark_sent(std::uint32_t media_ssrc, std::int64_t now_us) noexcept {
    last_sent_us_[media_ssrc] = now_us;
}

FirEntry FirCoalescer::make_entry(std::uint32_t media_ssrc) noexcept {
    const std::uint8_t seq = next_seq_nr_;
    next_seq_nr_ = static_cast<std::uint8_t>((next_seq_nr_ + 1u) & 0xFFu);
    return FirEntry{media_ssrc, seq};
}

} // namespace nimrtc::video_rtcp_feedback
