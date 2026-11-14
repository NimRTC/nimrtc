/**
 * @file nimrtc/transport/transport_stack.hpp
 * @brief ITransportStack + ITransportStackFactory + TransportSession.
 *
 * Slice 7 of the transport-selection plan
 * (docs/plan/transport-selection.md §5.2 + §6.4). The Stack + Session
 * seam: a Stack bundles ICE + DTLS + RTP + SCTP + (optional) raw-UDP
 * into a single bind-once, close-once object so the engine doesn't have
 * to wire five plugins together per connection.
 *
 * ----------------------------------------------------------------------------
 * FORWARD-DECLARATION STRATEGY (critical for parallel build)
 * ----------------------------------------------------------------------------
 * Slice 7's seam depends on Slice 4 (IDtlsSession), Slice 5 (ISctpSocket)
 * and Slice 6 (IRawUdpDatagram). To let Slice 7 land and compile BEFORE
 * Slice 4/5/6 are merged, this header uses forward declarations of the
 * Slice-4/5/6 interface types and returns them by reference / raw pointer
 * (the call sites store the raw pointer until the seam is wired into
 * the engine).
 *
 * **IMPORTANT**: Once Slice 4/5/6 land, this header must be updated to
 * include the real headers. The forward declarations below are
 * intentionally missing the inline accessors that need the full type;
 * once the real headers are included those accessors can be added back.
 * ----------------------------------------------------------------------------
 *
 * Slice 7 ships the interface only — it does NOT instantiate default
 * Stack factories.  The Selector therefore looks the factory up by id
 * and constructs an empty `TransportSession` when no factory is
 * registered (the "stack factories pending" case).
 */
#pragma once


// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

// The control-plane seam (remote-desktop plan G2) below uses
// `plugins::Status` and `plugins::OnDatagramCb`, so the plugin base
// header is needed here.  It was previously unnecessary because the
// header only forward-declared the plugin interfaces.
#include <nimrtc/plugins/base.hpp>

namespace nimrtc {

// ---------------------------------------------------------------------------
// Slice 4/5/6 interface FORWARD DECLARATIONS.
//
// These are forward-declared in this header so the transport module can
// compile without pulling in the DTLS / SCTP / raw_udp module headers.
// The concrete types all exist now (DTLS seam = Slice 4, SCTP seam =
// Slice 5, raw UDP = Slice 6); the forward declarations here keep the
// header dependency-free.
// ---------------------------------------------------------------------------
namespace plugins {
class NIMRTC_API_STABLE IICETransport;   // named "plugins::IICETransport" — Slice 4
                       // (already exists, see plugins/ice_transport.hpp)
class NIMRTC_API_STABLE IRTPFactory;
class NIMRTC_API_STABLE IRTP;
} // namespace plugins

namespace dtls {
class NIMRTC_API_STABLE IDtlsSession;
class NIMRTC_API_STABLE IDtlsSessionFactory;
} // namespace dtls

namespace sctp {
class NIMRTC_API_STABLE ISctpSocket;
class NIMRTC_API_STABLE ISctpSocketFactory;
} // namespace sctp

namespace raw_udp {
class NIMRTC_API_STABLE IRawUdpDatagram;
class NIMRTC_API_STABLE IRawUdpFactory;
} // namespace raw_udp

// ---------------------------------------------------------------------------
// StackConfig — names the per-stack backend plugins (Slice 7 reads these
// out of the JSON Profile; Selector passes them to the StackFactory).
// ---------------------------------------------------------------------------
struct NIMRTC_API_STABLE StackConfig {
    std::string ice_id   = "ice";      // plugins::IICETransportFactory id
    std::string dtls_id  = "wolfssl";  // dtls::IDtlsSessionFactory id (Slice 4)
    // sctp::ISctpSocketFactory id (Slice 5).
    //
    // The registered SCTP id is "stub", NOT "usrsctp" — the usrsctp
    // backend was retired from the default registration in v0.11.0
    // (see `src/sctp/src/sctp_stub_factory.hpp` and the SCTP module's
    // `register_default_plugins()`).  The previous default here was
    // "usrsctp", which resolved to no factory at all, so every stack
    // was built with a null SCTP component.
    std::string sctp_id  = "stub";
    // plugins::IRTPFactory id.  The registered RTP id is "webrtc"
    // (see `src/modules/rtp/src/rtp_plugin.cpp`).  The previous default
    // here was "nim", which resolved to no factory, so every stack was
    // built with a null RTP component.
    std::string rtp_id   = "webrtc";
    std::string raw_id;                // raw_udp factory id (Slice 6);
                                       // empty = do not enable raw_control()
};

// ---------------------------------------------------------------------------
// ITransportStack — interface
// ---------------------------------------------------------------------------
//
// One stack = one "bind once, close once" bundle of ICE+DTLS+RTP+SCTP
// (+ optional raw-UDP bypass). The engine holds a TransportSession that
// contains up to two stacks: a `media_stack` (almost always present) and
// a `control_stack` (present when the profile asks for a high-frequency
// control channel separate from media, per §5.3 rule 1).
//
// Accessors return references for plugins the stack always has (ICE/DTLS/
// RTP/SCTP) and a raw pointer (nullptr if not configured) for the optional
// raw-UDP bypass. The returned references are stable for the stack's
// lifetime; consumers MUST NOT cache them past close().
// ---------------------------------------------------------------------------
class NIMRTC_API_STABLE ITransportStack {
public:
    virtual ~ITransportStack() = default;

