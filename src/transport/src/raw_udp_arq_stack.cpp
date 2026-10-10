/**
 * @file src/transport/src/raw_udp_arq_stack.cpp
 * @brief RawUdpArqStack + RawUdpArqStackFactory — the Slice 7.6
 *        control-plane stack factory (transport-selection.md §6.3).
 *
 * Wires `ArqRawUdp` (Slice 6) into the `ITransportStack` seam so the
 * CapabilitySelector's §5.3 rule 1b / rule 2 / rule 4 (high-freq-control
 * variants) can resolve a real factory pointer rather than the
 * Slice 7.5 "webrtc-classic only" world.
 *
 * Design notes:
 *   - The factory is a thin wrapper around `ArqRawUdpFactory` (from
 *     src/raw_udp/include/.../raw_udp_factory.hpp). It does NOT
 *     re-implement the ARQ state machine; it just wraps it.
 *   - The stack's `raw_control_` is created with `enable_real_socket=true`
 *     so the real UDP socket is opened on start(). Tests that want to
 *     use the in-process mutex/queue path (Slice 6.0 tests) can still
 *     construct a `RawUdpArqStack` directly with `enable_real_socket=false`
 *     via a custom factory overload.
 *   - The `ice/dtls/rtp/sctp` accessors return references to
 *     process-static null instances (same pattern as
 *     `CapabilitySelectorStack` in default_transport_stack_factory.cpp).
 */

#include <memory>
#include <string_view>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/plugin_id.hpp>           // NIMRTC_PLUGIN_ID()
#include <nimrtc/core/registry.hpp>            // core::PluginRegistry
#include <nimrtc/dtls/dtls_session_iface.hpp>  // IDtlsSession
#include <nimrtc/plugins/base.hpp>             // BufferView, Status
#include <nimrtc/plugins/ice_transport.hpp>    // IICETransport
#include <nimrtc/plugins/rtp.hpp>              // IRTP
#include <nimrtc/raw_udp/arq_raw_udp.hpp>      // ArqRawUdp / ArqRawUdpFactory
#include <nimrtc/raw_udp/raw_udp_datagram.hpp> // IRawUdpDatagram
#include <nimrtc/sctp/sctp_socket_iface.hpp>   // ISctpSocket
#include <nimrtc/transport/raw_udp_arq_stack.hpp>
#include <nimrtc/transport/transport_stack.hpp>

