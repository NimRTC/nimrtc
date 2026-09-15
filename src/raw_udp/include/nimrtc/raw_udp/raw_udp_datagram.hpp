/**
 * @file nimrtc/raw_udp/raw_udp_datagram.hpp
 * @brief IRawUdpDatagram — raw UDP bypass seam for high-frequency control.
 *
 * Slice 6 (raw_udp bypass) of the transport selection plan
 * (docs/plan/transport-selection.md §5.2 / §6.3). This header is
 * **purely an interface definition**: it does NOT bind to any concrete
 * default implementation. The default ARQ implementation lives in
 * nimrtc::raw_udp::ArqRawUdp (src/raw_udp/src/arq_raw_udp.cpp).
 *
 * ## Why a raw-UDP bypass at all?
 *
 * WebRTC's SCTP-encapsulated DataChannel is too slow for 50–200 Hz
 * control traffic on a 100 Hz p99 ≤ 80 ms budget (see transport-selection
 * §3). Raw UDP + a custom selective-repeat ARQ + DTLS-PSK (this slice's
 * skeleton) is the fast lane for that traffic. The media plane stays on
 * the existing ICE+DTLS+SCTP path (slice 7's `webrtc-classic` stack).
 *
 * ## Scope of this Slice 6
 *
 * The skeleton provides:
 *   - the IRawUdpDatagram interface (this file);
 *   - a STUB selective-repeat ARQ (arq_raw_udp.cpp) that proves the
 *     interface shape but DOES NOT do real loss recovery, congestion
 *     control, or DTLS-PSK key exchange yet;
 *   - a 100 Hz simulated loopback test that reports p50 / p99 / p999
 *     latency so the skeleton has an observable contract.
 *
 * What the skeleton deliberately defers:
 *   - DTLS-PSK integration — the real PSK session will plug in here once
 *     Slice 4 lands a `IDtlsSession` extension for PSK mode (see
 *     arq_raw_udp.cpp comment block);
 *   - ICE-selected-pair reuse — once Slice 7's `ITransportStack::raw_control()`
 *     exposes the ICE-selected endpoints, ArqRawUdp will skip its own
 *     STUN path and consume the stack's pair (transport-selection §8 #4).
 *
 * What landed in Slice 6.5 (2026-09 follow-up) and is now exercised
 * end-to-end by `tests/test_raw_udp_real_loopback.cpp`:
 *   - Real UDP socket layer (Winsock / Berkeley) — ArqRawUdp opens a
 *     socket, runs a recv thread that demuxes DATA vs ACK frames, and
 *     applies the selective-repeat reception state machine.
 *   - Loss-driven retransmit — the retransmit thread walks tx_pending_
 *     and retransmits any DATA whose RTO has elapsed, with exponential
 *     backoff capped at rto_max_ms, up to max_retransmits.
 *
 * ## Threading
 *
 * Same model as ITransport: send() and on_recv() registration are
 * thread-safe; the ARQ retransmit timer runs on a background thread.
 *
 * @note Forward-looking skeleton. The interface is stable; the
 *       implementation deliberately stops short of production-grade
 *       reliability so we can wire Slice 4 (DTLS-PSK) + Slice 7 (stack
 *       integration) before hardening the ARQ.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/plugins/base.hpp>
#include <nimrtc/plugins/transport.hpp>

// =============================================================================
// Slice-6 forward-compat aliases
// ----------------------------------------------------------------------------//
// The transport-selection plan §5.2 sketches `plugins::Endpoint` and
// `plugins::OnDatagramCb` as the canonical types used by `IRawUdpDatagram`.
// Those types do not yet exist in `plugins/base.hpp` — this slice defines
// them locally as aliases over existing plugin-layer types so the interface
// reads as the doc says it should. A later consolidation ADR can promote
// them to `plugins/base.hpp` once Slice 4 (DTLS-PSK) and Slice 7 (stack
// integration) settle on the final shape.
//
//   Endpoint     — network endpoint (alias of plugins::Addr).
//   OnDatagramCb — per-datagram receive callback (alias of the
//                  per-buffer callback signature used by ITransport).
//
// Implementation note: keeping these as aliases rather than fresh structs
// means slice 6 introduces zero new ABI surface on the plugin layer; the
// skeleton compiles standalone and the eventual rename / redefinition will
// be source-compatible.
// =============================================================================

namespace nimrtc::plugins {

using Endpoint = Addr;  // raw UDP bypass talks to the same opaque address
                        // encoding that ITransport::send / recv already use.

using OnDatagramCb = std::function<void(BufferView)>;

} // namespace nimrtc::plugins

// =============================================================================
// nimrtc::raw_udp — namespace owning the raw-UDP-bypass seam
// ----------------------------------------------------------------------------//
// Placed under a dedicated `raw_udp` namespace rather than `transport`
// so that the slice-6 path (src/raw_udp/) and slice-7 path
// (src/transport/) do not collide on file tree or namespace even when
// parallel subagents land both at the same time (transport-selection
// §11 review checklist final item).
// =============================================================================
namespace nimrtc::raw_udp {

// ---------------------------------------------------------------------------
// Configuration passed to the factory + to open()
// ---------------------------------------------------------------------------
struct RawUdpConfig {
    /** Local bind host ("0.0.0.0" = any). Empty = OS default. */
    std::string local_host;

    /** Local bind port (0 = OS picks). */
    std::uint16_t local_port = 0;

    /** Peer address to send to. Must be set before send(). */
    plugins::Endpoint peer_endpoint{};

    /** ARQ receive window (packets). Larger = more memory, fewer forced
     *  retransmits. Slice 6 default is conservative (32). */
    std::uint16_t rx_window_packets = 32;

    /** Max retransmit attempts per packet before giving up. 0 = unlimited. */
    std::uint16_t max_retransmits = 5;

    /** Retransmit timeout base in milliseconds. RTO doubles on each loss
     *  up to `rto_max_ms`. */
    std::int64_t rto_base_ms = 50;
    std::int64_t rto_max_ms  = 1000;

    /** PSK identity hint — Slice 4 will hand us the actual PSK bytes via
     *  the IDtlsSession PSK extension; for Slice 6 we just store the hint
     *  string so consumers can wire it in once Slice 4 lands. */
    std::string psk_identity_hint;

    /** Enable the real UDP socket layer (slice-6.5 follow-up).
     *
     *  When true:
     *    - open() creates a UDP socket bound to local_host:local_port.
     *    - send() actually `sendto()`s the framed packet to peer_endpoint.
     *    - A background recv thread reads from the socket, demuxes DATA
     *      vs ACK frames, applies the selective-repeat reception state
     *      machine, and `sendto()`s ACK frames back.
     *    - The retransmit thread retransmits unacked DATA frames when
     *      their RTO elapses, with exponential backoff.
     *
     *  When false (default for backward compatibility with the slice-6
     *  in-process tests):
     *    - send() / recv() run against the in-process mutex/queue path
     *      driven by the arq_test_* test hooks.
     *    - No socket is opened; no threads besides the retransmit
     *      timer are spawned.
     */
    bool enable_real_socket = false;
};

