/**
 * @file src/modules/rtp/src/parser.cpp
 * @brief RTP Parser — parses raw bytes into PacketView (RFC 3550).
 *
 * Stateless parse: does not track state across packets.
 * Rejects malformed input with ProtocolError.
 *
 * @note P1 — RFC 3550 + RFC 5285 extensions.
 */

#include <nimrtc/rtp/packet.hpp>

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include <nimrtc/core/error.hpp>

namespace nimrtc::rtp {

namespace {

// RFC 3550 fixed header size (12 bytes)
constexpr std::size_t kFixedHeaderSize = 12;

// Minimum packet size: 12 bytes header
constexpr std::size_t kMinPacketSize = 12;

// CSRC count is in bits 0-3 of first byte
constexpr std::size_t kCsrcCountMask = 0x0F;

// Extension bit: bit 4 of first byte
[[maybe_unused]]
constexpr std::size_t kExtensionBit = 0x10;

// Marker bit: bit 8 of second byte
[[maybe_unused]]
constexpr std::size_t kMarkerBit = 0x80;

// Payload type: bits 0-6 of second byte
constexpr std::size_t kPayloadTypeMask = 0x7F;

// Sequence number: big-endian uint16 at bytes 2-3
// Timestamp: big-endian uint32 at bytes 4-7
// SSRC: big-endian uint32 at bytes 8-11

inline std::uint16_t read_be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

inline std::uint32_t read_be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

// RFC 5285 one-byte header extension:
//   0xBEDE (2 bytes) + 16-bit length (in 32-bit words) + blocks of [1-byte id/length | data...]
// Two-byte form:
//   0x1000 (2 bytes) + 16-bit length + blocks of [2-byte id/length | data...]

// One-byte extension header (12 bytes):
//   word[0] = 0xBEDE
//   word[1] = length (in 32-bit words, not including header words)
constexpr std::uint16_t kOneByteExtProfile = 0xBEDE;
constexpr std::uint16_t kTwoByteExtProfile = 0x1000;

} // anonymous namespace

// ---------------------------------------------------------------------------
// Parser::Impl
// ---------------------------------------------------------------------------

struct Parser::Impl {
    // RFC 5285 local id → URI mapping (e.g. 1 → "urn:ietf:params:rtp-hdrext:sdes:mid").
    // Used by parse() to fill PacketView::extmap_uri.
    std::unordered_map<std::uint16_t, std::string> extension_uris_;

    [[nodiscard]] core::Result<PacketView> parse(core::ByteSpan raw) const;

private:
    // Walk a one-byte-form (0xBEDE) extension data block. Each element is
    // [4-bit id | 4-bit len] [len bytes data] until a 0/0 terminator, then
    // padding to 4-byte boundary.
    void walk_one_byte(core::ByteSpan block,
                       std::map<std::uint16_t, std::string>& uri_out) const;

