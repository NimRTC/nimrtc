/**
 * @file nimrtc/plugins/rtp.hpp
 * @brief IRTP — pluggable RTP/RTCP packet processing interface.
 *
 * Replace this to use a custom RTP codec, header extension format,
 * or a proprietary framing scheme (e.g. Tencent's RDT).
 *
 * ## Implementing a custom RTP plugin
 *
 * 1. Implement `IRTP` over your RTP payload format.
 * 2. Register: `PluginRegistry::instance().register_rtp("my_rtp", factory);`
 * 3. Set `NimRTCEngine::Config::rtp_name = "my_rtp"`.
 *
 * @note P0 scaffold — interface stable, binary layout TBD P1.
 */

// base.hpp must be before include guard — see transport.hpp for rationale.
#include "nimrtc/plugins/base.hpp"

#ifndef NIMRTC_PLUGINS_RTP_HPP
#define NIMRTC_PLUGINS_RTP_HPP

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// RTP header fields
// ---------------------------------------------------------------------------

/** RFC 3550 RTP fixed header (first 12 bytes), plus CSRC array. */
struct RtpHeader {
    uint8_t  version = 2;
    bool     padding = false;
    bool     has_ext = false;
    uint8_t  csrc_count = 0;
    bool     marker = false;
    uint8_t  payload_type = 0;
    uint16_t seq = 0;
    uint32_t ts = 0;
    uint32_t ssrc = 0;

    /** Optional CSRC list, size == csrc_count. */
    std::vector<uint32_t> csrc;

    /** Extension header, if has_ext. */
    struct Ext {
        uint16_t profile = 0;
        std::vector<uint8_t> data;  // raw extension data
    };
    std::optional<Ext> ext;

    /** Minimum encoded size (fixed header). */
    static constexpr size_t kMinSize = 12;
};

/** Parsed RTP packet with header and payload view. */
struct RtpPacket {
    RtpHeader  header;
    BufferView payload;   // after header + ext
    BufferView raw;       // entire packet (for SRTP)
};

// ---------------------------------------------------------------------------
// RTCP packet types (subset)
// ---------------------------------------------------------------------------

enum class RtcpType : uint8_t {
    SR   = 200,   // Sender Report
    RR   = 201,   // Receiver Report
    SDES = 202,   // Source Description
    BYE  = 203,   // Goodbye
    APP  = 204,   // Application-Defined
    RTPFB= 205,   // Generic RTP Feedback (NACK, etc.)
    PSFB = 206,   // Payload-Specific Feedback (PLI, REMB, etc.)
    XR   = 207,   // Extended Reports
};

/** NACK entry. */
struct RtcpNack {
    uint16_t pid;          // packet ID
    uint16_t blp;          // bitmask of lost packets after pid
};

/** Parsed RTCP compound packet (points into original buffer). */
struct RtcpPacket {
    RtcpType  type;
    BufferView raw;   // entire compound packet
};

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

struct RtpConfig {
    std::string_view name;
    uint32_t local_ssrc = 0;
    std::vector<uint32_t> expected_remote_ssrcs;
    size_t mtu = 1400;
};

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

using RtpRecvCallback    = std::function<void(RtpPacket)>;
using RtcpRecvCallback   = std::function<void(const std::vector<RtcpPacket>&)>;
using RtpParseErrorCallback = std::function<void(Status, std::string_view)>;

// ---------------------------------------------------------------------------
// IRTP
// ---------------------------------------------------------------------------

class IRTP : public IPlugin {
public:
    virtual void set_callbacks(RtpRecvCallback     on_rtp,
                             RtcpRecvCallback     on_rtcp,
                             RtpParseErrorCallback on_error) noexcept = 0;

    /** Parse a raw packet into an RtpPacket.
     *  Returns nullopt on failure (error callback already fired). */
    virtual std::optional<RtpPacket>
    parse_packet(BufferView raw) const noexcept = 0;

    /** Build an outbound RTP packet from header + payload. */
    virtual size_t build_packet(
        const RtpHeader& hdr,
        BufferView payload,
        OutPacket& out) const noexcept = 0;

    /** Parse raw RTCP data into individual packets. */
    virtual std::vector<RtcpPacket>
    parse_rtcp(BufferView raw) const noexcept = 0;

    /** Build an outbound RTCP compound packet. */
    virtual size_t build_rtcp(
        const std::vector<RtcpPacket>& compound,
        OutPacket& out) const noexcept = 0;

    virtual void record_inbound(const RtpPacket& pkt,
                                TimestampUs now_us) noexcept = 0;

    struct Stats {
        uint64_t packets_in   = 0;
        uint64_t bytes_in    = 0;
        uint64_t packets_out = 0;
        uint64_t bytes_out   = 0;
        uint64_t parse_errors= 0;
        uint32_t last_seq    = 0;
        TimestampUs last_recv_us = 0;
    };
    virtual Stats stats() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

class IRTPFactory {
public:
    virtual ~IRTPFactory() = default;
    virtual std::string_view id()          const noexcept = 0;
    virtual std::string_view display_name()const noexcept = 0;
    virtual IRTP* create()                const = 0;
};

template<class T>
class SimpleRTPFactory : public IRTPFactory {
    std::string_view id_;
    std::string_view name_;
public:
    explicit SimpleRTPFactory(std::string_view id, std::string_view name) noexcept
        : id_(id), name_(name) {}
    virtual std::string_view id()          const noexcept override { return id_; }
    virtual std::string_view display_name()const noexcept override { return name_; }
    virtual IRTP* create()                const override { return new T(); }
};

} // namespace nimrtc::plugins
#endif // NIMRTC_PLUGINS_RTP_HPP
