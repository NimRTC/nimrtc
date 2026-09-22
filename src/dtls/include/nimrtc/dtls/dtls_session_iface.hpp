/**
 * @file nimrtc/dtls/dtls_session_iface.hpp
 * @brief IDtlsSession — DTLS session seam (PAL Slice 4).
 *
 * Per docs/plan/transport-selection.md §5.2 and §6.1, this is the
 * transport-layer plugin-replaceable seam for DTLS 1.2 handshakes.
 *
 * The seam lets a host application replace wolfSSL with another DTLS
 * backend (OpenSSL, BoringSSL, mbedTLS, custom) without touching the
 * engine.  Slice 8 (engine integration) will route `engine.cpp` through
 * the factory rather than constructing `DtlsSessionWolfSSL` directly;
 * for Slice 4 we only introduce the seam, register a default factory
 * (id = "wolfssl"), and keep the existing engine.cpp untouched.
 *
 * ## Thread model
 *
 * IDtlsSession is NOT thread-safe by itself.  The engine serializes all
 * DTLS calls on the engine thread; the wolfSSL implementation is
 * driven from there via feed_inbound / take_outbound / pump /
 * on_handshake_complete.
 *
 * ## Method semantics
 *
 *  - `set_role`              — install Server or Client role.  Must be
 *                              called before start(); silently ignored
 *                              once the handshake has progressed.
 *  - `set_peer_fingerprint`  — install the SDP `a=fingerprint` value
 *                              (raw 32-byte SHA-256).  May be called
 *                              before start() or before the handshake
 *                              completes (post-handshake setting is a
 *                              no-op for verification).
 *  - `start`                 — initialise the underlying DTLS context.
 *                              Equivalent to `DtlsSession::open()`.
 *                              Idempotent.
 *  - `pump`                  — drive the retransmit timer (RFC 6347
 *                              §4.2.4).  Required by wolfSSL's
 *                              non-blocking DTLS implementation: without
 *                              a periodic pump the handshake can stall
 *                              when a HelloVerifyRequest cookie is in
 *                              flight or a flight is dropped.  Engine
 *                              typically calls this at ~50 ms cadence
 *                              from its tick loop while the handshake
 *                              has not yet completed.
 *  - `on_handshake_complete` — register a one-shot (per-handshake)
 *                              callback fired when the DTLS state
 *                              transitions to Connected (or, on a
 *                              fatal failure, to Failed).  The Status
 *                              argument is kOk on Connected and a
 *                              non-zero error code on Failed.
 *  - `export_srtp_key_material` — copy the 60-byte DTLS-SRTP keying
 *                              material (RFC 5764 §4.2:
 *                              client_master_key[16] +
 *                              server_master_key[16] +
 *                              client_master_salt[14] +
 *                              server_master_salt[14]) into `out`.
 *                              Returns kOk on success,
 *                              kErrNotReady when the handshake has not
 *                              yet completed, kErrBufferTooSmall if
 *                              `out` is not exactly 60 bytes
 *                              (enforced at compile time via
 *                              std::span<uint8_t, 60>).
 *
 * ## Deviations from §5.2 (documented in the PR report)
 *
 *  - §5.2 uses `plugins::Role` for the role parameter.  That type does
 *    not exist in plugins:: yet (PAL Slice 4 does not own the plugins
 *    interface extension).  We instead use `dtls::Role`, an alias for
 *    the existing `dtls::DtlsRole` enum (Server/Client) so callers do
 *    not need a separate concept.
 *  - §5.2 uses `plugins::OnCompleteCb`.  No plugin-callback type with
 *    that name exists yet.  We define `IDtlsSession::OnCompleteCb`
 *    locally as `std::function<void(plugins::Status)>` so the seam is
 *    self-contained; future PAL Slices can promote it to plugins:: if
 *    other modules need the same shape.
 *  - §5.2 uses `span<const uint8_t>` and `span<uint8_t, 60>`.  We
 *    spell these out via `std::span` so callers don't have to know
 *    whether the plugin layer or the module layer owns `span`.
 *
 * @note P1 — interface added as part of PAL Slice 4 (v0.11.0).
 */

#ifndef NIMRTC_DTLS_SESSION_IFACE_HPP
#define NIMRTC_DTLS_SESSION_IFACE_HPP

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