    // Walk a two-byte-form (0x1000) extension data block. Each element is
    // [8-bit appbits | 8-bit id] [8-bit len] [len bytes data] until a 0
    // terminator, then padding to 4-byte boundary.
    void walk_two_byte(core::ByteSpan block,
                       std::map<std::uint16_t, std::string>& uri_out) const;
};

core::Result<PacketView> Parser::Impl::parse(core::ByteSpan raw) const {
    // Check minimum size
    if (raw.size() < kMinPacketSize) {
        return core::Result<PacketView>::fail(
            core::ErrorCode::ProtocolError,
            "RTP packet too small: " + std::to_string(raw.size()) + " < 12 bytes");
    }

    const std::uint8_t* const data = raw.data();

    // Byte 0: version (bits 6-7), padding (bit 5), extension (bit 4), CSRC count (bits 0-3)
    std::uint8_t version = (data[0] >> 6) & 0x03;
    bool has_padding = (data[0] >> 5) & 0x01;
    bool has_extension = (data[0] >> 4) & 0x01;
    std::size_t csrc_count = data[0] & kCsrcCountMask;

    // Validate version
    if (version != kVersion) {
        return core::Result<PacketView>::fail(
            core::ErrorCode::ProtocolError,
            "RTP version mismatch: expected 2, got " + std::to_string(version));
    }

    // Byte 1: marker (bit 7), payload type (bits 0-6)
    bool marker = (data[1] >> 7) & 0x01;
    std::uint8_t payload_type = data[1] & kPayloadTypeMask;

    // Bytes 2-3: sequence number (big-endian)
    std::uint16_t seq = read_be16(data + 2);

    // Bytes 4-7: timestamp (big-endian)
    std::uint32_t timestamp = read_be32(data + 4);

    // Bytes 8-11: SSRC (big-endian)
    std::uint32_t ssrc = read_be32(data + 8);

    // Calculate header size
    std::size_t header_size = kFixedHeaderSize + (csrc_count * 4);

    // Check we have enough data for header
    if (raw.size() < header_size) {
        return core::Result<PacketView>::fail(
            core::ErrorCode::ProtocolError,
            "RTP packet too small for CSRC count " + std::to_string(csrc_count));
    }

    // Process extension if present
    std::optional<Extension> extension;
    std::size_t ext_size = 0;
    std::map<std::uint16_t, std::string> extmap_uri;
    if (has_extension) {
        if (raw.size() < header_size + 4) {
            return core::Result<PacketView>::fail(
                core::ErrorCode::ProtocolError,
                "RTP packet too small for extension header");
        }

        // Extension header: 2 bytes profile + 2 bytes length
        std::uint16_t ext_profile = read_be16(raw.data() + header_size);
        std::uint16_t ext_length = read_be16(raw.data() + header_size + 2);

        // Length is in 32-bit words, total bytes = ext_length * 4
        std::size_t ext_total = 4 + (static_cast<std::size_t>(ext_length) * 4);

        if (raw.size() < header_size + ext_total) {
            return core::Result<PacketView>::fail(
                core::ErrorCode::ProtocolError,
                "RTP extension data truncated");
        }

        // Build extension data (excluding the 4-byte header)
        extension = Extension{};
        extension->type = ext_profile;
        extension->data = core::ByteSpan(raw.data() + header_size + 4,
                                         ext_length * 4);
        ext_size = ext_total;

        // Walk the extension element block(s) using the profile-specific
        // dispatch. Only populated when the caller has declared URIs —
        // otherwise `extension.data` still carries the raw bytes.
        if (!extension_uris_.empty()) {
            if (ext_profile == kOneByteExtProfile) {
                walk_one_byte(extension->data, extmap_uri);
            } else if (ext_profile == kTwoByteExtProfile) {
                walk_two_byte(extension->data, extmap_uri);
            }
            // Unknown profile: leave extmap_uri empty.
        }
    }

    // CSRC list
    std::vector<std::uint32_t> csrc;
    csrc.reserve(csrc_count);
    for (std::size_t i = 0; i < csrc_count; ++i) {
        csrc.push_back(read_be32(raw.data() + kFixedHeaderSize + (i * 4)));
    }

    // Payload starts after header + extension
    std::size_t payload_start = header_size + ext_size;

    // Handle padding (RFC 3550 §5.1)
    std::size_t padding_trailer = 0;
    if (has_padding && raw.size() > payload_start) {
        // Last byte of the packet is the padding count N.
        //
        // RFC 3550 §5.1 reading implemented here:
        //   - N is the number of *padding data* octets that appear immediately
        //     before the count byte.
        //   - Therefore the total bytes to strip from the payload end is
        //     N (padding data) + 1 (the count byte itself).
        //
        // Validator: at least N >= 1 data bytes plus 1 count byte must fit.
        std::size_t padding_count = raw.data()[raw.size() - 1];
        if (padding_count == 0 || padding_count + 1 > raw.size() - payload_start) {
            return core::Result<PacketView>::fail(
                core::ErrorCode::ProtocolError,
                "RTP padding count invalid: " + std::to_string(padding_count));
        }
        padding_trailer = padding_count + 1;  // N data bytes + 1 count byte
    }

    std::size_t payload_size = raw.size() - payload_start - padding_trailer;

    // Build PacketView
    PacketView pv;
    pv.raw = raw;
    pv.ssrc = ssrc;
    pv.seq = seq;
    pv.timestamp = timestamp;
    pv.payload_type = payload_type;
    pv.marker = marker;
    pv.csrc = std::move(csrc);
    pv.extension = std::move(extension);
    pv.extmap_uri = std::move(extmap_uri);
    pv.payload = core::ByteSpan(raw.data() + payload_start, payload_size);

    return core::Result<PacketView>::ok(std::move(pv));
}

// ---------------------------------------------------------------------------
// Extension walking
// ---------------------------------------------------------------------------

void Parser::Impl::walk_one_byte(core::ByteSpan block,
                                 std::map<std::uint16_t, std::string>& uri_out) const {
    // Each element header is 1 byte: high nibble = id (1..14), low nibble = len.
    // A header of 0x00 terminates the list.
    std::size_t i = 0;
    while (i + 1 <= block.size()) {
        std::uint8_t hdr = block[i];
        if (hdr == 0x00) break;                       // terminator
        std::uint8_t id  = (hdr >> 4) & 0x0F;
        std::uint8_t len = hdr & 0x0F;
        ++i;
        if (id < 1 || id > 14) return;                // malformed, bail
        if (i + len > block.size()) return;           // truncated
        if (auto it = extension_uris_.find(id); it != extension_uris_.end()) {
            uri_out[id] = it->second;
        }
        i += len;
    }
}

void Parser::Impl::walk_two_byte(core::ByteSpan block,
                                 std::map<std::uint16_t, std::string>& uri_out) const {
    // Each element is [1 byte appbits][1 byte id][1 byte len][len bytes data].
    // An id of 0 terminates the list.
    std::size_t i = 0;
    while (i + 2 <= block.size()) {
        std::uint8_t id = block[i + 1];
        if (id == 0) break;                           // terminator
        std::uint8_t len = block[i + 2];
        std::size_t hdr = 3;
        if (i + hdr + len > block.size()) return;     // truncated
        if (auto it = extension_uris_.find(id); it != extension_uris_.end()) {
            uri_out[id] = it->second;
        }
        i += hdr + len;
    }
}

// ---------------------------------------------------------------------------
// Parser public interface
// ---------------------------------------------------------------------------

Parser::Parser() : impl_(std::make_unique<Impl>()) {}

// Out-of-line destructor so that unique_ptr<Impl>::~unique_ptr() is
// instantiated where Impl is complete. Without this, any TU that uses
// Parser as a value (e.g. unit tests) fails with "can't delete an
// incomplete type" because it sees only the forward declaration.
Parser::~Parser() = default;

core::Result<PacketView> Parser::parse(core::ByteSpan raw) const {
    return impl_->parse(raw);
}

void Parser::declare_extension_uri(std::uint16_t type, std::string_view uri) {
    impl_->extension_uris_[type] = std::string(uri);
}

void Parser::clear_extension_uris() {
    impl_->extension_uris_.clear();
}

std::size_t Parser::declared_extension_uri_count() const noexcept {
    return impl_->extension_uris_.size();
}

std::optional<std::string_view>
Parser::declared_extension_uri(std::uint16_t type) const noexcept {
    auto it = impl_->extension_uris_.find(type);
    if (it == impl_->extension_uris_.end()) return std::nullopt;
    return std::string_view{it->second};
}

// ---------------------------------------------------------------------------
// RTCP helpers (inline in header, but defined here for linkage)
// ---------------------------------------------------------------------------

std::optional<std::uint16_t> seq_distance(std::uint16_t a, std::uint16_t b) noexcept {
    // Distance from b to a (how many steps forward from b to reach a)
    // Handles 16-bit wrap-around
    if (a >= b) {
        return static_cast<std::uint16_t>(a - b);
    } else {
        // Wrap-around case: a < b means we went past 65535
        // Distance = (2^16 - b) + a = 65536 - b + a
        return static_cast<std::uint16_t>(65536u - static_cast<std::uint32_t>(b) + a);
    }
}

bool seq_is_newer(std::uint16_t seq, std::uint16_t prev) noexcept {
    // seq is "newer" than prev if it is ahead, or if it has wrapped around
    // Using the standard WebRTC-style comparison:
    // seq is ahead if (seq - prev) mod 2^16 is in (0, 32768)
    // seq is behind if (seq - prev) mod 2^16 is in (32768, 65535]
    constexpr std::uint16_t half = 32768;
    std::uint16_t diff = static_cast<std::uint16_t>(seq - prev);
    return diff != 0 && diff < half;
}

// ============================================================================
// RTCP parsing — Sender Report / Receiver Report / Generic NACK
// ============================================================================
//
// Helpers local to this TU. Could be lifted to header if reused.

namespace {

[[maybe_unused]]
constexpr std::size_t kRtcpMinBody = 4;  // header + SSRC

inline std::uint16_t read_be16_at(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

inline std::uint32_t read_be32_at(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8)  |
            static_cast<std::uint32_t>(p[3]);
}

[[maybe_unused]]
inline void write_be16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[1] = static_cast<std::uint8_t>(v & 0xFF);
}

inline void write_be32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<std::uint8_t>(v & 0xFF);
}

