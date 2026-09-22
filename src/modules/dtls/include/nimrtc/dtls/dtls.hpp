/**
 * @file nimrtc/dtls/dtls.hpp
 * @brief DTLS (Datagram TLS) handshake for WebRTC DTLS-SRTP keying.
 *
 * Implements DTLS 1.2 server/client handshake state machine and exports
 * keying material via the TLS exporter interface (RFC 5705) for
 * DTLS-SRTP (RFC 5764).
 *
 * ## Cryptographic backends
 *
 * - **Windows** (primary when available): BCrypt (AES, HMAC-SHA256, ECDH P-256,
 *   ECDSA, DRBG).  Self-contained, no external deps.
 * - **Stub** (fallback): random number generation + ECDH arithmetic only,
 *   cert verification / signature disabled.  Used for unit tests and
 *   interop scaffolding.
 *
 * ## Thread model
 *
 * DtlsSession is NOT thread-safe.  Serialize calls from a single thread
 * (typically the engine tick thread).
 *
 * ## Usage
 *
 * ```cpp
 * DtlsSession::Config cfg;
 * cfg.role = DtlsRole::Server;  // or Client
 * cfg.peer_fingerprint_algo = "sha-256";
 * cfg.peer_fingerprint_value = "<base64>";
 *
 * DtlsSession dtls(cfg);
 * dtls.open();
 *
 * // Feed inbound DTLS records from transport:
 * while (dtls.feed_inbound(packet, from_addr) > 0) {
 *     // DTLS state machine consumed bytes; loop until consumed < packet size.
 * }
 *
 * // Drain outbound records the handshake produced:
 * for (auto& rec : dtls.take_outbound()) {
 *     transport.send(rec.bytes, rec.to);
 * }
 *
 * // After handshake completes, derive SRTP keys:
 * if (auto keys = dtls.srtp_keying_material()) {
 *     srtp_ctx.derive_keys_for_remote(keys->client_key, keys->server_salt, ...);
 * }
 * ```
 *
 * @note P1 — RFC 5764 / RFC 6347.  Provides fingerprint generation,
 *       DTLS 1.2 handshake, and SRTP key derivation.
 */

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/error.hpp>
#include <nimrtc/dtls/dtls_types.hpp>   // PAL Slice 4 hoist: Constants,
                                        // DtlsRole, DtlsState, DtlsAddr,
                                        // DtlsRecord, SrtpKeyingMaterial,
                                        // Fingerprint, Config.
#include <nimrtc/dtls/dtls_session_iface.hpp>   // PAL Slice 4 seam — DtlsSession
                                                // inherits IDtlsSession from
                                                // here.

namespace nimrtc::dtls {

// -----------------------------------------------------------------------------
// Types — see nimrtc/dtls/dtls_types.hpp (PAL Slice 4 hoist).
// This header only owns the concrete DtlsSession class now.
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// DtlsSession — one peer connection.
//
// As of the Linux-adaptation refactor, the only DtlsSession implementation
// is `DtlsSessionWolfSSL` (defined in dtls_wolfssl_session.hpp).  We still
// expose `class DtlsSession` here so that downstream code (engine.cpp,
// tests) can keep referring to `nimrtc::dtls::DtlsSession` uniformly.
// `DtlsSession` is implemented as a thin wrapper around
// `DtlsSessionWolfSSL` via delegation (see dtls_wolfssl_session.cpp for
// the out-of-line definitions).
//
// The hand-written DTLS state machine (the original `dtls.cpp`) was
// removed; the only DTLS provider now is wolfSSL.
// -----------------------------------------------------------------------------
class DtlsSession : public IDtlsSession {
public:
    // Expose the seam-surface overloads so the legacy concrete
    // methods below (`set_role(DtlsRole)`, `set_peer_fingerprint(string,
    // vector)`) don't hide them via C++ name-hiding rules.  See
    // dtls_session_iface.hpp for the seam vs. legacy split.
    using IDtlsSession::set_role;
    using IDtlsSession::set_peer_fingerprint;

    explicit DtlsSession(Config config);
    ~DtlsSession();

    DtlsSession(const DtlsSession&)            = delete;
    DtlsSession& operator=(const DtlsSession&) = delete;
    DtlsSession(DtlsSession&&)                 noexcept;
    DtlsSession& operator=(DtlsSession&&)      noexcept;

    // ---- Lifecycle --------------------------------------------------------

