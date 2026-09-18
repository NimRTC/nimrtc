/**
 * @file src/transport/src/default_transport_stack_factory.cpp
 * @brief CapabilitySelectorStackFactory + CapabilitySelectorStack —
 *        minimal ITransportStackFactory / ITransportStack pair.
 *
 * Transport PAL Slice 8 (v0.10.2) — this file populates the
 * `nimrtc::core::PluginRegistry::register_transport_stack(id, factory*)`
 * hook (see `docs/plan/transport-selection.md` §6.5 follow-up gap) so
 * the Slice 8 engine integration can look up a stack factory by id
 * "default" through the registry.
 *
 * The factory's `create()` returns an ITransportStack that *wraps* the
 * existing CapabilitySelector output. Real stack composition
 * (ICE + DTLS seam + RTP + SCTP, with `raw_control()` pointing at the
 * ARQ bypass) lands in Slice 7.5 — until then this factory produces a
 * stack with null components and start()/close() that log + no-op.
 *
 * ## Why ship a "shell" stack now?
 *
 * Three reasons:
 *   (1) The Slice 8 DoD gate "register_transport_stack hook populated"
 *       needs a concrete factory — a registry slot with zero entries
 *       is functionally equivalent to no hook at all.
 *   (2) The Slice 8 engine integration test (added in this same PR,
 *       see tests/test_engine_plugin_loading.cpp) verifies the
 *       registry hook returns a non-null factory pointer; an empty
 *       slot would silently pass tests even if the hook regressed.
 *   (3) The Selector output is a TransportSession already, so the
 *       "shell" stack is genuinely useful for Selector-driven code
 *       paths (a future Slice 7.5 implementation can replace the
 *       null component returns with real instances without changing
 *       the public interface).
 *
 * ## Reference-return contract
 *
 * `ITransportStack::ice() / dtls() / rtp() / sctp()` return references,
 * not pointers — the transport_stack.hpp design says "always-present
 * components" return references. The shell implementation backs each
 * accessor with a static null-instance (process-lifetime) so the
 * references remain valid for the stack's lifetime. Each null instance
 * implements the full seam interface with no-op / kErrNotReady
 * returns. **Production code MUST NOT dereference these references**
 * — the Slice 8 engine integration gates every call to the components
 * on a non-null check (see engine.cpp). The references exist only to
 * satisfy the interface; calling code that doesn't check for null
 * first would invoke the no-op methods and observe correct-looking
 * "kErrNotReady" / empty-vector / zero-stats returns — this is the
 * "loud-failure-but-don't-crash" mode for "you're using Slice 7.5
 * features before Slice 7.5 lands".
 */

#include <nimrtc/transport/transport_selector.hpp>
#include <nimrtc/transport/transport_stack.hpp>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/plugin_id.hpp>   // Slice 8: NIMRTC_PLUGIN_ID() macro
#include <nimrtc/dtls/dtls_session_iface.hpp>
#include <nimrtc/plugins/base.hpp>
#include <nimrtc/plugins/ice_transport.hpp>
#include <nimrtc/plugins/rtp.hpp>
#include <nimrtc/raw_udp/raw_udp_datagram.hpp>
#include <nimrtc/sctp/sctp_socket_iface.hpp>

#include <chrono>
#include <span>

namespace nimrtc::transport::detail {

// ---- Null ICE transport ---------------------------------------------------
//
// Inherits the full IICETransport surface (which itself virtually
// inherits ITransport). All methods are no-ops or return "not ready";
// the per-method override list mirrors the IICETransport vtable so
// the linker doesn't reject the class for missing overrides.
class NullIceTransport final : public plugins::IICETransport {
public:
    // ---- plugins::IPlugin ---------------------------------------------------
    const char* name() const noexcept override {
        return "NullIceTransport (Slice 8 shell)";
    }
    plugins::Status open() noexcept override { return plugins::kOk; }
    void close() noexcept override {}

    // ---- plugins::ITransport ------------------------------------------------
    void set_callbacks(plugins::RecvCallback /*on_recv*/,
                       plugins::ErrorCallback /*on_error*/) noexcept override {}
    plugins::Status send(plugins::BufferView /*view*/,
                         plugins::Addr /*dst_addr*/) noexcept override {
        return plugins::kErrNotReady;
    }
    int recv() noexcept override { return 0; }
    plugins::Addr local_addr() const noexcept override { return {}; }
    plugins::Addr remote_addr() const noexcept override { return {}; }
    void set_on_consent_lost(plugins::OnConsentLost /*cb*/) noexcept override {}