[[maybe_unused]]
inline void write_be64(std::uint8_t* p, std::uint64_t v) noexcept {
    write_be32(p, static_cast<std::uint32_t>(v >> 32));
    write_be32(p + 4, static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
}

inline std::int32_t sign_extend_24(std::uint32_t x) noexcept {
    // 24-bit two's-complement sign extension
    if (x & 0x800000u) {
        return static_cast<std::int32_t>(x | 0xFF000000u);
    }
    return static_cast<std::int32_t>(x);
}

// Parse the common RTCP header (RFC 3550 §6.1) and return RC + length_words.
// `expected_pt` lets us reject packets with the wrong payload type.
struct RtcpHeaderInfo {
    std::uint8_t  version        = 0;   // expected 2
    std::uint8_t  padding        = 0;   // bit 5 of byte 0
    std::uint8_t  rc_or_fmt      = 0;
    std::uint8_t  payload_type   = 0;
    std::uint16_t length_words   = 0;   // unit: 32-bit words (1-based)
};

core::Result<RtcpHeaderInfo> parse_rtcp_header(
    core::ByteSpan raw,
    std::uint8_t expected_pt,
    std::size_t body_offset_after_header)
{
    if (raw.size() < kRtcpHeaderSize) {
        return core::Result<RtcpHeaderInfo>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP packet too small: " + std::to_string(raw.size()) +
            " < 4 bytes header");
    }
    RtcpHeaderInfo h;
    const std::uint8_t b0 = raw[0];
    h.version        = static_cast<std::uint8_t>((b0 >> 6) & 0x03);
    h.padding        = static_cast<std::uint8_t>((b0 >> 5) & 0x01);
    h.rc_or_fmt      = static_cast<std::uint8_t>(b0 & 0x1F);
    h.payload_type   = raw[1];
    h.length_words   = read_be16_at(raw.data() + 2);

    if (h.version != 2) {
        return core::Result<RtcpHeaderInfo>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP version != 2: got " + std::to_string(h.version));
    }
    if (h.payload_type != expected_pt) {
        return core::Result<RtcpHeaderInfo>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP payload type mismatch: expected " +
            std::to_string(expected_pt) + ", got " +
            std::to_string(h.payload_type));
    }
    // length field is "total length in 32-bit words minus 1".
    const std::size_t total_bytes = static_cast<std::size_t>(h.length_words + 1) * 4u;
    if (total_bytes > raw.size()) {
        return core::Result<RtcpHeaderInfo>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP length field exceeds packet size: length_words=" +
            std::to_string(h.length_words) + " (=" +
            std::to_string(total_bytes) + "B), packet=" +
            std::to_string(raw.size()) + "B");
    }
    if (total_bytes < body_offset_after_header) {
        return core::Result<RtcpHeaderInfo>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP length field shorter than minimum body: length_bytes=" +
            std::to_string(total_bytes) + ", min=" +
            std::to_string(body_offset_after_header));
    }
    return core::Result<RtcpHeaderInfo>::ok(std::move(h));
}