namespace nimrtc::transport {

// ---------------------------------------------------------------------------
// Null-instance singletons (mirrors CapabilitySelectorStack's pattern).
// ---------------------------------------------------------------------------
namespace detail {

class NullIceTransport final : public plugins::IICETransport {
public:
    const char* name() const noexcept override {
        return "NullIceTransport (raw-udp-arq control stack)";
    }
    plugins::Status open() noexcept override { return plugins::kOk; }
    void close() noexcept override {}
    void set_callbacks(plugins::RecvCallback, plugins::ErrorCallback) noexcept override {}
    plugins::Status send(plugins::BufferView, plugins::Addr) noexcept override {
        return plugins::kErrNotReady;
    }
    int recv() noexcept override { return 0; }
    plugins::Addr local_addr() const noexcept override { return {}; }
    plugins::Addr remote_addr() const noexcept override { return {}; }
    void set_on_consent_lost(plugins::OnConsentLost) noexcept override {}
    plugins::IceState state() const noexcept override { return plugins::IceState::Disconnected; }
    std::string local_ufrag() const noexcept override { return {}; }
    std::string local_password() const noexcept override { return {}; }
    bool wait_for_gathering(int) noexcept override { return false; }
    std::vector<std::string> gathered_local_candidates() const noexcept override { return {}; }
    plugins::Status set_remote_description(std::string_view) noexcept override {
        return plugins::kErrNotReady;
    }
    plugins::Status add_remote_candidate(std::string_view) noexcept override {
        return plugins::kErrNotReady;
    }
    void set_bind_address(std::string_view) noexcept override {}
    void set_stun_server(std::string_view, std::uint16_t) noexcept override {}
    void set_local_port_range(std::uint16_t, std::uint16_t) noexcept override {}
    void add_turn_server(std::string_view, std::uint16_t,
                         std::string_view, std::string_view) noexcept override {}
    void set_bwe(plugins::IBwe*) noexcept override {}
    void set_scheduler(plugins::IScheduler*) noexcept override {}
};

class NullRtp final : public plugins::IRTP {
public:
    const char* name() const noexcept override { return "NullRtp (raw-udp-arq control stack)"; }
    plugins::Status open() noexcept override { return plugins::kOk; }
    void close() noexcept override {}
    void set_callbacks(plugins::RtpRecvCallback, plugins::RtcpRecvCallback,
                       plugins::RtpParseErrorCallback) noexcept override {}
    std::optional<plugins::RtpPacket> parse_packet(plugins::BufferView) const noexcept override {
        return std::nullopt;
    }
    std::size_t build_packet(const plugins::RtpHeader&, plugins::BufferView,
                              plugins::OutPacket&) const noexcept override { return 0; }
    std::vector<plugins::RtcpPacket> parse_rtcp(plugins::BufferView) const noexcept override {
        return {};
    }
    std::size_t build_rtcp(const std::vector<plugins::RtcpPacket>&,
                            plugins::OutPacket&) const noexcept override { return 0; }
    void record_inbound(const plugins::RtpPacket&, plugins::TimestampUs) noexcept override {}
    plugins::IRTP::Stats stats() const noexcept override { return {}; }
};

class NullDtlsSession final : public dtls::IDtlsSession {
public:
    void set_role(dtls::Role) noexcept override {}
    void set_peer_fingerprint(std::span<const std::uint8_t>) noexcept override {}
    void start() noexcept override {}
    void pump() noexcept override {}
    void on_handshake_complete(OnCompleteCb) noexcept override {}
    plugins::Status export_srtp_key_material(std::span<std::uint8_t, 60>) noexcept override {
        return plugins::kErrNotReady;
    }
    core::Result<void> open() noexcept override { return core::Result<void>::make_ok(); }
    void set_role(dtls::DtlsRole) noexcept override {}
    void set_peer_fingerprint(std::string, std::vector<std::uint8_t>) noexcept override {}
    std::size_t feed_inbound(std::span<const std::uint8_t>, const dtls::DtlsAddr&) noexcept override {
        return 0;
    }
    std::vector<dtls::DtlsRecord> take_outbound() noexcept override { return {}; }
    void tick() noexcept override {}
    dtls::DtlsState state() const noexcept override { return dtls::DtlsState::Closed; }
    bool is_connected() const noexcept override { return false; }
    const dtls::Fingerprint& local_fingerprint() const noexcept override {
        return local_fp_;
    }
    std::optional<dtls::SrtpKeyingMaterial> srtp_keying_material() const noexcept override {
        return std::nullopt;
    }
private:
    dtls::Fingerprint local_fp_{};
};

class NullSctpSocket final : public sctp::ISctpSocket {
public:
    plugins::Status send_datagram(std::uint16_t, plugins::BufferView) noexcept override {
        return plugins::kErrNotReady;
    }
    plugins::Status send_stream(std::uint16_t, plugins::BufferView) noexcept override {
        return plugins::kErrNotReady;
    }
    plugins::Status send_partial_reliable(std::uint16_t, plugins::BufferView,
                                          std::chrono::milliseconds) noexcept override {
        return plugins::kErrNotReady;
    }
    void set_on_recv(plugins::OnSctpRecvCb) noexcept override {}
};

inline NullIceTransport&   null_ice()  noexcept { static NullIceTransport   n; return n; }
inline NullRtp&            null_rtp()  noexcept { static NullRtp            n; return n; }
inline NullDtlsSession&    null_dtls() noexcept { static NullDtlsSession    n; return n; }
inline NullSctpSocket&     null_sctp() noexcept { static NullSctpSocket     n; return n; }

}  // namespace detail

// ---------------------------------------------------------------------------
// RawUdpArqStack
// ---------------------------------------------------------------------------
RawUdpArqStack::RawUdpArqStack(StackConfig cfg) noexcept
    : cfg_(std::move(cfg)) {}

RawUdpArqStack::~RawUdpArqStack() {
    close();
}

plugins::IICETransport& RawUdpArqStack::ice() noexcept {
    return detail::null_ice();
}
dtls::IDtlsSession& RawUdpArqStack::dtls() noexcept {
    return detail::null_dtls();
}
plugins::IRTP& RawUdpArqStack::rtp() noexcept {
    return detail::null_rtp();
}
sctp::ISctpSocket& RawUdpArqStack::sctp() noexcept {
    return detail::null_sctp();
}

void RawUdpArqStack::start() noexcept {
    if (raw_control_) {
        NIMRTC_LOG_INFO("transport: RawUdpArqStack start() — already started");
        return;
    }
    NIMRTC_LOG_INFO("transport: RawUdpArqStack('" << id() << "') starting...");

    // Build the ARQ session. We resolve through the typed registry
    // (Slot 6 — register_raw_udp_datagram) so the factory is swappable
    // for testing or alternative ARQ impls.
    auto& reg = core::PluginRegistry::instance();
    const raw_udp::IRawUdpFactory* f = reg.get_raw_udp_datagram(cfg_.raw_id);
    if (!f) {
        // Fall back to id="arq" (the Slice 6 default) if the user
        // left cfg_.raw_id empty.
        f = reg.get_raw_udp_datagram("arq");
    }
    if (!f) {
        NIMRTC_LOG_ERROR("transport: RawUdpArqStack: ARQ factory not found "
                         "(cfg.raw_id='" << cfg_.raw_id << "', fallback 'arq' missing)");
        return;
    }
    raw_control_ = f->create();
    if (auto st = raw_control_->open(); st != plugins::kOk) {
        NIMRTC_LOG_ERROR("transport: RawUdpArqStack: ARQ open failed: "
                         << plugins::status_string(st));
        raw_control_.reset();
        return;
    }
    NIMRTC_LOG_INFO("transport: RawUdpArqStack('" << id()
                    << "') started. ARQ factory: " << f->id());
}

void RawUdpArqStack::close() noexcept {
    if (!raw_control_) {
        return;
    }
    NIMRTC_LOG_INFO("transport: RawUdpArqStack('" << id() << "') closing...");
    raw_control_->close();
    raw_control_.reset();
    NIMRTC_LOG_INFO("transport: RawUdpArqStack('" << id() << "') closed.");
}

// ---------------------------------------------------------------------------
// RawUdpArqStackFactory
// ---------------------------------------------------------------------------
std::string_view RawUdpArqStackFactory::id() const noexcept {
    return NIMRTC_PLUGIN_ID(kBackendId);
}

std::string_view RawUdpArqStackFactory::display_name() const noexcept {
    return "Raw UDP + Selective-Repeat ARQ (control plane; Slice 7.6)";
}

std::unique_ptr<ITransportStack>
RawUdpArqStackFactory::create(const StackConfig& cfg) const {
    return std::make_unique<RawUdpArqStack>(cfg);
}

}  // namespace nimrtc::transport

// Singleton accessor — used by transport_plugin.cpp's Registrar.
namespace nimrtc::transport::detail {
const ITransportStackFactory* raw_udp_arq_stack_factory_singleton() noexcept {
    static const nimrtc::transport::RawUdpArqStackFactory s_factory{};
    return &s_factory;
}
}  // namespace nimrtc::transport::detail