    // ---- plugins::IICETransport (ICE-specific) ------------------------------
    plugins::IceState state() const noexcept override {
        return plugins::IceState::Disconnected;
    }
    std::string local_ufrag() const noexcept override { return {}; }
    std::string local_password() const noexcept override { return {}; }
    bool wait_for_gathering(int /*timeout_ms*/) noexcept override { return false; }
    std::vector<std::string>
    gathered_local_candidates() const noexcept override { return {}; }
    plugins::Status set_remote_description(
        std::string_view /*ice_block*/) noexcept override {
        return plugins::kErrNotReady;
    }
    plugins::Status add_remote_candidate(
        std::string_view /*candidate_sdp*/) noexcept override {
        return plugins::kErrNotReady;
    }
    void set_bind_address(std::string_view /*host*/) noexcept override {}
    void set_stun_server(std::string_view /*host*/,
                         std::uint16_t /*port*/) noexcept override {}
    void set_local_port_range(std::uint16_t /*begin*/,
                              std::uint16_t /*end*/) noexcept override {}
    void add_turn_server(std::string_view /*host*/,
                         std::uint16_t /*port*/,
                         std::string_view /*user*/,
                         std::string_view /*pass*/) noexcept override {}
    void set_bwe(plugins::IBwe* /*bwe*/) noexcept override {}
    void set_scheduler(plugins::IScheduler* /*sched*/) noexcept override {}
};

// ---- Null RTP -------------------------------------------------------------
//
// Implements every pure-virtual method on plugins::IRTP (which inherits
// IPlugin). Defaults are "kOk" / empty-vector returns; build_packet()
// returns 0 to signal "no bytes written"; parse_packet() returns
// nullopt to signal "malformed". Slice 7.5 will replace this with a
// real RTP impl wired to the IICETransport's recv callback.
class NullRtp final : public plugins::IRTP {
public:
    // ---- plugins::IPlugin ---------------------------------------------------
    const char* name() const noexcept override {
        return "NullRtp (Slice 8 shell)";
    }
    plugins::Status open() noexcept override { return plugins::kOk; }
    void close() noexcept override {}

    // ---- plugins::IRTP ------------------------------------------------------
    void set_callbacks(plugins::RtpRecvCallback /*on_rtp*/,
                       plugins::RtcpRecvCallback /*on_rtcp*/,
                       plugins::RtpParseErrorCallback /*on_error*/) noexcept override {}
    std::optional<plugins::RtpPacket>
    parse_packet(plugins::BufferView /*raw*/) const noexcept override {
        return std::nullopt;
    }
    std::size_t build_packet(const plugins::RtpHeader& /*hdr*/,
                              plugins::BufferView /*payload*/,
                              plugins::OutPacket& /*out*/) const noexcept override {
        return 0;
    }
    std::vector<plugins::RtcpPacket>
    parse_rtcp(plugins::BufferView /*raw*/) const noexcept override {
        return {};
    }
    std::size_t build_rtcp(
        const std::vector<plugins::RtcpPacket>& /*compound*/,
        plugins::OutPacket& /*out*/) const noexcept override {
        return 0;
    }
    void record_inbound(const plugins::RtpPacket& /*pkt*/,
                        plugins::TimestampUs /*now_us*/) noexcept override {}
    plugins::IRTP::Stats stats() const noexcept override { return {}; }
};

// ---- Null DTLS session ---------------------------------------------------
class NullDtlsSession final : public dtls::IDtlsSession {
public:
    void set_role(dtls::Role /*role*/) noexcept override {}
    void set_peer_fingerprint(
        std::span<const std::uint8_t> /*fp*/) noexcept override {}
    void start() noexcept override {}
    void pump() noexcept override {}
    void on_handshake_complete(OnCompleteCb /*cb*/) noexcept override {}
    plugins::Status export_srtp_key_material(
        std::span<std::uint8_t, 60> /*out*/) noexcept override {
        return plugins::kErrNotReady;
    }
};

// ---- Null SCTP socket -----------------------------------------------------
class NullSctpSocket final : public sctp::ISctpSocket {
public:
    plugins::Status send_datagram(std::uint16_t /*stream*/,
                                  plugins::BufferView /*data*/) noexcept override {
        return plugins::kErrNotReady;
    }
    plugins::Status send_stream(std::uint16_t /*stream*/,
                                plugins::BufferView /*data*/) noexcept override {
        return plugins::kErrNotReady;
    }
    plugins::Status send_partial_reliable(
        std::uint16_t /*stream*/,
        plugins::BufferView /*data*/,
        std::chrono::milliseconds /*ttl*/) noexcept override {
        return plugins::kErrNotReady;
    }
    void set_on_recv(plugins::OnSctpRecvCb /*cb*/) noexcept override {}
};

inline NullIceTransport&   null_ice()   noexcept { static NullIceTransport   n; return n; }
inline NullRtp&            null_rtp()   noexcept { static NullRtp            n; return n; }
inline NullDtlsSession&    null_dtls()  noexcept { static NullDtlsSession    n; return n; }
inline NullSctpSocket&     null_sctp()  noexcept { static NullSctpSocket     n; return n; }

} // namespace nimrtc::transport::detail

