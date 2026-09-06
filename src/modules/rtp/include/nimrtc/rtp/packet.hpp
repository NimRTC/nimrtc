#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/error.hpp>
#include <nimrtc/core/time.hpp>

// =============================================================================
// nimrtc::rtp
// -----------------------------------------------------------------------------
// RTP (RFC 3550) packet parse and build.
// RTCP SR/RR/NACK/REMB types are declared here but not implemented in P0.
// Implementation lands in P1 (Agent 1).
// =============================================================================
namespace nimrtc::rtp {

inline constexpr std::uint8_t kVersion = 2;

// -----------------------------------------------------------------------------
// Extension element (RFC 5285)
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// Extension element (RFC 5285). abs-send-time and transport-wide-CC
// (RFC 9143) are layered on top of this; see parse_*_extension below.
// -----------------------------------------------------------------------------
struct Extension {
    std::uint16_t type = 0;          // 1..14 for one-byte form; 1..255 for two-byte
    core::ByteSpan data{};           // payload bytes (NOT including the type byte)
};

// -----------------------------------------------------------------------------
// RFC 9143 abs-send-time (one-byte-form, extension id = 1).
// 24-bit absolute timestamp at 24 MHz; wrapping is handled by the
// assignment operator (caller may store the 32-bit-aligned value here).
// -----------------------------------------------------------------------------
struct AbsSendTime {
    std::uint32_t send_time_24mhz = 0;
};

// -----------------------------------------------------------------------------
// RFC 9143 transport-wide CC feedback packet (PT = RTPFB, FMT = 15).
// `packets` has length = packet_status_count; one element per RTP packet,
// in seq order starting at base_seq (modulo 2^16).
// -----------------------------------------------------------------------------
enum class PacketStatus : std::uint8_t {
    NotReceived   = 0,
    Received      = 1,
    ReceivedSmall = 2,
};
struct TransportCcPacketStatus {
    PacketStatus status    = PacketStatus::NotReceived;
    std::int16_t delta_250us = 0;  // recv-time delta in 250-µs ticks vs reference time
                                   // (only meaningful when status != NotReceived)
};
struct TransportCcFeedback {
    std::uint32_t sender_ssrc = 0;
    std::uint32_t media_ssrc  = 0;
    std::uint16_t base_seq    = 0;
    std::uint16_t packet_status_count = 0;
    std::uint32_t reference_time_24mhz = 0;  // 24-bit wrap timestamp at 1 kHz / 24 kHz
    std::vector<TransportCcPacketStatus> packets;
};


// -----------------------------------------------------------------------------
// PacketView — non-owning view into a parsed RTP packet.
// The caller must keep the underlying bytes alive for as long as the view is used.
// -----------------------------------------------------------------------------
struct PacketView {
    core::ByteSpan raw{};            // entire packet including header
    core::ByteSpan payload{};        // bytes after header+CSRC+extension

    std::uint32_t ssrc            = 0;
    std::uint16_t seq             = 0;
    std::uint32_t timestamp       = 0;
    std::uint8_t  payload_type    = 0;
    bool          marker          = false;

    std::vector<std::uint32_t> csrc;          // contributing source IDs
    std::optional<Extension>   extension;     // RFC 5285 header extension (at most one)

    // Parsed extension elements (RFC 5285). Populated when at least one
    // extension URI has been declared via Parser::declare_extension_uri();
    // otherwise left empty and only `extension.data` carries raw bytes.
    //
    //   extmap_uri[id] -> the URI declared for that extension id, or "" if
    //   the id has no declared URI.
    std::map<std::uint16_t, std::string> extmap_uri;

    // Capture-time / arrival-time metadata (set by caller; default unset).
    // §8.4 hook: arrival is the monotonic-clock time the packet was *received*
    // by the network thread, before any queueing.
    std::optional<core::TimePoint> arrival;
};

// -----------------------------------------------------------------------------
// Forward declarations for RTCP types — the full definitions appear below
// (after the Parser class). Doing it this way lets Parser methods reference
// SenderReport / ReceiverReport / NackPacket in their signatures without
// forcing the structs to come first.
// -----------------------------------------------------------------------------
struct SenderReport;
struct ReceiverReport;
struct NackPacket;

// -----------------------------------------------------------------------------
// Parser — parses a raw RTP packet or RTCP compound packet into the matching
// view/struct. Stateless; returns ProtocolError on malformed input.
// -----------------------------------------------------------------------------
class Parser {
public:
    Parser();

    // Out-of-line destructor (pImpl idiom): must be where Impl is complete so
    // that ~unique_ptr<Impl> compiles in TUs that only see the header.
    ~Parser();

    // RTP packet parse (RFC 3550 + RFC 5285 extensions).
    // Sequence-number ordering / jitter validation lives in modules/jb.
    core::Result<PacketView> parse(core::ByteSpan raw) const;

