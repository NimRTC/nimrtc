/**
 * @file src/modules/rtp/src/builder.cpp
 * @brief RTP PacketBuilder — builds raw RTP packets from fields (RFC 3550).
 *
 * One-shot builder with fluent API. Call setters, then build().
 * Preserves state across builds; call reset() to clear.
 *
 * @note P1 — RFC 3550 + RFC 5285 extensions.
 */

#include <nimrtc/rtp/packet.hpp>

#include <algorithm>
#include <cstring>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/error.hpp>

namespace nimrtc::rtp {

namespace {

// RFC 3550 fixed header size
constexpr std::size_t kFixedHeaderSize = 12;

// Write big-endian uint16 to buffer
inline void write_be16(std::uint8_t* out, std::uint16_t v) noexcept {
    out[0] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    out[1] = static_cast<std::uint8_t>(v & 0xFF);
}

// Write big-endian uint32 to buffer
inline void write_be32(std::uint8_t* out, std::uint32_t v) noexcept {
    out[0] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    out[1] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    out[2] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    out[3] = static_cast<std::uint8_t>(v & 0xFF);
}

// Write big-endian uint64 (used by SR/RR NTP timestamps).
inline void write_be64(std::uint8_t* out, std::uint64_t v) noexcept {
    write_be32(out,     static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFu));
    write_be32(out + 4, static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
}

// Serialise one RFC 3550 §6.4.3 report block. Mirrors the parser-side
// parse_report_block. 24 bytes written.
inline void serialise_report_block(std::uint8_t* p, const ReportBlock& rb) {
    write_be32(p,      rb.ssrc);
    p[4] = rb.fraction_lost;
    // cumulative_packets_lost is 24-bit two's complement.
    const std::uint32_t lost24 =
        static_cast<std::uint32_t>(rb.cumulative_packets_lost) & 0xFFFFFFu;
    p[5] = static_cast<std::uint8_t>((lost24 >> 16) & 0xFF);
    p[6] = static_cast<std::uint8_t>((lost24 >> 8)  & 0xFF);
    p[7] = static_cast<std::uint8_t>(lost24 & 0xFF);
    write_be32(p + 8,  rb.extended_highest_seq);
    write_be32(p + 12, rb.interarrival_jitter);
    write_be32(p + 16, rb.last_sr_ntp);
    write_be32(p + 20, rb.delay_since_last_sr);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// PacketBuilder
// ---------------------------------------------------------------------------

PacketBuilder& PacketBuilder::set_ssrc(std::uint32_t v) noexcept {
    ssrc_ = v;
    return *this;
}

PacketBuilder& PacketBuilder::set_seq(std::uint16_t v) noexcept {
    seq_ = v;
    return *this;
}

PacketBuilder& PacketBuilder::set_timestamp(std::uint32_t v) noexcept {
    timestamp_ = v;
    return *this;
}

PacketBuilder& PacketBuilder::set_payload_type(std::uint8_t v) noexcept {
    payload_type_ = v;
    return *this;
}

PacketBuilder& PacketBuilder::set_marker(bool v) noexcept {
    marker_ = v;
    return *this;
}

PacketBuilder& PacketBuilder::add_csrc(std::uint32_t v) {
    csrc_.push_back(v);
    return *this;
}

PacketBuilder& PacketBuilder::set_extension(std::uint16_t type, core::ByteSpan data) {
    extension_ = Extension{};
    extension_->type = type;
    extension_->data = data;
    return *this;
}

PacketBuilder& PacketBuilder::clear_extension() noexcept {
    extension_ = std::nullopt;
    return *this;
}

PacketBuilder& PacketBuilder::set_payload(core::ByteSpan data) {
    payload_ = data;
    return *this;
}

PacketBuilder& PacketBuilder::set_padding(std::size_t bytes) {
    padding_bytes_ = bytes;
    return *this;
}

std::size_t PacketBuilder::compute_size() const noexcept {
    std::size_t size = kFixedHeaderSize;  // fixed header

    // CSRC list
    size += csrc_.size() * 4;

    // Extension header (4 bytes) + extension data (rounded up to 4-byte boundary)
    if (extension_.has_value()) {
        size += 4;  // 2 bytes profile + 2 bytes length
        // Length field is in 32-bit words, ceiling division
        std::size_t words = (extension_->data.size() + 3) / 4;
        size += words * 4;
    }

    // Padding: per RFC 3550 §5.1 the last byte of the padding region is
    // the count of padding octets that should be ignored (including itself).
    // The parser strips (count + 1) bytes from the end of the packet, treating
    // the count as "number of zero padding data octets preceding the count
    // byte". So set_padding(N) produces N zero bytes followed by a count byte
    // whose value is N, giving N+1 bytes of padding region in total.
    size += payload_.size();  // Payload

    // Padding region (only present when padding_bytes_ > 0).
    if (padding_bytes_ > 0) {
        size += padding_bytes_ + 1;  // N zeros + 1 count byte
    }

    return size;
}

core::ByteBuffer PacketBuilder::build() const {
    core::ByteBuffer out;
    out.resize(compute_size());

    std::uint8_t* buf = out.data();
    std::size_t offset = 0;

    // Byte 0: version=2 (bits 6-7), padding flag, extension flag, CSRC count
    std::uint8_t byte0 = (kVersion << 6);
    if (padding_bytes_ > 0) byte0 |= 0x20;
    if (extension_.has_value()) byte0 |= 0x10;
    byte0 |= static_cast<std::uint8_t>(csrc_.size() & 0x0F);
    buf[offset++] = byte0;

    // Byte 1: marker bit + payload type
    std::uint8_t byte1 = (marker_ ? 0x80 : 0x00) | (payload_type_ & 0x7F);
    buf[offset++] = byte1;

    // Bytes 2-3: sequence number
    write_be16(buf + offset, seq_);
    offset += 2;

    // Bytes 4-7: timestamp
    write_be32(buf + offset, timestamp_);
    offset += 4;

    // Bytes 8-11: SSRC
    write_be32(buf + offset, ssrc_);
    offset += 4;

    // CSRC list
    for (std::uint32_t csrc : csrc_) {
        write_be32(buf + offset, csrc);
        offset += 4;
    }

    // Extension header
    if (extension_.has_value()) {
        // Profile (2 bytes)
        write_be16(buf + offset, extension_->type);
        offset += 2;

        // Length in 32-bit words
        std::size_t ext_words = (extension_->data.size() + 3) / 4;
        write_be16(buf + offset, static_cast<std::uint16_t>(ext_words));
        offset += 2;

        // Extension data (padded to 4-byte boundary)
        std::memcpy(buf + offset, extension_->data.data(), extension_->data.size());
        offset += ext_words * 4;
    }

    // Payload
    if (payload_.data() && !payload_.empty()) {
        std::memcpy(buf + offset, payload_.data(), payload_.size());
        offset += payload_.size();
    }

    // Padding
    // Padding: see compute_size() for the layout reasoning.
    // set_padding(N) → N zero bytes + 1 count byte (value N).
    if (padding_bytes_ > 0) {
        std::memset(buf + offset, 0, padding_bytes_);
        buf[offset + padding_bytes_] = static_cast<std::uint8_t>(padding_bytes_);
        offset += padding_bytes_ + 1;
    }

    return out;
}

void PacketBuilder::reset() noexcept {
    ssrc_ = 0;
    seq_ = 0;
    timestamp_ = 0;
    payload_type_ = 0;
    marker_ = false;
    csrc_.clear();
    extension_ = std::nullopt;
    payload_ = {};
    padding_bytes_ = 0;
}

// ============================================================================
// RTCP builders — Sender Report / Receiver Report / Generic NACK
// ============================================================================

namespace {

inline std::uint8_t rtcp_byte0(std::uint8_t padding,
                               std::uint8_t rc_or_fmt) noexcept {
    // V=2 in bits 6-7
    return static_cast<std::uint8_t>((2u << 6) |
                                     ((padding & 1u) << 5) |
                                     (rc_or_fmt & 0x1Fu));
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// SrBuilder
// ---------------------------------------------------------------------------

SrBuilder& SrBuilder::set_ssrc(std::uint32_t v) noexcept {
    ssrc_ = v; return *this;
}
SrBuilder& SrBuilder::set_ntp_timestamp(std::uint64_t v) noexcept {
    ntp_timestamp_ = v; return *this;
}
SrBuilder& SrBuilder::set_rtp_timestamp(std::uint32_t v) noexcept {
    rtp_timestamp_ = v; return *this;
}
SrBuilder& SrBuilder::set_sender_packet_count(std::uint32_t v) noexcept {
    sender_packet_count_ = v; return *this;
}
SrBuilder& SrBuilder::set_sender_octet_count(std::uint32_t v) noexcept {
    sender_octet_count_ = v; return *this;
}
SrBuilder& SrBuilder::add_report_block(ReportBlock rb) {
    if (report_blocks_.size() >= 31) return *this;  // RC max = 31
    report_blocks_.push_back(std::move(rb));
    return *this;
}

std::size_t SrBuilder::compute_size() const noexcept {
    return kRtcpHeaderSize + 4u /*ssrc*/ + 20u /*sender info*/ +
           report_blocks_.size() * kReportBlockSize;
}

core::ByteBuffer SrBuilder::build() const {
    core::ByteBuffer buf(compute_size(), 0);
    const std::uint8_t rc = static_cast<std::uint8_t>(report_blocks_.size());
    buf[0] = rtcp_byte0(/*padding*/ 0, rc);
    buf[1] = static_cast<std::uint8_t>(RtcpType::SR);
    const std::uint16_t length_words =
        static_cast<std::uint16_t>((buf.size() / 4u) - 1u);
    buf[2] = static_cast<std::uint8_t>((length_words >> 8) & 0xFF);
    buf[3] = static_cast<std::uint8_t>(length_words & 0xFF);

    std::uint8_t* p = buf.data();
    write_be32(p + kRtcpHeaderSize,      ssrc_);
    write_be64(p + kRtcpHeaderSize + 4,  ntp_timestamp_);
    write_be32(p + kRtcpHeaderSize + 12, rtp_timestamp_);
    write_be32(p + kRtcpHeaderSize + 16, sender_packet_count_);
    write_be32(p + kRtcpHeaderSize + 20, sender_octet_count_);

    std::uint8_t* bp = p + kRtcpHeaderSize + 4 + 20;
    for (std::size_t i = 0; i < report_blocks_.size(); ++i) {
        serialise_report_block(bp + i * kReportBlockSize,
                               report_blocks_[i]);
    }
    return buf;
}

void SrBuilder::reset() noexcept {
    ssrc_ = 0; ntp_timestamp_ = 0; rtp_timestamp_ = 0;
    sender_packet_count_ = 0; sender_octet_count_ = 0;
    report_blocks_.clear();
}

// ---------------------------------------------------------------------------
// RrBuilder
// ---------------------------------------------------------------------------

RrBuilder& RrBuilder::set_ssrc(std::uint32_t v) noexcept {
    ssrc_ = v; return *this;
}
RrBuilder& RrBuilder::add_report_block(ReportBlock rb) {
    if (report_blocks_.size() >= 31) return *this;
    report_blocks_.push_back(std::move(rb));
    return *this;
}

std::size_t RrBuilder::compute_size() const noexcept {
    return kRtcpHeaderSize + 4u /*ssrc*/ +
           report_blocks_.size() * kReportBlockSize;
}

core::ByteBuffer RrBuilder::build() const {
    core::ByteBuffer buf(compute_size(), 0);
    const std::uint8_t rc = static_cast<std::uint8_t>(report_blocks_.size());
    buf[0] = rtcp_byte0(0, rc);
    buf[1] = static_cast<std::uint8_t>(RtcpType::RR);
    const std::uint16_t length_words =
        static_cast<std::uint16_t>((buf.size() / 4u) - 1u);
    buf[2] = static_cast<std::uint8_t>((length_words >> 8) & 0xFF);
    buf[3] = static_cast<std::uint8_t>(length_words & 0xFF);

    std::uint8_t* p = buf.data();
    write_be32(p + kRtcpHeaderSize, ssrc_);

    std::uint8_t* bp = p + kRtcpHeaderSize + 4;
    for (std::size_t i = 0; i < report_blocks_.size(); ++i) {
        serialise_report_block(bp + i * kReportBlockSize,
                               report_blocks_[i]);
    }
    return buf;
}

void RrBuilder::reset() noexcept {
    ssrc_ = 0;
    report_blocks_.clear();
}

// ---------------------------------------------------------------------------
// NackBuilder
// ---------------------------------------------------------------------------

NackBuilder& NackBuilder::set_sender_ssrc(std::uint32_t v) noexcept {
    sender_ssrc_ = v; return *this;
}
NackBuilder& NackBuilder::set_media_ssrc(std::uint32_t v) noexcept {
    media_ssrc_ = v; return *this;
}
NackBuilder& NackBuilder::add_entry(std::uint16_t pid, std::uint16_t blp) {
    entries_.push_back(NackEntry{pid, blp});
    return *this;
}

std::size_t NackBuilder::compute_size() const noexcept {
    return kRtcpHeaderSize + 8u /*sender + media ssr*/ +
           entries_.size() * kNackFciSize;
}

core::ByteBuffer NackBuilder::build() const {
    core::ByteBuffer buf(compute_size(), 0);
    // For RTPFB, FMT replaces RC in the same byte. FMT=1 for Generic NACK.
    buf[0] = rtcp_byte0(/*padding*/ 0,
                        /*FMT*/ static_cast<std::uint8_t>(RtcpFbKind::GenericNACK));
    buf[1] = static_cast<std::uint8_t>(RtcpType::RTPFB);
    const std::uint16_t length_words =
        static_cast<std::uint16_t>((buf.size() / 4u) - 1u);
    buf[2] = static_cast<std::uint8_t>((length_words >> 8) & 0xFF);
    buf[3] = static_cast<std::uint8_t>(length_words & 0xFF);

    std::uint8_t* p = buf.data();
    write_be32(p + kRtcpHeaderSize,     sender_ssrc_);
    write_be32(p + kRtcpHeaderSize + 4, media_ssrc_);

    std::uint8_t* fp = p + kRtcpHeaderSize + 8;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        write_be16(fp + i * kNackFciSize,     entries_[i].packet_id);
        write_be16(fp + i * kNackFciSize + 2, entries_[i].blp);
    }
    return buf;
}

void NackBuilder::reset() noexcept {
    sender_ssrc_ = 0;
    media_ssrc_  = 0;
    entries_.clear();
}

} // namespace nimrtc::rtp
