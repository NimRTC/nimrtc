/**
 * @file nimrtc/web_transport/web_transport_config.hpp
 * @brief WebTransportConfig — WebTransport / QUIC connection
 *        configuration (v0.14.0 alpha.2, ADR-014).
 *
 * Mirrors the QUIC settings that msquic (the chosen v0.14.0-beta.1
 * backend) exposes through `QUIC_SETTINGS` + `QUIC_CONFIGURATION`
 * parameters; the field set is intentionally small and
 * transport-portable (does not leak any msquic-specific types
 * into the public surface).
 *
 * Default values match the RFC 9000 / RFC 9221 default QUIC
 * configuration; msquic's defaults are slightly tighter, so v0.x
 * callers that rely on the defaults see the more permissive
 * RFC values. Custom values are layered on top by the
 * `WebTransportMsQuicSession` impl (beta.1) when constructing
 * the `QUIC_SETTINGS` struct.
 *
 * @note P0 — added as part of v0.14.0 alpha.2 (the
 *       v1.0.0-alpha.2 pre-release).
 */

#ifndef NIMRTC_WEB_TRANSPORT_CONFIG_HPP
#define NIMRTC_WEB_TRANSPORT_CONFIG_HPP

// Stability classification: NIMRTC_API_UNSTABLE — see
// web_transport_session_iface.hpp for the rationale.
#include <cstdint>

namespace nimrtc::web_transport {

/**
 * @brief WebTransport / QUIC connection configuration.
 *
 * The `IWebTransportSessionFactory::create()` factory method
 * (lands in beta.1) consumes this struct when constructing the
 * underlying msquic `QUIC_CONFIGURATION` handle.
 */
struct WebTransportConfig {
    /**
     * @brief Maximum idle timeout in milliseconds.
     *
     * After this many milliseconds of no activity, the connection
     * is closed by the idle-timeout mechanism (RFC 9000 §10.1).
     * Default: 30 000 ms (30 s). Range: [1 000, 600 000].
     *
     * msquic mapping: `IdleTimeoutMs` in `QUIC_SETTINGS`.
     */
    std::uint32_t max_idle_timeout_ms = 30'000;

    /**
     * @brief Initial per-stream flow-control window in bytes.
     *
     * Initial value for the per-stream data limit (RFC 9000
     * §19.10). The peer may raise this via `MAX_STREAM_DATA`
     * frames; the local limit is also raised automatically as
     * the stream consumes its window. Default: 65 536 (64 KiB).
     *
     * msquic mapping: `InitialStreamWindowSize` in
     * `QUIC_SETTINGS`.
     */
    std::uint32_t initial_max_stream_data = 65'536;

    /**
     * @brief Initial per-connection flow-control window in bytes.
     *
     * Initial value for the per-connection data limit (RFC 9000
     * §19.9). The peer may raise this via `MAX_DATA` frames; the
     * local limit is also raised automatically as the connection
     * consumes its window. Default: 1 048 576 (1 MiB).
     *
     * msquic mapping: `InitialConnectionWindowSize` in
     * `QUIC_SETTINGS`.
     */
    std::uint32_t initial_max_data = 1'048'576;

    /**
     * @brief Keep-alive interval in milliseconds.
     *
     * If the connection is idle for this many milliseconds, a
     * PING frame is sent to keep NAT bindings alive. A value of
     * 0 disables keep-alives. Default: 0 (disabled; the engine
     * does not enable keep-alives by default — v0.x callers
     * that need them set the field explicitly).
     *
     * msquic mapping: `KeepAliveIntervalMs` in
     * `QUIC_SETTINGS`.
     */
    std::uint32_t keep_alive_interval_ms = 0;
};

} // namespace nimrtc::web_transport

#endif // NIMRTC_WEB_TRANSPORT_CONFIG_HPP