    // Extension URI registry (RFC 5285).
    void declare_extension_uri(std::uint16_t type, std::string_view uri);
    void clear_extension_uris();
    std::size_t declared_extension_uri_count() const noexcept;
    std::optional<std::string_view>
        declared_extension_uri(std::uint16_t type) const noexcept;

    // RTCP parse methods (RFC 3550 §6).
    core::Result<SenderReport>   parse_sr  (core::ByteSpan raw) const;
    core::Result<ReceiverReport> parse_rr  (core::ByteSpan raw) const;
    core::Result<NackPacket>     parse_nack(core::ByteSpan raw) const;
    core::Result<TransportCcFeedback> parse_transport_cc(core::ByteSpan raw) const;
    core::Result<void> parse_abs_send_time_extension(core::ByteSpan ext_data, AbsSendTime& out) const;
    core::Result<std::size_t> build_abs_send_time_extension(AbsSendTime t, core::MutableByteSpan out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// -----------------------------------------------------------------------------
// PacketBuilder — builds raw RTP packets (one-shot, call reset() to reuse).
// -----------------------------------------------------------------------------
class PacketBuilder {
public:
    PacketBuilder& set_ssrc(std::uint32_t v) noexcept;
    PacketBuilder& set_seq(std::uint16_t v) noexcept;
    PacketBuilder& set_timestamp(std::uint32_t v) noexcept;
    PacketBuilder& set_payload_type(std::uint8_t v) noexcept;
    PacketBuilder& set_marker(bool v) noexcept;
    PacketBuilder& add_csrc(std::uint32_t v);

    PacketBuilder& set_extension(std::uint16_t type, core::ByteSpan data);
    PacketBuilder& set_abs_send_time(AbsSendTime t) noexcept;
    PacketBuilder& clear_extension() noexcept;
    PacketBuilder& set_payload(core::ByteSpan data);
    PacketBuilder& set_padding(std::size_t bytes);

    // Serialise into a fresh buffer. Internal state is preserved; call
    // reset() explicitly if you want to discard.
    core::ByteBuffer build() const;

    // Compute serialised size without actually building. Useful for sizing
    // output buffers in tight paths.
    std::size_t compute_size() const noexcept;

    void reset() noexcept;

private:
    std::uint32_t ssrc_            = 0;
    std::uint16_t seq_             = 0;
    std::uint32_t timestamp_       = 0;
    std::uint8_t  payload_type_    = 0;
    bool          marker_          = false;
    std::vector<std::uint32_t> csrc_;
    std::optional<Extension> extension_;
    std::optional<AbsSendTime> abs_send_time_;
    core::ByteSpan payload_{};
    std::size_t    padding_bytes_  = 0;
};

// -----------------------------------------------------------------------------
// RTCP — types and minimal types declared here; full implementation in P1.
// -----------------------------------------------------------------------------
enum class RtcpType : std::uint8_t {
    SR     = 200,
    RR     = 201,
    SDES   = 202,
    BYE    = 203,
    APP    = 204,
    RTPFB  = 205,    // generic NACK, PLI, FIR, etc. (RFC 4585)
    PSFB   = 206,    // REMB, AFB (RFC 4585)
    XR     = 207,    // extended reports (RFC 3611)
};

// FCI (feedback control information) kinds for RTPFB:
//   NACK (1), TMMBR/TMMBN (3), TSTR (4), TSTN (5), PLI (6), SLI (7), FIR (8).
enum class RtcpFbKind : std::uint8_t {
    GenericNACK = 1,
    PLI         = 6,
    SLI         = 7,
    FIR         = 8,
};

// FCI kinds for PSFB:
//   PLI, SLI, FIR (same as above), REMB (15).
enum class RtcpPsFbKind : std::uint8_t {
    REMB = 15,
};

// -----------------------------------------------------------------------------
// Helpers — small wrappers used by NACK/REMB code (P1).
// -----------------------------------------------------------------------------
std::optional<std::uint16_t> seq_distance(std::uint16_t a, std::uint16_t b) noexcept;
bool seq_is_newer(std::uint16_t seq, std::uint16_t prev) noexcept;

// =============================================================================
// RTCP — Sender Report / Receiver Report / Generic NACK
// =============================================================================
// All RTCP compound packets share the same fixed 4-byte header (RFC 3550 §6.1):
//
//   0                   1                   2                   3
//   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |V=2|P|    RC   |       PT      |             length            |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
//   V=2   version
//   P     padding flag (byte-stuffed, last byte = count)
//   RC    report count for SR/RR; FMT for RTPFB/PSFB; 0 otherwise
//   PT    payload type (200 SR, 201 RR, 205 RTPFB, 206 PSFB)
//   length packet length in 32-bit words, MINUS ONE (i.e. header only → 1)

inline constexpr std::size_t kRtcpHeaderSize    = 4;
inline constexpr std::size_t kReportBlockSize   = 24;
inline constexpr std::size_t kNackFciSize       = 4;

// -----------------------------------------------------------------------------
// Report block (RFC 3550 §6.4.3) — appears 0..31 times in SR and RR bodies.
// -----------------------------------------------------------------------------
struct ReportBlock {
    std::uint32_t ssrc                   = 0;    // SSRC of source this report describes
    std::uint8_t  fraction_lost          = 0;    // 0..255; uint8 since RFC stores 8 bits
    std::int32_t  cumulative_packets_lost = 0;   // 24-bit signed, sign-extended
    std::uint32_t extended_highest_seq   = 0;    // low 16 bits = highest seq; high 16 = cycles
    std::uint32_t interarrival_jitter    = 0;
    std::uint32_t last_sr_ntp            = 0;    // middle 32 bits of last SR's NTP timestamp
    std::uint32_t delay_since_last_sr    = 0;    // 1/65536-second units
};

// -----------------------------------------------------------------------------
// Sender Report (RFC 3550 §6.4.1, PT=200).
// Header + 20B sender-info + RC * 24B report-blocks.
// -----------------------------------------------------------------------------
struct SenderReport {
    std::uint32_t              ssrc                = 0;
    std::uint64_t              ntp_timestamp       = 0;     // full 64-bit NTP timestamp
    std::uint32_t              rtp_timestamp       = 0;
    std::uint32_t              sender_packet_count = 0;
    std::uint32_t              sender_octet_count  = 0;
    std::vector<ReportBlock>   report_blocks;
};

// -----------------------------------------------------------------------------
// Receiver Report (RFC 3550 §6.4.2, PT=201).
// Header + 4B SSRC + RC * 24B report-blocks.
// -----------------------------------------------------------------------------
struct ReceiverReport {
    std::uint32_t              ssrc        = 0;
    std::vector<ReportBlock>   report_blocks;
};

// -----------------------------------------------------------------------------
// Generic NACK (RFC 4585 §6.2.1, FMT=1, PT=205).
// Header (with FMT=1) + 4B sender-SSRC + 4B media-SSRC + N * 4B FCI.
//
// Each FCI entry:
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//   |            PID                |             BLP                |
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//   PID = seq of a lost packet.
//   BLP = bitmask of the next 16 packets (bit 0 = PID+1, bit 15 = PID+16).
// -----------------------------------------------------------------------------
struct NackEntry {
    std::uint16_t packet_id = 0;
    std::uint16_t blp       = 0;
};

struct NackPacket {
    std::uint32_t          sender_ssrc = 0;     // SSRC of the feedback sender
    std::uint32_t          media_ssrc  = 0;     // SSRC of the media source being ack'd
    std::vector<NackEntry> entries;
};

// -----------------------------------------------------------------------------
// Builders — one-shot. Chain setters, then call build() to serialise.
// Header-only SR (RC=0) is legal; builder enforces RC ≤ 31.
// -----------------------------------------------------------------------------
class SrBuilder {
public:
    SrBuilder& set_ssrc(std::uint32_t v) noexcept;
    SrBuilder& set_ntp_timestamp(std::uint64_t v) noexcept;
    SrBuilder& set_rtp_timestamp(std::uint32_t v) noexcept;
    SrBuilder& set_sender_packet_count(std::uint32_t v) noexcept;
    SrBuilder& set_sender_octet_count(std::uint32_t v) noexcept;
    SrBuilder& add_report_block(ReportBlock rb);

    core::ByteBuffer build() const;
    std::size_t     compute_size() const noexcept;
    void            reset() noexcept;

private:
    std::uint32_t              ssrc_                = 0;
    std::uint64_t              ntp_timestamp_       = 0;
    std::uint32_t              rtp_timestamp_       = 0;
    std::uint32_t              sender_packet_count_ = 0;
    std::uint32_t              sender_octet_count_  = 0;
    std::vector<ReportBlock>   report_blocks_;
};

class RrBuilder {
public:
    RrBuilder& set_ssrc(std::uint32_t v) noexcept;
    RrBuilder& add_report_block(ReportBlock rb);

    core::ByteBuffer build() const;
    std::size_t     compute_size() const noexcept;
    void            reset() noexcept;

private:
    std::uint32_t              ssrc_ = 0;
    std::vector<ReportBlock>   report_blocks_;
};

class NackBuilder {
public:
    NackBuilder& set_sender_ssrc(std::uint32_t v) noexcept;
    NackBuilder& set_media_ssrc(std::uint32_t v) noexcept;
    NackBuilder& add_entry(std::uint16_t pid, std::uint16_t blp);

    core::ByteBuffer build() const;
    std::size_t     compute_size() const noexcept;
    void            reset() noexcept;

private:
    std::uint32_t          sender_ssrc_ = 0;
    std::uint32_t          media_ssrc_  = 0;
    std::vector<NackEntry> entries_;
};

} // namespace nimrtc::rtp