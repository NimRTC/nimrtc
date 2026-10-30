/**
 * @file nimrtc/sctp/sctp_socket_iface.hpp
 * @brief ISctpSocket — seam interface for SCTP / PR-SCTP / SCTP-over-DTLS
 *        sockets (Transport PAL Slice 5).
 *
 * Per `docs/plan/transport-selection.md` §5.2 (Slice 5):
 *   - `send_datagram(uint16_t, span)` — unreliable unordered (QUIC datagram
 *     style; best-effort + retransmit cap).
 *   - `send_stream(uint16_t, span)` — reliable ordered (file transfer,
 *     structured logs).
 *   - `send_partial_reliable(uint16_t, span, ttl)` — PR-SCTP with TTL
 *     (drop-oldest-on-limit; teleop control commands).
 *   - `set_on_recv(plugins::OnSctpRecvCb)` — inbound delivery callback,
 *     registered from the engine thread before the first send.
 *
 * Slice 5 status:
 *   This header is the SEAM ONLY. The default impl
 *   (`SctpStubSocket : ISctpSocket`, id="stub") returns
 *   `plugins::kErrNotReady` from every send method and is documented as a
 *   placeholder pending v0.11.0 usrsctp integration per
 *   `docs/plan/transport-selection.md` §7.
 *
 * v0.11.0 work (out of scope for Slice 5) will:
 *   1. Land `UsrsctpSocket : ISctpSocket` (id="usrsctp") as the production
 *      default impl — see `docs/plan/transport-selection.md` §6.2 / §7.
 *   2. Wire it into `nimrtc::sctp::register_default_plugins()` via an
 *      idempotent static-local latch (same pattern as
 *      `nimrtc::ice::register_default_plugins()`).
 *
 * Thread model:
 *   - `set_on_recv` and `send_*` are called from a single thread (the
 *     engine thread) per the IDataChannel contract.
 *   - The on-recv callback fires on the SCTP backend's own thread (usrsctp's
 *     internal UDP-loop thread in v0.11.0); implementations must be
 *     thread-safe with respect to it.
 *   - All methods are `noexcept` per the doc contract.
 *
 * @note P0 scaffold — interface stable; binary layout TBD P4.
 */

#ifndef NIMRTC_SCTP_SCTP_SOCKET_IFACE_HPP
#define NIMRTC_SCTP_SCTP_SOCKET_IFACE_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.

#include <nimrtc/core/api_version.hpp>
#include <chrono>
#include <cstdint>
#include <functional>

#include <nimrtc/plugins/base.hpp>   // BufferView (= core::ByteSpan), kErrNotReady,
                                     // OnSctpRecvCb (promoted in Slice 8)

namespace nimrtc::sctp {

// ---------------------------------------------------------------------------
// ISctpSocket
// ---------------------------------------------------------------------------

/**
 * @brief SCTP socket seam — methods are all `noexcept` per §5.2.
 *
 * The default impl in v0.10.x is `SctpStubSocket` (returns kErrNotReady);
 * v0.11.0 will replace the registration id "stub" with "usrsctp" once
 * `UsrsctpSocket` lands. Until then the seam compiles and registers; the
 * stub is honest about not being ready (sends fail loudly instead of being
 * silently dropped).
 */
class NIMRTC_API_STABLE ISctpSocket {
public:
    virtual ~ISctpSocket() = default;

    // ---- Send side ------------------------------------------------------

    /** Unreliable unordered send (QUIC datagram style).
     *  @param stream  SCTP stream id (0..65534).
     *  @param data    Payload bytes (zero-copy; lifetime must outlive the call).
     *  @return kOk on enqueue success; kErrNotReady / kErrInternal otherwise.
     *  @note noexcept per §5.2 contract. */
    virtual plugins::Status send_datagram(std::uint16_t stream,
                                          plugins::BufferView data) noexcept = 0;

    /** Reliable ordered send. */
    virtual plugins::Status send_stream(std::uint16_t stream,
                                        plugins::BufferView data) noexcept = 0;

    /** Partial-reliable + TTL (PR-SCTP RFC 3758).
     *  @param ttl  Drop the message if it has not been delivered within
     *              `ttl` from send time. Zero/negative = no TTL bound.
     *  @note Used for teleop control (50–100 Hz small packets). */
    virtual plugins::Status send_partial_reliable(
        std::uint16_t stream,
        plugins::BufferView data,
        std::chrono::milliseconds ttl) noexcept = 0;

    // ---- RFC 8831 PPID-aware send surface (v0.14.x DataChannel wiring) ---
    //
    // Non-pure-virtual with safe defaults so existing backends keep
    // compiling: the default routes through the matching legacy send
    // (unordered / ordered / partial-reliable) and ignores `ppid` /
    // `unordered`. Backends that speak real SCTP (usrsctp AF_CONN)
    // override to stamp the wire PPID (DCEP=50, string=51, binary=53).
    //
    // `unordered=true` forces SCTP_UNORDERED regardless of reliability.
    // All methods are `noexcept` per §5.2 contract.
    virtual plugins::Status send_stream_with_ppid(std::uint16_t stream,
                                                  plugins::BufferView data,
                                                  std::uint32_t ppid) noexcept {
        (void)ppid;
        return send_stream(stream, data);
    }
    virtual plugins::Status send_datagram_with_ppid(std::uint16_t stream,
                                                    plugins::BufferView data,
                                                    std::uint32_t ppid) noexcept {
        (void)ppid;
        return send_datagram(stream, data);
    }
    virtual plugins::Status send_partial_reliable_with_ppid(
        std::uint16_t stream,
        plugins::BufferView data,
        std::chrono::milliseconds ttl,
        std::uint32_t ppid,
        bool unordered) noexcept {
        (void)ppid;
        (void)unordered;
        return send_partial_reliable(stream, data, ttl);
    }

    // ---- SCTP-over-DTLS packet transport (RFC 8831 §5) ------------------
    //
    // Non-pure-virtual with safe defaults so AF_INET loopback backends
    // keep compiling. AF_CONN backends override:
    //   - `set_on_packet` — usrsctp `conn_output` trampoline forwards
    //     each outbound SCTP packet here; the engine encrypts it via
    //     IDtlsSession::send_app_data.
    //   - `feed_inbound_packet` — engine feeds DTLS-decrypted SCTP
    //     packets here; the backend passes them to usrsctp_conninput.
    //   - `set_on_recv_with_ppid` — PPID-aware inbound delivery.
    virtual void set_on_packet(plugins::OnSctpPacketCb cb) noexcept {
        (void)cb;
    }
    virtual plugins::Status feed_inbound_packet(
        plugins::BufferView packet) noexcept {
        (void)packet;
        return plugins::kErrNotReady;
    }
    virtual void set_on_recv_with_ppid(
        plugins::OnSctpRecvWithPpidCb cb) noexcept {
        (void)cb;
    }

    // ---- Recv side ------------------------------------------------------

    /** Register the inbound message callback. Must be called before the first
     *  send. Pass an empty std::function to clear.
     *  @note Not thread-safe — call from the engine thread only. */
    virtual void set_on_recv(plugins::OnSctpRecvCb cb) noexcept = 0;
};

} // namespace nimrtc::sctp

#endif // NIMRTC_SCTP_SCTP_SOCKET_IFACE_HPP
