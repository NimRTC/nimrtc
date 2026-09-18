/**
 * @file src/transport/src/webrtc_classic_stack.cpp
 * @brief WebRtcClassicStack + WebRtcClassicStackFactory.
 *
 * Transport PAL Slice 7.5: concrete ITransportStack implementation that
 * wires together the four real plugin backends (ICE + DTLS + RTP + SCTP)
 * into a single bundle, mirroring the composition the engine performs
 * in `engine.cpp::init_modules_once()`.
 *
 * This replaces `CapabilitySelectorStackFactory` (id="default", shell impl
 * from Slice 8) with a real stack. The factory id is "webrtc-classic"
 * so the CapabilitySelector's §5.3 rule 1 ("needs_browser_interop == true")
 * resolves the real factory rather than the shell.
 *
 * ## What this file provides (Slice 7.5)
 *
 * **Lifecycle management** (start / close / tick):
 *   - ICE:     open() + close() via IICETransport
 *   - DTLS:    start() + tick() via IDtlsSession  (pump drives retransmit timer)
 *   - RTP:     open() via IRTP  (stateless)
 *   - SCTP:    no lifecycle — ISctpSocket has no open/close; stub is no-op
 *
 * **Component accessors** (ice() / dtls() / rtp() / sctp() / raw_control()):
 *   The engine continues to own the I/O loop and demux.  Accessors let the
 *   engine reach the resolved concrete components directly without going
 *   through a second registry lookup.  The engine tick loop drives:
 *     ice_t_->recv()   → demux (same as today)
 *     dtls_->feed_inbound() → dtls_->take_outbound() → ice_t_->send()
 *     dtls_->tick() (wired here via stack->tick())
 *
 * **tick()**: calls dtls_->pump() to drive the retransmit timer (RFC 6347
 * §4.2.4).  Safe to call when DTLS state is Connected / Failed / Closed.
 *
 * ## What is NOT in this file (deferred to future slices)
 *
 *   - SCTP lifecycle: `ISctpSocket` has no open/close; the stub is always
 *     ready. v0.11.0 lands `UsrsctpSocket` and extends the interface.
 *   - SRTP install / uninstall: owned by the engine; a future slice can
 *     pull it in by adding `install_srtp(SrtpKeyingMaterial)` to the stack.
 *   - Control stack (`raw_control()`): "webrtc-classic" returns nullptr.
 *     Rule 1 of §5.3 maps `needs_high_freq_control` → "raw-udp-arq"
 *     for the control stack; `RawUdpArqStackFactory` handles that path.
 */

#include <memory>
#include <string_view>
#include <vector>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/plugin_id.hpp>        // NIMRTC_PLUGIN_ID()
#include <nimrtc/core/registry.hpp>         // typed registry slots
#include <nimrtc/dtls/dtls.hpp>            // DtlsConfig, DtlsRole::Server, SrtpProfile (concrete)
#include <nimrtc/dtls/dtls_session_iface.hpp> // IDtlsSession (seam)
#include <nimrtc/plugins/base.hpp>          // BufferView, Status
#include <nimrtc/plugins/ice_transport.hpp>  // IICETransport (full def)
#include <nimrtc/plugins/rtp.hpp>          // IRTP (full def)
#include <nimrtc/sctp/sctp_socket_iface.hpp> // ISctpSocket (seam)
#include <nimrtc/sctp/sctp_socket_factory.hpp> // ISctpSocketFactory, SctpConfig
#include <nimrtc/transport/transport_stack.hpp>

namespace nimrtc::transport {

// ---------------------------------------------------------------------------
// WebRtcClassicStack
//
// Wires ICE + DTLS + RTP + SCTP into one bundle. Owns the component
// lifetimes; close() tears them down in reverse order.
//
// Thread model: all public ITransportStack methods are called from the
// engine thread.  ICE transport's recv() is synchronous (libjuice drains
// the socket on the calling thread), so tick() is also synchronous.
//
// Design notes:
//   - I/O (recv / feed_inbound / take_outbound / send) stays in the engine
//     for now.  The stack owns construction + lifecycle + tick().
//   - SCTP has no open/close on the ISctpSocket seam; the stub is always
//     ready.  v0.11.0 will extend the interface.
// ---------------------------------------------------------------------------
class WebRtcClassicStack final : public ITransportStack {
public:
    explicit WebRtcClassicStack(StackConfig cfg) noexcept
        : cfg_(std::move(cfg)) {}

    // ---------------------------------------------------------------------------
    // ITransportStack: component accessors
    // ---------------------------------------------------------------------------

