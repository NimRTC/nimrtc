/**
 * @file nimrtc/dtls/dtls_session_iface.hpp
 * @brief IDtlsSession — DTLS session seam (PAL Slice 4).
 *
 * Per docs/plan/transport-selection.md §5.2 and §6.1, this is the
 * transport-layer plugin-replaceable seam for DTLS 1.2 handshakes.
 *
 * The seam lets a host application replace wolfSSL with another DTLS
 * backend (OpenSSL, BoringSSL, mbedTLS, custom) without touching the
 * engine.  Slice 8 (engine integration) routes `engine.cpp` through the
 * factory rather than constructing `DtlsSessionWolfSSL` directly; PAL
 * Slice 4 / TPAL-4 (v0.11.0) extended the seam interface with the
 * engine-facing surface (`open()` / `state()` / `local_fingerprint()` /
 * `srtp_keying_material()` / `feed_inbound()` / `take_outbound()` /
 * `tick()` / `set_role(DtlsRole)` / `set_peer_fingerprint(...)` /
 * `is_connected()`) so the engine can hold `unique_ptr<IDtlsSession>`
 * and never downcast to the concrete backend.
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
 * @note P1 — interface added as part of PAL Slice 4 (v0.11.0),
 *       extended with the engine-facing surface in TPAL-4 (v0.11.0).
 */

#ifndef NIMRTC_DTLS_SESSION_IFACE_HPP
#define NIMRTC_DTLS_SESSION_IFACE_HPP

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/error.hpp>      // core::Result<void>
#include <nimrtc/plugins/base.hpp>    // plugins::Status
#include <nimrtc/dtls/dtls_types.hpp> // DtlsRole, DtlsState, DtlsAddr,
                                     // DtlsRecord, Fingerprint,
                                     // SrtpKeyingMaterial, Config — the
                                     // common types hoisted out of
                                     // `dtls.hpp` in PAL Slice 4 / TPAL-4
                                     // (v0.11.0) to break the circular
                                     // include between this seam header
                                     // and the concrete `dtls.hpp`.

namespace nimrtc::dtls {

// Alias requested by PAL Slice 4 DoD — `DtlsConfig` is the seam-level
// name for the concrete `Config` struct (kept in
// `nimrtc/dtls/dtls_types.hpp`).  Implementers of
// `IDtlsSessionFactory::create()` receive a `DtlsConfig` and pass it
// straight through to the concrete backend constructor.
using DtlsConfig = Config;

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

    // =====================================================================
    // Engine-facing surface (PAL Slice 4 / TPAL-4 — DTLS seam engine
    // integration, v0.11.0 — DoD gate #6).
    // ---------------------------------------------------------------------
    //
    // These methods mirror the existing concrete `DtlsSession` /
    // `DtlsSessionWolfSSL` surface so that the engine can hold the
    // DTLS session as `unique_ptr<IDtlsSession>` and never downcast to
    // the concrete backend (which is what the v0.10.2 Slice 8 code
    // did).  Engine-driven replacement of the wolfSSL backend (OpenSSL
    // / BoringSSL / mbedTLS / 国密 / custom) is now a
    // `register_dtls_session(id, factory*)` away.
    //
    // The signatures are byte-for-byte identical to the v0.10.3
    // concrete methods — no behaviour changes, no return-type
    // widening, no signature narrowing.  Backends already implementing
    // these (DtlsSessionWolfSSL) just need to add `override`; new
    // backends must implement the full surface or inherit from
    // DtlsSessionWolfSSL / DtlsSession.
    // =====================================================================

    /**
     * @brief Initialise the DTLS context, certificate, and I/O callbacks.
     *
     * Equivalent to `start()` (which returns void); this overload
     * returns `core::Result<void>` so the engine can report the
     * cert-load / wolfSSL_Init failure modes that v0.10.x used to
     * surface as a `bool` from the concrete `DtlsSession::open()`.
     *
     * MUST be called before `feed_inbound()`; idempotent (a cached
     * `open_called` flag short-circuits the second+ call with an
     * `ok()` result).
     */
    virtual core::Result<void> open() noexcept = 0;

