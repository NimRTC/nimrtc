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

#include <chrono>
#include <cstdint>
#include <functional>

#include <nimrtc/plugins/base.hpp>   // BufferView (= core::ByteSpan), kErrNotReady, ...

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// Inbound callback (Transport PAL Slice 5 contract).
//
// `stream` is the SCTP stream id (0..65534) the message arrived on;
// `data` is the message payload (zero-copy view; lifetime ends when the
// callback returns).
//
// Documented in §5.2 as `plugins::OnSctpRecvCb`; defined here (rather than
// in a plugins/* header) because the SCTP module owns the seam and we don't
// want to leak transport-specific types into the shared plugins/base.hpp.
// ---------------------------------------------------------------------------
using OnSctpRecvCb = std::function<void(uint16_t stream,
                                        core::ByteSpan data)>;

} // namespace nimrtc::plugins

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
class ISctpSocket {
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

    // ---- Recv side ------------------------------------------------------

    /** Register the inbound message callback. Must be called before the first
     *  send. Pass an empty std::function to clear.
     *  @note Not thread-safe — call from the engine thread only. */
    virtual void set_on_recv(plugins::OnSctpRecvCb cb) noexcept = 0;
};

} // namespace nimrtc::sctp

#endif // NIMRTC_SCTP_SCTP_SOCKET_IFACE_HPP