    plugins::IICETransport& ice() noexcept override { return *ice_; }
    dtls::IDtlsSession&     dtls() noexcept override { return *dtls_; }
    plugins::IRTP&          rtp() noexcept override  { return *rtp_; }
    sctp::ISctpSocket&     sctp() noexcept override { return *sctp_; }

    raw_udp::IRawUdpDatagram* raw_control() noexcept override {
        // "webrtc-classic" does not use the raw-UDP bypass.
        return nullptr;
    }

    // ---------------------------------------------------------------------------
    // ITransportStack: lifecycle
    // ---------------------------------------------------------------------------

    void start() noexcept override {
        NIMRTC_LOG_INFO("transport: WebRtcClassicStack('"
                        << id() << "') starting...");

        // ICE: open first — foundation for everything.
        if (ice_) {
            if (auto st = ice_->open(); st != plugins::kOk) {
                NIMRTC_LOG_ERROR("transport: ICE open failed: "
                                 << plugins::status_string(st));
            }
        }

        // DTLS: start() after ICE so local credentials are available.
        // IDtlsSession::start() is idempotent (second call is a no-op).
        if (dtls_) {
            dtls_->start();
        }

        // RTP: stateless; open() is a no-op.
        if (rtp_) {
            (void)rtp_->open();
        }

        // SCTP: ISctpSocket has no open/close on the seam.  The stub
        // is ready from construction.  v0.11.0 will extend the interface.

        NIMRTC_LOG_INFO("transport: WebRtcClassicStack('"
                        << id() << "') started.");
    }

    void close() noexcept override {
        NIMRTC_LOG_INFO("transport: WebRtcClassicStack('"
                        << id() << "') closing...");

        // Tear down in reverse order: SCTP (no-op) → RTP → DTLS → ICE.
        if (sctp_) { sctp_.reset(); }
        if (rtp_)  { rtp_->close();  rtp_.reset();  }
        if (dtls_) { dtls_.reset(); }   // IDtlsSession has no close() on seam;
                                           // the engine calls dtls->close() directly
        if (ice_)  { ice_->close();  ice_.reset();  }

        NIMRTC_LOG_INFO("transport: WebRtcClassicStack('"
                        << id() << "') closed.");
    }

    std::string_view id() const noexcept override { return "webrtc-classic"; }

    // ---------------------------------------------------------------------------
    // Component setters — called by WebRtcClassicStackFactory after
    // resolving each plugin from the registry.
    // ---------------------------------------------------------------------------
    void set_ice(std::unique_ptr<plugins::IICETransport> p) noexcept {
        ice_ = std::move(p);
    }
    void set_dtls(std::unique_ptr<dtls::IDtlsSession> p) noexcept {
        dtls_ = std::move(p);
    }
    void set_rtp(std::unique_ptr<plugins::IRTP> p) noexcept {
        rtp_ = std::move(p);
    }
    void set_sctp(std::unique_ptr<sctp::ISctpSocket> p) noexcept {
        sctp_ = std::move(p);
    }

    plugins::IICETransport* ice_ptr() const noexcept { return ice_.get(); }

    // ---------------------------------------------------------------------------
    // tick — called by the engine tick loop (~50 ms cadence).
    //
    // Drives the DTLS retransmit timer (RFC 6347 §4.2.4).  wolfSSL's
    // non-blocking DTLS defers retransmits to an external pump; without this
    // a stalled handshake (lost HelloVerifyRequest, lost flight) hangs forever.
    //
    // Safe to call when DTLS state is Connected / Failed / Closed (no-op).
    // ---------------------------------------------------------------------------
    void tick() noexcept {
        if (dtls_) dtls_->pump();
    }

private:
    StackConfig cfg_;

    // Non-null when the factory resolved and injected them.
    std::unique_ptr<plugins::IICETransport> ice_;
    std::unique_ptr<dtls::IDtlsSession>     dtls_;
    std::unique_ptr<plugins::IRTP>          rtp_;
    std::unique_ptr<sctp::ISctpSocket>     sctp_;
};

// ---------------------------------------------------------------------------
// WebRtcClassicStackFactory
//
// Resolves ICE + DTLS + RTP + SCTP factories from the PluginRegistry by
// typed slot, constructs a WebRtcClassicStack, injects the resolved
// components, and returns the wired stack.
//
// Note: the factory does NOT call start() on any component — start() is
// the caller's (the engine's) responsibility via ITransportStack::start().
// This keeps lifecycle ownership in one place (the stack) while still
// letting the engine drive the I/O loop through the component accessors.
// ---------------------------------------------------------------------------

class WebRtcClassicStackFactory final : public ITransportStackFactory {
public:
    std::string_view id() const noexcept override {
        return NIMRTC_PLUGIN_ID(kBackendId);
    }