    // ---- Component access (return references for required components) ---
    //
    // Note: these return pointers rather than references for the
    // forward-declared types because we cannot dereference an incomplete
    // type. Once Slice 4/5/6 land, change the return types to references
    // if the API committee decides references are clearer. The change is
    // source-compatible for callers (a pointer-typed member works as a
    // reference at call sites).

    virtual plugins::IICETransport& ice() noexcept = 0;

    // [[deprecated]] — returns a reference to a component owned by the
    // stack.  Storing the reference past close() is a UAF.  Prefer
    // `dtls_weak()` (v0.13.0) and lock the returned weak_ptr when you
    // need to call session methods.
    //
    // The deprecation is documentation-only at v0.13.0; removal is
    // scheduled for v0.15.0 (six-month deprecation window per the
    // ADR-001 versioning policy).  Engine code does not use this
    // getter anymore.
    virtual dtls::IDtlsSession&      dtls() noexcept = 0;
    virtual plugins::IRTP&          rtp() noexcept = 0;
    virtual sctp::ISctpSocket&       sctp() noexcept = 0;

    // ---- Optional component (nullptr if not configured) -----------------
    virtual raw_udp::IRawUdpDatagram* raw_control() noexcept = 0;

    // ---- The control-plane seam (remote-desktop plan G2) ----------------
    //
    // The remote-desktop profile wants the control plane on its own
    // transport (RD-5): its own ICE-selected pair, its own congestion-
    // control domain, so a saturated video path cannot starve the
    // key/mouse events.
    //
    // The engine may not include the `raw_udp` module's header — that
    // module is not on the ADR-025 exemption list — so it cannot call
    // `raw_control()->send()` / `->on_recv()` directly (the type is
    // only forward-declared here).  These three virtuals are the
    // L4-compliant way for the engine to reach the control path: the
    // *transport* module owns the concrete `IRawUdpDatagram` and
    // forwards through it.
    //
    // Default implementations report "no control path", so a stack that
    // does not implement the control plane (webrtc-classic, and the
    // Slice-8 capability shell) needs no change.
    //
    // `on_control` registers the receive callback.  It fires on the
    // transport's own thread, so the engine's caller must queue the
    // bytes rather than run host code in place.
    virtual bool has_control_path() const noexcept { return false; }

    virtual plugins::Status send_control(
        std::span<const std::uint8_t> data) noexcept {
        (void)data;
        return plugins::kErrUnsupported;
    }

    virtual void on_control(plugins::OnDatagramCb cb) noexcept { (void)cb; }

    // ---- Weak-ref DTLS accessor (v0.13.0, replaces raw-pointer capture)
    //
    // Returns a `weak_ptr` to the DTLS session that the engine stores
    // as `Impl::dtls`.  Unlike `dtls()` (which returns a reference
    // stable only for the stack's lifetime), this `weak_ptr` can be
    // safely `.lock()`-ed after the stack is destroyed:
    //
    //   if (auto s = stack->dtls_weak().lock()) { s->feed_inbound(...); }
    //
    // The weak_ptr auto-expires when the stack's internal shared_ptr
    // releases its last reference (i.e. after `close()`).  v0.12.0's
    // `is_open()` guards on the engine API surface and v0.13.0's
    // `weak_ptr.lock()` checks on the transport-thread surface form a
    // defence-in-depth pair that eliminates the entire UAF class.
    //
    // The old `dtls()` reference getter above is preserved (with a
    // deprecation comment) so existing call sites are not broken — it
    // is NOT safe for engine use; always prefer `dtls_weak()`.
    virtual std::weak_ptr<dtls::IDtlsSession> dtls_weak() noexcept = 0;

    // ---- Lifecycle ------------------------------------------------------
    virtual void start() noexcept = 0;
    virtual void close() noexcept = 0;

    // ---- Diagnostics ----------------------------------------------------
    /** Human-readable name (e.g. "webrtc-classic", "raw-udp-arq"). */
    virtual std::string_view id() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// ITransportStackFactory — interface
// ---------------------------------------------------------------------------
//
// A factory produces a fully-configured ITransportStack. The "default
// impl" factories (WebRtcClassicStackFactory, RawUdpArqStackFactory,
// etc.) land in Slice 7.5 — they depend on Slice 4/5/6 default impls
// being merged first. Slice 7 ships only this interface + Selector.
// ---------------------------------------------------------------------------
class NIMRTC_API_STABLE ITransportStackFactory {
public:
    virtual ~ITransportStackFactory() = default;

    /** Unique id (e.g. "webrtc-classic", "raw-udp-arq"). */
    virtual std::string_view id() const noexcept = 0;

    /** Short human-readable name. */
    virtual std::string_view display_name() const noexcept = 0;

    /** Construct a stack with the given backend selection. */
    virtual std::unique_ptr<ITransportStack> create(
        const StackConfig& cfg) const = 0;
};

// ---------------------------------------------------------------------------
// TransportSession — what the Selector hands back
// ---------------------------------------------------------------------------
//
// Holds up to two stacks (media + control). `start()`/`close()` fan out
// to whichever stacks are non-null. The selector stores the per-stack
// id strings so log lines / error messages can name which stack was
// chosen.
// ---------------------------------------------------------------------------
struct NIMRTC_API_STABLE TransportSession {
    std::unique_ptr<ITransportStack> media_stack;
    std::unique_ptr<ITransportStack> control_stack;     // may be nullptr
    std::string media_stack_id;                         // e.g. "webrtc-classic"
    std::string control_stack_id;                       // e.g. "raw-udp-arq" or ""

    void start() noexcept;
    void close() noexcept;
};

} // namespace nimrtc