namespace nimrtc::transport {

// ===========================================================================
// CapabilitySelectorStack — ITransportStack shell backed by null instances.
//
// Documents the Slice 7.5 boundary: every accessor that returns a
// reference hands back a null-instance singleton whose method calls
// return "not ready" status without crashing. Slice 8 consumers MUST
// gate every component call on a non-null check (or, equivalently,
// only invoke when the corresponding slice-7.5 factory has populated
// the stack).
// ===========================================================================
class CapabilitySelectorStack final : public ITransportStack {
public:
    explicit CapabilitySelectorStack(StackConfig cfg, std::string_view id) noexcept
        : cfg_(std::move(cfg)), id_(id) {}

    plugins::IICETransport& ice() noexcept override { return detail::null_ice(); }
    dtls::IDtlsSession&     dtls() noexcept override { return detail::null_dtls(); }
    plugins::IRTP&          rtp() noexcept override  { return detail::null_rtp(); }
    sctp::ISctpSocket&      sctp() noexcept override { return detail::null_sctp(); }

    raw_udp::IRawUdpDatagram* raw_control() noexcept override { return nullptr; }

    void start() noexcept override {
        NIMRTC_LOG_INFO("transport: CapabilitySelectorStack('"
                        << id_ << "') start() — Slice 8 shell; "
                        "real ICE/DTLS/RTP/SCTP composition lands in Slice 7.5");
    }
    void close() noexcept override {
        NIMRTC_LOG_INFO("transport: CapabilitySelectorStack('"
                        << id_ << "') close() — Slice 8 shell");
    }

    std::string_view id() const noexcept override { return id_; }

private:
    StackConfig cfg_;
    std::string id_;
};

// ===========================================================================
// CapabilitySelectorStackFactory — ITransportStackFactory that hands out
// CapabilitySelectorStack instances.
//
// Slice 8 (v0.10.2): registers under id "default" in the
// `core::PluginRegistry::register_transport_stack(...)` slot. Slice 7.5
// will replace this with `WebRtcClassicStackFactory` (id "webrtc-classic")
// + `RawUdpArqStackFactory` (id "raw-udp-arq") — both of which return
// real stacks with populated components. The "default" id is preserved
// for backward-compat (Slice 7 tests rely on it).
// ===========================================================================
class CapabilitySelectorStackFactory final : public ITransportStackFactory {
public:
    std::string_view id() const noexcept override {
        // "default" matches the CapabilitySelector's registration id in
        // transport_selector.cpp — keeps the Slice 7 test contract intact.
        return NIMRTC_PLUGIN_ID(kBackendId);
    }

    std::string_view display_name() const noexcept override {
        return "CapabilitySelectorStack (Slice 8 shell; real composition in Slice 7.5)";
    }

    std::unique_ptr<ITransportStack> create(
        const StackConfig& cfg) const override {
        // Each call returns a fresh shell stack. The returned pointer is
        // non-null so consumers can observe the registry populated; the
        // component accessors return references to null-instance
        // singletons (see file header for the contract).
        return std::make_unique<CapabilitySelectorStack>(cfg, id());
    }

private:
    static constexpr const char* kBackendId = "default";
};

} // namespace nimrtc::transport

// Singleton accessor used by transport_plugin.cpp's Registrar.
// Lives in `nimrtc::transport::detail` to match the forward
// declaration in transport_plugin.cpp.
namespace nimrtc::transport::detail {
const ITransportStackFactory* default_stack_factory_singleton() noexcept {
    static const nimrtc::transport::CapabilitySelectorStackFactory s_factory{};
    return &s_factory;
}
} // namespace nimrtc::transport::detail