    std::string_view display_name() const noexcept override {
        return "WebRTC Classic — ICE + DTLS + RTP + SCTP (stub; v0.11 adds usrsctp)";
    }

    std::unique_ptr<ITransportStack>
    create(const StackConfig& cfg) const override {
        auto stack = std::make_unique<WebRtcClassicStack>(cfg);
        auto& reg = core::PluginRegistry::instance();

        // ---- ICE (always required) ------------------------------------------
        {
            const plugins::IICETransportFactory* f =
                reg.get_ice_transport(cfg.ice_id);
            if (!f) {
                NIMRTC_LOG_ERROR("transport: WebRtcClassicStackFactory: "
                                 "ICE factory not found: " << cfg.ice_id);
                return stack;
            }
            stack->set_ice(
                std::unique_ptr<plugins::IICETransport>(f->create_ice()));
            NIMRTC_LOG_INFO("transport: WebRtcClassicStackFactory resolved ICE: "
                            << cfg.ice_id);
        }

        // ---- DTLS (always required) ----------------------------------------
        {
            const dtls::IDtlsSessionFactory* f =
                reg.get_dtls_session(cfg.dtls_id);
            if (!f) {
                NIMRTC_LOG_ERROR("transport: WebRtcClassicStackFactory: "
                                 "DTLS factory not found: " << cfg.dtls_id);
                return stack;
            }
            // Build a default DtlsConfig matching what engine.cpp used pre-Slice 7.5.
            // Slice 7.5 follow-up: read cfg fields to populate role / srtp_profile.
            dtls::Config dcfg;
            dcfg.role         = dtls::DtlsRole::Server;
            dcfg.srtp_profile = dtls::SrtpProfile::Aes128CmSha1_80;
            stack->set_dtls(
                std::unique_ptr<dtls::IDtlsSession>(f->create(dcfg)));
            NIMRTC_LOG_INFO("transport: WebRtcClassicStackFactory resolved DTLS: "
                            << cfg.dtls_id);
        }

        // ---- RTP (always required) ------------------------------------------
        {
            const plugins::IRTPFactory* f = reg.get_rtp(cfg.rtp_id);
            if (!f) {
                NIMRTC_LOG_ERROR("transport: WebRtcClassicStackFactory: "
                                 "RTP factory not found: " << cfg.rtp_id);
                return stack;
            }
            stack->set_rtp(std::unique_ptr<plugins::IRTP>(f->create()));
            NIMRTC_LOG_INFO("transport: WebRtcClassicStackFactory resolved RTP: "
                            << cfg.rtp_id);
        }

        // ---- SCTP (stub; v0.11.0 adds usrsctp) --------------------------
        {
            const sctp::ISctpSocketFactory* f =
                reg.get_sctp_socket(cfg.sctp_id);
            if (!f) {
                NIMRTC_LOG_WARN("transport: WebRtcClassicStackFactory: "
                                "SCTP factory not found: " << cfg.sctp_id
                                << " (stub; v0.11.0 adds usrsctp)");
                return stack;
            }
            sctp::SctpConfig scfg;
            scfg.label = "webrtc-classic-sctp";
            stack->set_sctp(
                std::unique_ptr<sctp::ISctpSocket>(f->create(scfg)));
            NIMRTC_LOG_INFO("transport: WebRtcClassicStackFactory resolved SCTP: "
                            << cfg.sctp_id);
        }

        // Wire ICE error callback so transport errors surface in logs.
        if (stack->ice_ptr()) {
            stack->ice_ptr()->set_callbacks(
                // on_recv: ICE recv is driven by the engine tick (pull model).
                // Slice 7.5 follow-up: expose last_recv_packet() on IICETransport
                // and wire demux here for push model.
                [](plugins::BufferView /*bv*/) {},
                [](plugins::Status err, std::string_view msg) {
                    NIMRTC_LOG_ERROR("transport[ice]: " << msg
                                     << " (status=" << err << ")");
                });
        }

        return stack;
    }

private:
    static constexpr const char* kBackendId = "webrtc-classic";
};

} // namespace nimrtc::transport

// Singleton accessor — used by transport_plugin.cpp's Registrar.
namespace nimrtc::transport::detail {
const ITransportStackFactory*
webrtc_classic_stack_factory_singleton() noexcept {
    static const nimrtc::transport::WebRtcClassicStackFactory s_factory{};
    return &s_factory;
}
} // namespace nimrtc::transport::detail