ReportBlock parse_report_block(const std::uint8_t* p) {
    ReportBlock rb;
    rb.ssrc                   = read_be32_at(p);
    rb.fraction_lost          = p[4];
    std::uint32_t lost24      = (static_cast<std::uint32_t>(p[5]) << 16) |
                                (static_cast<std::uint32_t>(p[6]) << 8)  |
                                 static_cast<std::uint32_t>(p[7]);
    rb.cumulative_packets_lost = sign_extend_24(lost24);
    rb.extended_highest_seq   = read_be32_at(p + 8);
    rb.interarrival_jitter    = read_be32_at(p + 12);
    rb.last_sr_ntp            = read_be32_at(p + 16);
    rb.delay_since_last_sr    = read_be32_at(p + 20);
    return rb;
}

[[maybe_unused]]
void serialise_report_block(std::uint8_t* p, const ReportBlock& rb) {
    write_be32(p,      rb.ssrc);
    p[4] = rb.fraction_lost;
    // cumulative_packets_lost is 24-bit two's complement
    std::uint32_t lost24 = static_cast<std::uint32_t>(rb.cumulative_packets_lost) & 0xFFFFFFu;
    p[5] = static_cast<std::uint8_t>((lost24 >> 16) & 0xFF);
    p[6] = static_cast<std::uint8_t>((lost24 >> 8)  & 0xFF);
    p[7] = static_cast<std::uint8_t>(lost24 & 0xFF);
    write_be32(p + 8,  rb.extended_highest_seq);
    write_be32(p + 12, rb.interarrival_jitter);
    write_be32(p + 16, rb.last_sr_ntp);
    write_be32(p + 20, rb.delay_since_last_sr);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public RTCP parse methods — note these are free functions on Parser per
// the header declaration, NOT Parser methods. Actually the header declares
// them as Parser methods. Below they forward to free impls.
// ---------------------------------------------------------------------------

core::Result<SenderReport> Parser::parse_sr(core::ByteSpan raw) const {
    auto h = parse_rtcp_header(raw,
                               static_cast<std::uint8_t>(RtcpType::SR),
                               /*min body*/ kRtcpHeaderSize + 4 + 20);
    if (!h.ok()) {
        return core::Result<SenderReport>::fail(h.error().code(),
                                                 h.error().message());
    }
    if (h.value().rc_or_fmt > 31) {
        return core::Result<SenderReport>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP SR RC out of range: " + std::to_string(h.value().rc_or_fmt));
    }
    if (raw.size() < kRtcpHeaderSize + 4 + 20) {
        return core::Result<SenderReport>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP SR body truncated: " + std::to_string(raw.size()));
    }

    SenderReport sr;
    const std::uint8_t* p = raw.data();
    sr.ssrc                = read_be32_at(p + kRtcpHeaderSize);
    std::uint32_t ntp_hi   = read_be32_at(p + kRtcpHeaderSize + 4);
    std::uint32_t ntp_lo   = read_be32_at(p + kRtcpHeaderSize + 8);
    sr.ntp_timestamp       = (static_cast<std::uint64_t>(ntp_hi) << 32) | ntp_lo;
    sr.rtp_timestamp       = read_be32_at(p + kRtcpHeaderSize + 12);
    sr.sender_packet_count = read_be32_at(p + kRtcpHeaderSize + 16);
    sr.sender_octet_count  = read_be32_at(p + kRtcpHeaderSize + 20);

    const std::uint8_t rc = h.value().rc_or_fmt;
    const std::size_t  need = kRtcpHeaderSize + 4 + 20 +
                              static_cast<std::size_t>(rc) * kReportBlockSize;
    if (raw.size() < need) {
        return core::Result<SenderReport>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP SR report-blocks truncated: need " +
            std::to_string(need) + "B, got " + std::to_string(raw.size()) + "B");
    }
    sr.report_blocks.reserve(rc);
    const std::uint8_t* bp = p + kRtcpHeaderSize + 4 + 20;
    for (std::uint8_t i = 0; i < rc; ++i) {
        sr.report_blocks.push_back(parse_report_block(bp + i * kReportBlockSize));
    }
    return core::Result<SenderReport>::ok(std::move(sr));
}

