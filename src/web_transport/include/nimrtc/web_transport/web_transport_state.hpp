/**
 * @file nimrtc/web_transport/web_transport_state.hpp
 * @brief WebTransportState — connection-state enum for
 *        IWebTransportSession (v0.14.0 alpha.2, ADR-014).
 *
 * Parallel to `dtls::DtlsState` (the DTLS seam uses an enum of
 * the same shape). The state transitions are documented on
 * `IWebTransportSession::state()` in
 * `web_transport_session_iface.hpp`.
 *
 * @note P0 — added as part of v0.14.0 alpha.2 (the
 *       v1.0.0-alpha.2 pre-release).
 */

#ifndef NIMRTC_WEB_TRANSPORT_STATE_HPP
#define NIMRTC_WEB_TRANSPORT_STATE_HPP

// Stability classification: NIMRTC_API_UNSTABLE — see
// web_transport_session_iface.hpp for the rationale.
#include <nimrtc/core/api_version.hpp>
#include <cstdint>

namespace nimrtc::web_transport {

/**
 * @brief WebTransport / QUIC connection state.
 *
 * The engine polls `IWebTransportSession::state()` from its tick
 * loop at ~50 ms cadence; the state-diff drives the handshake
 * completion callback and the close lifecycle.
 */
enum class NIMRTC_API_UNSTABLE WebTransportState : std::uint8_t {
    /**
     * @brief Connection handshake in progress.
     *
     * Initial state. Transitions to kConnected when the QUIC
     * handshake completes (1-RTT keys derived), or to kFailed on
     * a fatal handshake error.
     */
    kConnecting = 0,

    /**
     * @brief Connection established; streams + datagrams are
     *        usable.
     *
     * Open_bidi_stream / open_uni_stream / send_datagram return
     * non-null / non-error values; inbound datagram callbacks
     * fire.
     */
    kConnected = 1,

    /**
     * @brief close() called; in-flight streams are being torn
     *        down.
     *
     * New stream opens return nullptr; new datagram sends return
     * kErrNotReady. Inbound datagram callbacks stop firing
     * once the QUIC CONNECTION_CLOSE frame is acked.
     */
    kClosing = 2,

    /**
     * @brief Session fully closed; all streams invalidated.
     *
     * Terminal state. The engine may drop its `IWebTransportSession*`
     * reference; calling any method except `state()` is a
     * no-op returning kErrNotReady.
     */
    kClosed = 3,

    /**
     * @brief Fatal error during handshake or steady state.
     *
     * Terminal state. The error is logged with enough detail to
     * diagnose; no recovery path is exposed. The engine may
     * fall back to the SCTP-over-DTLS path (the v0.11.0
     * regression surface) if the profile allows.
     */
    kFailed = 4,
};

} // namespace nimrtc::web_transport

#endif // NIMRTC_WEB_TRANSPORT_STATE_HPP