#include <nimrtc/plugins/base.hpp>   // plugins::Status

namespace nimrtc::dtls {

// Forward declarations — the concrete `Config` type lives in
// `nimrtc/dtls/dtls.hpp` (the existing module header).  Including that
// here would force every consumer of the seam to also pull in the
// concrete dtls config; the forward declaration keeps the seam header
// minimal.
struct Config;
using DtlsConfig = Config;   // alias requested by the Slice 4 DoD

/**
 * @brief DTLS role — Server (answerer) or Client (offerer).
 *
 * Defined here as an alias for `dtls::DtlsRole` (the existing module
 * enum) so the seam interface and the concrete backend share a single
 * role concept.  See the header comment deviation note above for why
 * this isn't `plugins::Role`.
 */
enum class Role : std::uint8_t {
    Server = 0,
    Client = 1,
};

/**
 * @brief IDtlsSession — pluggable DTLS session.
 *
 * One IDtlsSession per peer connection.  Created by
 * IDtlsSessionFactory::create().
 *
 * @see docs/plan/transport-selection.md §5.2 / §6.1.
 */
class IDtlsSession {
public:
    /**
     * @brief One-shot completion callback.
     *
     * Fired exactly once per handshake attempt:
     *   - with kOk when the DTLS state transitions to Connected;
     *   - with a non-zero status (e.g. kErrCorrupt, kErrInternal) on
     *     a fatal handshake failure that drove the state to Failed.
     *
     * Pass an empty std::function to clear the registration.
     * Not thread-safe; call only from the engine thread.
     */
    using OnCompleteCb = std::function<void(plugins::Status)>;

    virtual ~IDtlsSession() = default;

    /**
     * @brief Install the DTLS role before start().
     *
     * No-op (with a log warning) if called after start() has produced
     * any handshake traffic.  Idempotent within the Initial/Closed
     * window.
     */
    virtual void set_role(Role role) noexcept = 0;

    /**
     * @brief Install the SDP `a=fingerprint` raw 32-byte SHA-256 hash.
     *
     * The bytes are interpreted as a SHA-256 digest per RFC 8122 (NOT
     * the SPKI hash — that would be RFC 7250 / WebRTC ignores it).
     * Empty span = no pin (backends may accept fail-open or fail-closed
     * per their own policy; the wolfSSL backend currently logs a
     * warning and accepts to preserve e2e Case D).
     */
    virtual void set_peer_fingerprint(
        std::span<const std::uint8_t> raw_sha256) noexcept = 0;

    /**
     * @brief Initialise the DTLS context, certificate, and I/O callbacks.
     *
     * Idempotent.  Returns silently on a second call (the underlying
     * concrete session caches an `open_called` flag).
     */
    virtual void start() noexcept = 0;

    /**
     * @brief Drive the DTLS retransmit timer once.
     *
     * Must be called periodically (~50 ms) by the engine tick loop
     * while the handshake has not yet completed.  wolfSSL's non-blocking
     * DTLS defers retransmits to an external pump; without this call a
     * stalled handshake (lost HelloVerifyRequest, lost flight) hangs
     * forever.  Safe no-op once Connected/Failed/Closed.
     */
    virtual void pump() noexcept = 0;

    /**
     * @brief Register the handshake completion callback.
     *
     * The callback fires once, when state() transitions to either
     * Connected (kOk) or Failed (non-zero Status).  After firing, the
     * registration is cleared; re-arm by calling this again before
     * the next handshake attempt (e.g. after a teardown / restart).
     *
     * Pass an empty std::function to clear.
     */
    virtual void on_handshake_complete(OnCompleteCb cb) noexcept = 0;

    /**
     * @brief Export the 60-byte DTLS-SRTP keying material.
     *
     * RFC 5764 §4.2 layout:
     *   client_master_key[16] | server_master_key[16]
     *   | client_master_salt[14] | server_master_salt[14]
     *
     * @return kOk on success;
     *         kErrNotReady when the handshake has not completed yet.
     */
    virtual plugins::Status export_srtp_key_material(
        std::span<std::uint8_t, 60> out) noexcept = 0;
};

} // namespace nimrtc::dtls

#endif // NIMRTC_DTLS_SESSION_IFACE_HPP