core::Result<ReceiverReport> Parser::parse_rr(core::ByteSpan raw) const {
    auto h = parse_rtcp_header(raw,
                               static_cast<std::uint8_t>(RtcpType::RR),
                               /*min body*/ kRtcpHeaderSize + 4);
    if (!h.ok()) {
        return core::Result<ReceiverReport>::fail(h.error().code(),
                                                  h.error().message());
    }
    if (h.value().rc_or_fmt > 31) {
        return core::Result<ReceiverReport>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP RR RC out of range: " + std::to_string(h.value().rc_or_fmt));
    }
    if (raw.size() < kRtcpHeaderSize + 4) {
        return core::Result<ReceiverReport>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP RR body truncated: " + std::to_string(raw.size()));
    }
    ReceiverReport rr;
    const std::uint8_t* p = raw.data();
    rr.ssrc = read_be32_at(p + kRtcpHeaderSize);

    const std::uint8_t rc = h.value().rc_or_fmt;
    const std::size_t  need = kRtcpHeaderSize + 4 +
                              static_cast<std::size_t>(rc) * kReportBlockSize;
    if (raw.size() < need) {
        return core::Result<ReceiverReport>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP RR report-blocks truncated: need " +
            std::to_string(need) + "B, got " + std::to_string(raw.size()) + "B");
    }
    rr.report_blocks.reserve(rc);
    const std::uint8_t* bp = p + kRtcpHeaderSize + 4;
    for (std::uint8_t i = 0; i < rc; ++i) {
        rr.report_blocks.push_back(parse_report_block(bp + i * kReportBlockSize));
    }
    return core::Result<ReceiverReport>::ok(std::move(rr));
}

