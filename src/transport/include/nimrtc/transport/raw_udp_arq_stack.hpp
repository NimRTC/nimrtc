/**
 * @file nimrtc/transport/raw_udp_arq_stack.hpp
 * @brief RawUdpArqStack + RawUdpArqStackFactory — the control-plane
 *        `ITransportStack` that exposes a real `IRawUdpDatagram`.
 *
 * Transport PAL Slice 7.6 (transport-selection.md §6.3 follow-up + §5.2
 * Slice 6 wiring). The "raw-udp-arq" stack is the **control plane**
 * counterpart of "webrtc-classic": the engine composes a
 * `TransportSession` whose `media_stack` is `webrtc-classic` (for
 * browser interop) and whose `control_stack` is `raw-udp-arq` (for
 * high-frequency control traffic). See docs/plan/transport-strategies.md
 * §2.3 (agent-gateway), §2.2 (teleop-ultra-low-latency).
 *
 * ## Stack shape
 *
 * The control stack deliberately does **not** own ICE / DTLS / RTP /
 * SCTP. The §5.3 Selector rules use `raw-udp-arq` only for the
 * **control_stack** slot (Rule 1b / Rule 2 / Rule 2 dual); the
 * media_stack is always `webrtc-classic` (Rule 1) or a separate
 * `raw-udp-arq` (Rule 2 — out of v0.12.0 scope; see transport-strategies.md
 * §6 #2 pre-1.0 decision window). This stack therefore returns:
 *   - `raw_control()`         — non-null `IRawUdpDatagram*` (the ARQ session)
 *   - `ice() / dtls() / rtp() / sctp()` — null instances (the engine
 *                            never derefs these; the lifetime is process-static)
 *
 * ## ICE-selected-pair reuse
 *
 * transport-selection.md §8 Open question #4 (deferred to Slice 8 / v1.x):
 * "raw UDP must reuse the ICE selected-pair, not open a second NAT
 * path." For v0.12.0, the `RawUdpArqStack` opens its own UDP socket
 * (the `ArqRawUdp::enable_real_socket=true` path) and does NOT reuse
 * the media stack's ICE pair. This is the v0.11.0 ergonomic-of-isolation
 * choice: the control plane is independent of the media plane's ICE
 * state, so a stalled ICE handshake cannot block control traffic. The
 * trade-off is that the control plane must traverse NAT on its own
 * (the ARQ impl handles this via its own STUN — out of scope for the
 * v0.12.0 minimal impl). v1.0.0 will revisit the ICE pair reuse per
 * the §5.3 follow-up.
 *
 * ## Threading
 *
 * Same model as `ArqRawUdp` (see src/raw_udp/include/.../arq_raw_udp.hpp):
 *   - `send()` and `on_recv()` registration: thread-safe.
 *   - ARQ retransmit timer: background thread inside the ARQ impl.
 *   - `start()` / `close()`: caller-thread; close() joins the
 *     background threads.
 */
#pragma once

#include <memory>
#include <string_view>

#include <nimrtc/transport/transport_stack.hpp>

namespace nimrtc::transport {

// ---------------------------------------------------------------------------
// RawUdpArqStack — control-plane ITransportStack
// ---------------------------------------------------------------------------
class RawUdpArqStack final : public ITransportStack {
public:
    explicit RawUdpArqStack(StackConfig cfg) noexcept;
    ~RawUdpArqStack() override;

    RawUdpArqStack(const RawUdpArqStack&)            = delete;
    RawUdpArqStack& operator=(const RawUdpArqStack&) = delete;

    // ---- ITransportStack: component accessors ------------------------------
    //
    // The control stack does not own ICE / DTLS / RTP / SCTP — the
    // accessors return references to null-instance singletons so the
    // interface is satisfied without a crash. The engine gates every
    // call on the Selector's per-stack id, so it never dereferences
    // these on a raw-udp-arq stack.
    plugins::IICETransport& ice() noexcept override;
    dtls::IDtlsSession&     dtls() noexcept override;
    plugins::IRTP&          rtp() noexcept override;
    sctp::ISctpSocket&      sctp() noexcept override;

    // ---- Weak-ref (v0.13.0) ------------------------------------------------
    // RawUdpArqStack is a control stack; its dtls() returns a null singleton
    // that the engine never dereferences (media_stack owns the real session).
    // We return an empty weak_ptr so dtls_weak().lock() is always nullptr,
    // which is the correct safe behaviour for the control plane.
    std::weak_ptr<dtls::IDtlsSession> dtls_weak() noexcept override {
        return {};
    }

    // The control-plane payload: the ARQ session.
    raw_udp::IRawUdpDatagram* raw_control() noexcept override {
        return raw_control_.get();
    }

    // ---- ITransportStack: lifecycle ----------------------------------------
    void start() noexcept override;
    void close() noexcept override;
    std::string_view id() const noexcept override { return "raw-udp-arq"; }

    // ---- Test-only accessors ------------------------------------------------
    StackConfig config() const noexcept { return cfg_; }

private:
    StackConfig cfg_;
    std::unique_ptr<raw_udp::IRawUdpDatagram> raw_control_;
};

// ---------------------------------------------------------------------------
// RawUdpArqStackFactory — produces RawUdpArqStack instances
// ---------------------------------------------------------------------------
class RawUdpArqStackFactory final : public ITransportStackFactory {
public:
    std::string_view id() const noexcept override;
    std::string_view display_name() const noexcept override;
    std::unique_ptr<ITransportStack> create(
        const StackConfig& cfg) const override;

private:
    static constexpr const char* kBackendId = "raw-udp-arq";
};

}  // namespace nimrtc::transport