// ---------------------------------------------------------------------------
// Stats — produced by the ARQ impl for observability + the Slice-6 test.
// ---------------------------------------------------------------------------
struct RawUdpStats {
    std::uint64_t packets_sent = 0;
    std::uint64_t packets_recv = 0;
    std::uint64_t packets_retransmit = 0;
    std::uint64_t packets_dropped = 0;     // out-of-window / given-up
    std::uint64_t acks_sent = 0;
    std::uint64_t acks_recv = 0;
    std::uint64_t nacks_sent = 0;
    std::uint64_t nacks_recv = 0;
};

// ---------------------------------------------------------------------------
// IRawUdpDatagram — interface
// ---------------------------------------------------------------------------
class IRawUdpDatagram {
public:
    virtual ~IRawUdpDatagram() = default;

    // ---- Plugin lifecycle (matches plugins::IPlugin shape) ----------------

    /** Short human-readable name (e.g. "ArqRawUdp"). */
    virtual const char* name() const noexcept = 0;

    /** Open the UDP socket + start the ARQ retransmit thread.
     *  Idempotent: calling open() twice is a no-op + kOk. */
    virtual plugins::Status open() noexcept = 0;

    /** Close the socket + stop the ARQ thread. Safe to call multiple times. */
    virtual void close() noexcept = 0;

    // ---- Datagram send/recv ----------------------------------------------

    /** Send a datagram to the configured peer_endpoint. The buffer is copied
     *  internally so the caller may free `data` immediately on return.
     *
     *  @return kOk on enqueue success; kErrNotReady if open() has not been
     *          called yet; kErrBufferTooSmall if the ARQ window is full.
     *
     *  @note Thread-safe. */
    virtual plugins::Status send(plugins::Endpoint dst,
                                 std::span<const std::uint8_t> data) noexcept = 0;

    /** Register the receive callback. The callback fires from the ARQ
     *  recv thread on each in-window, in-order delivered datagram. */
    virtual void on_recv(plugins::OnDatagramCb cb) noexcept = 0;

    /** Drain queued received datagrams and invoke on_recv for each. Returns
     *  the number dispatched. Provided so tests can drive a synchronous
     *  loop without spinning a background consumer. */
    virtual int recv() noexcept = 0;

    // ---- Endpoint introspection ------------------------------------------

    /** Bound local endpoint (after open()). May be empty before open(). */
    virtual plugins::Endpoint local_endpoint() const noexcept = 0;

    /** Configured remote endpoint. Empty if not set. */
    virtual plugins::Endpoint remote_endpoint() const noexcept = 0;

    // ---- Stats (observability + test) ------------------------------------

    /** Snapshot of current ARQ stats. Cheap to call. */
    virtual RawUdpStats stats() const noexcept = 0;
};

} // namespace nimrtc::raw_udp