core::Result<NackPacket> Parser::parse_nack(core::ByteSpan raw) const {
    auto h = parse_rtcp_header(raw,
                               static_cast<std::uint8_t>(RtcpType::RTPFB),
                               /*min body*/ kRtcpHeaderSize + 8);
    if (!h.ok()) {
        return core::Result<NackPacket>::fail(h.error().code(),
                                              h.error().message());
    }
    // FMT must be 1 for Generic NACK (RFC 4585 §6.2.1).
    if (h.value().rc_or_fmt !=
        static_cast<std::uint8_t>(RtcpFbKind::GenericNACK)) {
        return core::Result<NackPacket>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP RTPFB FMT != 1 (GenericNACK): got " +
            std::to_string(h.value().rc_or_fmt));
    }
    if (raw.size() < kRtcpHeaderSize + 8) {
        return core::Result<NackPacket>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP NACK body truncated: " + std::to_string(raw.size()));
    }
    NackPacket nk;
    const std::uint8_t* p = raw.data();
    nk.sender_ssrc = read_be32_at(p + kRtcpHeaderSize);
    nk.media_ssrc  = read_be32_at(p + kRtcpHeaderSize + 4);

    const std::size_t fci_bytes = raw.size() - kRtcpHeaderSize - 8;
    if (fci_bytes % kNackFciSize != 0) {
        return core::Result<NackPacket>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP NACK FCI not 4-byte aligned: " +
            std::to_string(fci_bytes) + " bytes");
    }
    const std::size_t n_entries = fci_bytes / kNackFciSize;
    nk.entries.reserve(n_entries);
    const std::uint8_t* fp = p + kRtcpHeaderSize + 8;
    for (std::size_t i = 0; i < n_entries; ++i) {
        NackEntry e;
        e.packet_id = read_be16_at(fp + i * kNackFciSize);
        e.blp       = read_be16_at(fp + i * kNackFciSize + 2);
        nk.entries.push_back(e);
    }
    return core::Result<NackPacket>::ok(std::move(nk));
}