    /** Initialise state machine + key material.  Must be called before
     *  feed_inbound().  Generates a self-signed cert if no local fingerprint
     *  has been provided (we'll advertise the resulting fingerprint). */
    core::Result<void> open() noexcept;

    void close() noexcept;

    // ---- Wire I/O ---------------------------------------------------------

    /** Feed inbound bytes from the transport.  Returns the number of bytes
     *  consumed from `bytes`.  Outbound records (if any) are pushed onto
     *  the internal queue and can be drained via take_outbound(). */
    std::size_t feed_inbound(std::span<const std::uint8_t> bytes,
                             const DtlsAddr& from) noexcept;

    /** Drain handshake-generated outbound records. */
    std::vector<DtlsRecord> take_outbound() noexcept;

    /** Drive the DTLS retransmit timer (RFC 6347 §4.2.4).  Call from the
     *  engine tick loop at ~50 ms cadence while the handshake has not
     *  yet completed.  Safe no-op once state() is Connected/Failed/Closed. */
    void tick() noexcept;

    // ---- Status -----------------------------------------------------------

    DtlsState state() const noexcept;
    bool is_connected() const noexcept;

    /** Human-readable name for a DtlsState enum value (for tracing). */
    static const char* state_name(DtlsState s) noexcept;

    /** Local certificate fingerprint (advertised in SDP).  Valid after open(). */
    const Fingerprint& local_fingerprint() const noexcept;

    /** Update the SDP-pinned peer fingerprint without recreating the local
     *  certificate/keypair.  Safe to call any time before the handshake
     *  completes.  Required because the engine learns the remote fingerprint
     *  AFTER its own SDP has already advertised the local fingerprint —
     *  recreating the session here would change the advertised fingerprint
     *  and break the handshake. */
    void set_peer_fingerprint(std::string algo,
                              std::vector<std::uint8_t> value) noexcept;

    /** Update the DTLS role without recreating the local certificate/keypair.
     *  When transitioning Server -> Client, the state machine also generates
     *  and enqueues an initial ClientHello.  Safe to call any time before
     *  the handshake completes. */
    void set_role(DtlsRole r) noexcept;

    /** SRTP keying material — available once state() == Connected. */
    std::optional<SrtpKeyingMaterial> srtp_keying_material() const noexcept;

    // ------------------------------------------------------------------
    // PAL Slice 4 / TPAL-4 seam surface — delegates to DtlsSessionWolfSSL
    // via the inner impl_.  Kept explicit (not just `using`) so a reader
    // can see the full interface this class satisfies without grepping.
    // ------------------------------------------------------------------

    /** @override IDtlsSession — accepts the seam Role enum.
     *  Delegates to set_role(DtlsRole). */
    void set_role(Role role) noexcept;

    /** @override IDtlsSession — accepts a raw 32-byte SHA-256 span.
     *  Forwards to set_peer_fingerprint("sha-256", value). */
    void set_peer_fingerprint(
        std::span<const std::uint8_t> raw_sha256) noexcept;

    /** @override IDtlsSession — equivalent to open() but void-returning.
     *  Discards the Result<void>; errors surface via state() ==
     *  Failed or the on_handshake_complete callback. */
    void start() noexcept;

    /** @override IDtlsSession — equivalent to tick(). */
    void pump() noexcept;

    /** @override IDtlsSession — registers the seam one-shot callback
     *  with the inner wolfSSL-backed session. */
    void on_handshake_complete(OnCompleteCb cb) noexcept;

    /** @override IDtlsSession — flattens the SrtpKeyingMaterial
     *  4-field layout into the RFC 5764 §4.2 60-byte form.  Returns
     *  kErrNotReady if the handshake has not completed. */
    plugins::Status export_srtp_key_material(
        std::span<std::uint8_t, 60> out) noexcept;

    // ---- Stats ------------------------------------------------------------

    struct Stats {
        std::uint64_t records_in    = 0;
        std::uint64_t records_out   = 0;
        std::uint64_t handshake_ms  = 0;
        std::uint64_t retransmits   = 0;
        std::uint64_t alerts_in     = 0;
        std::uint64_t alerts_out    = 0;
        std::uint64_t errors        = 0;
    };
    Stats stats() const noexcept;

private:
    /** Holds the actual wolfSSL-backed implementation (DtlsSessionWolfSSL).
     *  We use a pImpl idiom so that callers (engine.cpp) only need the
     *  forward declarations of DtlsSession + DtlsSessionWolfSSL — the
     *  wolfSSL headers never leak through this public header. */
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nimrtc::dtls
