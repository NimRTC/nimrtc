/**
 * @file src/modules/rtp/src/rtp_plugin.cpp
 * @brief PluginAdapter + PluginFactory implementation for nimrtc::rtp.
 *
 * Translates between plugins::IRTP types (RtpHeader, RtpPacket, RtcpPacket)
 * and concrete rtp::PacketView / rtp::RTCP builders/parsers.
 *
 * Per ADR-001 + the MSVC static-link workaround established for audio3a/ice:
 * explicit-registration entry point `register_default_plugins()` forces the
 * .obj (with its registrar) into the consumer's link.
 */

#include <nimrtc/rtp/rtp_plugin.hpp>

#include <atomic>
#include <cstdint>
#include <utility>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>

namespace nimrtc::rtp {

// ---------------------------------------------------------------------------
// Type conversion helpers
// ---------------------------------------------------------------------------

namespace {

/** Convert concrete rtp::PacketView → plugins::RtpPacket (view-conversion).
 *  Both payload and raw fields are non-owning views into the caller-supplied
 *  buffer; lifetime contract is documented in the header. */
plugins::RtpPacket view_to_plugin_packet(const rtp::PacketView& v) noexcept {
    plugins::RtpPacket p;
    p.header.version       = 2;       // rtp::PacketView doesn't store version; RTP v2 only
    p.header.padding       = false;   // parser strips padding; flag not exposed
    p.header.has_ext       = v.extension.has_value();
    p.header.csrc_count    = static_cast<std::uint8_t>(v.csrc.size());
    p.header.marker        = v.marker;
    p.header.payload_type  = v.payload_type;
    p.header.seq           = v.seq;
    p.header.ts            = v.timestamp;
    p.header.ssrc          = v.ssrc;
    p.header.csrc          = v.csrc;
    if (v.extension.has_value()) {
        plugins::RtpHeader::Ext ext;
        ext.profile = v.extension->type;
        // Concrete Extension.data is core::ByteSpan; copy bytes to vector so
        // the plugin-side BufferView (non-owning) can point into it.
        std::vector<std::uint8_t> data(v.extension->data.size());
        if (!v.extension->data.empty()) {
            std::memcpy(data.data(), v.extension->data.data(),
                        v.extension->data.size());
        }
        ext.data = std::move(data);
        p.header.ext = std::move(ext);
    }
    p.payload = v.payload;   // BufferView == core::ByteSpan
    p.raw     = v.raw;
    return p;
}

/** Translate plugins::RtpHeader → rtp::PacketBuilder settings. */
void apply_header_to_builder(const plugins::RtpHeader& h,
                              rtp::PacketBuilder& b) noexcept {
    b.reset();
    b.set_ssrc(h.ssrc)
     .set_seq(h.seq)
     .set_timestamp(h.ts)
     .set_payload_type(h.payload_type)
     .set_marker(h.marker);
    for (auto c : h.csrc) b.add_csrc(c);
    if (h.ext.has_value()) {
        core::ByteSpan data(h.ext->data.data(), h.ext->data.size());
        b.set_extension(h.ext->profile, data);
    }
}

/** Walk an RTCP compound buffer and slice into individual RtcpPackets.
 *  Each RTCP packet has a 4-byte common header: V(2)|P(1)|RC/FMT(5)|PT(8)|length(16).
 *  Length is in 32-bit words minus one. */
std::vector<plugins::RtcpPacket>
scan_rtcp_compound(plugins::BufferView raw) noexcept {
    std::vector<plugins::RtcpPacket> out;
    const std::uint8_t* p   = raw.data();
    std::size_t        off  = 0;
    while (off + 4 <= raw.size()) {
        const std::uint8_t pt = p[off + 1];
        const std::uint16_t length_words =
            static_cast<std::uint16_t>((p[off + 2] << 8) | p[off + 3]);
        const std::size_t pkt_size =
            static_cast<std::size_t>(length_words + 1u) * 4u;
        if (pkt_size < 4 || off + pkt_size > raw.size()) {
            break;   // malformed; stop scanning
        }
        plugins::RtcpPacket rp;
        rp.type = static_cast<plugins::RtcpType>(pt);
        rp.raw  = plugins::BufferView(p + off, pkt_size);
        out.push_back(std::move(rp));
        off += pkt_size;
    }
    if (out.empty() && !raw.empty()) {
        // Fallback: at least one packet with the whole buffer.
        plugins::RtcpPacket rp;
        rp.type = plugins::RtcpType::APP;
        rp.raw  = raw;
        out.push_back(std::move(rp));
    }
    return out;
}

/** Reconstruct one RTCP compound buffer from a list of plugin RtcpPackets. */
core::ByteBuffer
build_rtcp_compound(const std::vector<plugins::RtcpPacket>& pkts) noexcept {
    std::size_t total = 0;
    for (auto& p : pkts) total += p.raw.size();
    core::ByteBuffer out;
    out.resize(total);
    std::size_t off = 0;
    for (auto& p : pkts) {
        if (p.raw.empty()) continue;
        std::memcpy(out.data() + off, p.raw.data(), p.raw.size());
        off += p.raw.size();
    }
    return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// PluginAdapter
// ---------------------------------------------------------------------------

PluginAdapter::PluginAdapter()
    : parser_(std::make_unique<rtp::Parser>()) {
    core::log::Logger::instance().debug(
        "rtp::PluginAdapter created");
}

PluginAdapter::~PluginAdapter() = default;

const char* PluginAdapter::name() const noexcept {
    return "nimrtc::rtp::PluginAdapter (RFC 3550 RTP/RTCP)";
}

plugins::Status PluginAdapter::open() noexcept {
    // RTP is stateless at the network level; nothing to do.
    return plugins::kOk;
}

void PluginAdapter::close() noexcept {
    // No-op.
}

void PluginAdapter::set_callbacks(plugins::RtpRecvCallback       on_rtp,
                                 plugins::RtcpRecvCallback      on_rtcp,
                                 plugins::RtpParseErrorCallback on_error) noexcept {
    on_rtp_   = std::move(on_rtp);
    on_rtcp_  = std::move(on_rtcp);
    on_error_ = std::move(on_error);
}

std::optional<plugins::RtpPacket>
PluginAdapter::parse_packet(plugins::BufferView raw) const noexcept {
    if (raw.empty()) {
        if (on_error_) on_error_(plugins::kErrInvalidParam, "empty RTP buffer");
        ++parse_errors_;
        return std::nullopt;
    }
    core::ByteSpan span = raw;   // same type
    auto res = parser_->parse(span);
    if (!res) {
        if (on_error_) on_error_(plugins::kErrCorrupt, "RTP parse failed");
        ++parse_errors_;
        return std::nullopt;
    }
    auto pkt = view_to_plugin_packet(res.value());
    ++packets_in_;
    bytes_in_ += raw.size();
    last_seq_     = pkt.header.seq;
    last_recv_us_ = static_cast<plugins::TimestampUs>(
                         std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now().time_since_epoch()).count());
    return pkt;
}

std::size_t PluginAdapter::build_packet(const plugins::RtpHeader& hdr,
                                        plugins::BufferView payload,
                                        plugins::OutPacket& out) const noexcept {
    rtp::PacketBuilder b;
    apply_header_to_builder(hdr, b);
    if (!payload.empty()) {
        b.set_payload(core::ByteSpan(payload.data(), payload.size()));
    }
    auto buf = b.build();
    if (buf.empty()) {
        return 0;
    }
    std::uint8_t* dst = out.prepare(buf.size());
    if (!dst) {
        return 0;
    }
    std::memcpy(dst, buf.data(), buf.size());
    out.commit(buf.size());
    ++packets_out_;
    bytes_out_ += buf.size();
    return buf.size();
}

std::vector<plugins::RtcpPacket>
PluginAdapter::parse_rtcp(plugins::BufferView raw) const noexcept {
    return scan_rtcp_compound(raw);
}

std::size_t PluginAdapter::build_rtcp(
    const std::vector<plugins::RtcpPacket>& compound,
    plugins::OutPacket& out) const noexcept {
    if (compound.empty()) return 0;
    auto buf = build_rtcp_compound(compound);
    if (buf.empty()) return 0;
    std::uint8_t* dst = out.prepare(buf.size());
    if (!dst) return 0;
    std::memcpy(dst, buf.data(), buf.size());
    out.commit(buf.size());
    return buf.size();
}

void PluginAdapter::record_inbound(const plugins::RtpPacket& pkt,
                                   plugins::TimestampUs now_us) noexcept {
    ++packets_in_;
    bytes_in_ += pkt.payload.size() + 12;   // approx; exact only on parse
    last_seq_     = pkt.header.seq;
    last_recv_us_ = now_us;
    if (on_rtp_) on_rtp_(pkt);
}

plugins::IRTP::Stats PluginAdapter::stats() const noexcept {
    plugins::IRTP::Stats s;
    s.packets_in    = packets_in_.load(std::memory_order_relaxed);
    s.bytes_in      = bytes_in_.load(std::memory_order_relaxed);
    s.packets_out   = packets_out_.load(std::memory_order_relaxed);
    s.bytes_out     = bytes_out_.load(std::memory_order_relaxed);
    s.parse_errors  = parse_errors_.load(std::memory_order_relaxed);
    s.last_seq      = last_seq_.load(std::memory_order_relaxed);
    s.last_recv_us  = last_recv_us_.load(std::memory_order_relaxed);
    return s;
}

// ---------------------------------------------------------------------------
// PluginFactory
// ---------------------------------------------------------------------------

std::string_view PluginFactory::id() const noexcept {
    return "webrtc";   // matches EngineConfig::rtp_name default
}

std::string_view PluginFactory::display_name() const noexcept {
    return "RTP/RTCP — RFC 3550 (parse + build + SR/RR/NACK)";
}

plugins::IRTP* PluginFactory::create() const {
    return new PluginAdapter();
}

// ---------------------------------------------------------------------------
// Public registration entry point
// ---------------------------------------------------------------------------

namespace detail {

void do_register_default_plugins() noexcept {
    static const struct Registrar {
        Registrar() {
            static nimrtc::rtp::PluginFactory s_factory{};
            nimrtc::core::PluginRegistry::instance().register_rtp(
                std::string_view{s_factory.id()}, &s_factory);
        }
    } s_registrar;
    (void)s_registrar;
}

} // namespace detail

// Non-inline (declared in rtp_plugin.hpp) so the symbol is guaranteed
// in nimrtc_rtp.lib for consumers that link via static lib + PluginRegistry.
void register_default_plugins() noexcept {
    static const int once = []() {
        detail::do_register_default_plugins();
        return 1;
    }();
    (void)once;
}

} // namespace nimrtc::rtp