core::Result<void> Parser::parse_abs_send_time_extension(core::ByteSpan d, AbsSendTime& o) const {
    // Per RFC 5285 one-byte-form layout, the on-wire abs-send-time element
    // is 4 bytes total: 1 byte header (id+len nibbles) + 3 data bytes.
    // Accept either the full 4-byte element (most common) or the raw 3-byte
    // payload (advanced callers who already stripped the header).
    if (d.size() == 3) {
        o.send_time_24mhz =
            (static_cast<std::uint32_t>(d[0]) << 16) |
            (static_cast<std::uint32_t>(d[1]) <<  8) |
             static_cast<std::uint32_t>(d[2]);
        return core::Result<void>::make_ok();
    }
    if (d.size() == 4) {
        // Validate the 1-byte header: local id 1 (high nibble), len = 2
        // (low nibble = (3 bytes data) - 1).
        const std::uint8_t hdr = d[0];
        const std::uint8_t id  = (hdr >> 4) & 0x0Fu;
        const std::uint8_t len =  hdr       & 0x0Fu;
        if (id != 1 || len != 2) {
            return core::Result<void>::fail(
                core::ErrorCode::ProtocolError,
                "abs-send-time header invalid: id=" + std::to_string(id) +
                    " len=" + std::to_string(len));
        }
        o.send_time_24mhz =
            (static_cast<std::uint32_t>(d[1]) << 16) |
            (static_cast<std::uint32_t>(d[2]) <<  8) |
             static_cast<std::uint32_t>(d[3]);
        return core::Result<void>::make_ok();
    }
    return core::Result<void>::fail(
        core::ErrorCode::ProtocolError,
        "abs-send-time element must be 3 or 4 bytes; got " +
            std::to_string(d.size()));
}
core::Result<std::size_t> Parser::build_abs_send_time_extension(AbsSendTime t, core::MutableByteSpan out) const {
    if (out.size() < 3) return core::Result<std::size_t>::fail(core::ErrorCode::InvalidArgument, "output too small");
    out[0]=static_cast<std::uint8_t>(t.send_time_24mhz>>16); out[1]=static_cast<std::uint8_t>(t.send_time_24mhz>>8); out[2]=static_cast<std::uint8_t>(t.send_time_24mhz); return core::Result<std::size_t>::ok(3);
}