    /**
     * @brief Install the DTLS role using the concrete-typed enum.
     *
     * Concrete counterpart of `set_role(Role)` above; takes
     * `dtls::DtlsRole` (the existing module enum).  Engines call this
     * overload because the role comes straight from the parsed
     * `a=setup` SDP attribute via `dcfg.role = DtlsRole::Server`.
     * Safe to call any time before the handshake completes; on
     * Server -> Client transition the state machine also emits an
     * initial ClientHello.
     */
    virtual void set_role(DtlsRole r) noexcept = 0;

    /**
     * @brief Install the SDP `a=fingerprint` (algo + raw bytes).
     *
     * Concrete counterpart of `set_peer_fingerprint(span)` above;
     * accepts the algorithm name explicitly so a backend that
     * supports multiple hash algorithms can pick the right verifier.
     * The wolfSSL backend currently only honours `"sha-256"`.
     */
    virtual void set_peer_fingerprint(
        std::string algo,
        std::vector<std::uint8_t> value) noexcept = 0;

    /**
     * @brief Feed inbound DTLS record bytes from the transport.
     *
     * Returns the number of bytes consumed from `bytes` (DTLS is a
     * datagram protocol so the entire record is either accepted or
     * rejected).  Outbound records produced as a side effect are
     * pushed onto the internal queue and can be drained via
     * `take_outbound()`.
     */
    virtual std::size_t feed_inbound(
        std::span<const std::uint8_t> bytes,
        const DtlsAddr& from) noexcept = 0;

    /**
     * @brief Drain handshake-produced outbound DTLS records.
     *
     * Returns the queue of outbound records and clears it.  Called
     * by the engine `drain_dtls()` after every `feed_inbound()` /
     * `tick()` so the records go out the wire (or into the
     * scheduler).
     */
    virtual std::vector<DtlsRecord> take_outbound() noexcept = 0;

    /**
     * @brief Drive the DTLS retransmit timer (RFC 6347 §4.2.4).
     *
     * Same semantics as `pump()` above — the engine tick loop calls
     * this at ~50 ms cadence while the handshake has not yet
     * completed.  Safe no-op once state() is Connected / Failed /
     * Closed.
     */
    virtual void tick() noexcept = 0;

    /**
     * @brief Current DTLS state machine value.
     *
     * Used by the engine's `dtls_state()` accessor and by the
     * `process_remote_sdp()` flow to drive role / fingerprint
     * updates.  Out-of-line value `DtlsState::Closed` when the
     * session has not been opened yet — same convention as the
     * concrete class.
     */
    virtual DtlsState state() const noexcept = 0;

    /**
     * @brief True iff state() == Connected.
     *
     * Shortcut for `state() == DtlsState::Connected`; the engine
     * checks this before permitting SRTP encrypt / decrypt on the
     * media path (see `send_audio()` / `on_transport_recv()` in
     * `src/engine/src/engine.cpp`).
     */
    virtual bool is_connected() const noexcept = 0;

    /**
     * @brief Local certificate fingerprint (advertised in our SDP).
     *
     * Valid after `open()`.  The engine reads `.hex_colon` to build
     * the `a=fingerprint` SDP attribute (RFC 8122 colon-separated
     * upper-hex form).  Returns a reference into backend-owned
     * storage; lifetime is tied to the IDtlsSession instance.
     */
    virtual const Fingerprint& local_fingerprint() const noexcept = 0;

    /**
     * @brief SRTP keying material — available once Connected.
     *
     * Returns `std::nullopt` until the DTLS handshake reaches
     * `DtlsState::Connected`; from then on the engine calls
     * `maybe_install_srtp_keys()` which routes the key material into
     * `nimrtc::srtp::SrtpContext`.
     */
    virtual std::optional<SrtpKeyingMaterial>
    srtp_keying_material() const noexcept = 0;
};

} // namespace nimrtc::dtls

#endif // NIMRTC_DTLS_SESSION_IFACE_HPP