core::Result<TransportCcFeedback> Parser::parse_transport_cc(core::ByteSpan raw) const {
    // Header: V=2 P=0 FMT=15 PT=205 length=...
    auto h = parse_rtcp_header(raw,
                               static_cast<std::uint8_t>(RtcpType::RTPFB),
                               /*min body*/ kRtcpHeaderSize + 16);
    if (!h.ok()) {
        return core::Result<TransportCcFeedback>::fail(h.error().code(),
                                                      h.error().message());
    }
    if (h.value().rc_or_fmt != 15) {
        return core::Result<TransportCcFeedback>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP RTPFB FMT != 15 (transport-cc): got " +
                std::to_string(h.value().rc_or_fmt));
    }
    if (raw.size() < kRtcpHeaderSize + 16) {
        return core::Result<TransportCcFeedback>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP transport-cc body truncated: " + std::to_string(raw.size()));
    }

    const std::uint8_t* p = raw.data();
    TransportCcFeedback f;
    f.sender_ssrc         = read_be32_at(p + kRtcpHeaderSize);
    f.media_ssrc          = read_be32_at(p + kRtcpHeaderSize + 4);
    f.base_seq            = read_be16_at(p + kRtcpHeaderSize + 8);
    f.packet_status_count = read_be16_at(p + kRtcpHeaderSize + 10);
    // Reference time is a 24-bit field carrying the reference time at 1 kHz
    // tick (RFC 9143 §7.1: lower 8 bits of NTP timestamp at 1ms resolution).
    f.reference_time_24mhz =
        (static_cast<std::uint32_t>(p[kRtcpHeaderSize + 12]) << 16) |
        (static_cast<std::uint32_t>(p[kRtcpHeaderSize + 13]) <<  8) |
         static_cast<std::uint32_t>(p[kRtcpHeaderSize + 14]);

    // Walk packet-status chunks starting after the 16-byte feedback header.
    std::size_t off = kRtcpHeaderSize + 16;
    f.packets.reserve(f.packet_status_count);

    while (f.packets.size() < f.packet_status_count && off + 2 <= raw.size()) {
        const std::uint16_t word = read_be16_at(p + off);
        off += 2;

        const bool T = (word & 0x8000u) != 0;
        if (!T) {
            // Run length chunk: T=0 — 2-bit status + 14-bit run length.
            const auto s = static_cast<PacketStatus>((word >> 13) & 0x3u);
            const std::size_t run = static_cast<std::size_t>(word & 0x1FFFu);
            for (std::size_t i = 0; i < run &&
                   f.packets.size() < f.packet_status_count; ++i) {
                f.packets.push_back({s, 0});
            }
            continue;
        }

        // T=1 chunk.
        const bool S_bit = (word & 0x4000u) != 0;   // 0=1-bit sym; 1=2-bit sym
        const std::size_t symbol_count = static_cast<std::size_t>(word & 0x3FFFu);
        const std::size_t sym_bits = S_bit ? 2u : 1u;
        const std::size_t total_status_bits = symbol_count * sym_bits;

        // Read the packed status list byte-by-byte.
        std::vector<PacketStatus> symbols;
        symbols.reserve(symbol_count);
        std::size_t bits_read = 0;
        while (bits_read < total_status_bits) {
            if (off >= raw.size()) break;
            const std::uint8_t byte = p[off++];
            for (std::uint8_t m = 0; m < 8 && bits_read < total_status_bits;
                 m += static_cast<std::uint8_t>(sym_bits)) {
                std::uint8_t bits;
                if (sym_bits == 1) {
                    bits = (byte >> (7 - m)) & 0x1u;
                } else {
                    bits = (byte >> (6 - m)) & 0x3u;
                }
                symbols.push_back(static_cast<PacketStatus>(bits));
                bits_read += sym_bits;
            }
        }

        // Read deltas (only when S_bit=1): one 14-bit delta per "received"
        // packet (status != NotReceived). NotReceived symbols carry delta=0.
        std::vector<std::int16_t> deltas;
        if (S_bit) {
            deltas.reserve(symbol_count);
            std::size_t bits_left = 0;
            std::uint32_t acc = 0;
            for (std::size_t i = 0; i < symbol_count; ++i) {
                while (bits_left < 14) {
                    if (off >= raw.size()) { off = raw.size(); break; }
                    acc = (acc << 8) | p[off++];
                    bits_left += 8;
                }
                if (bits_left < 14) {
                    deltas.push_back(0);
                    continue;
                }
                const std::uint32_t delta14 =
                    (acc >> (bits_left - 14)) & 0x3FFFu;
                bits_left -= 14;
                acc &= (bits_left == 0) ? 0u
                                        : ((1u << bits_left) - 1u);
                if (symbols[i] == PacketStatus::NotReceived) {
                    deltas.push_back(0);
                } else {
                    // Sign-extend 14-bit to 16-bit.
                    const std::int32_t sign =
                        (delta14 & 0x2000u)
                            ? static_cast<std::int32_t>(delta14 | 0xFFFFC000u)
                            : static_cast<std::int32_t>(delta14);
                    deltas.push_back(static_cast<std::int16_t>(sign));
                }
            }
        }

        for (std::size_t i = 0; i < symbols.size() &&
               f.packets.size() < f.packet_status_count; ++i) {
            f.packets.push_back(
                {symbols[i],
                 S_bit ? deltas[i] : static_cast<std::int16_t>(0)});
        }
    }

    return core::Result<TransportCcFeedback>::ok(std::move(f));
}

} // namespace nimrtc::rtp
