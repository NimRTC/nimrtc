/**
 * @file nimrtc/dtls/dtls_wolfssl_session.hpp
 * @brief DtlsSession implementation backed by wolfSSL DTLS 1.2.
 *
 * Implements the DtlsSession interface (same API as dtls.hpp) but
 * delegates the full DTLS 1.2 handshake to wolfSSL instead of the
 * hand-written state machine in dtls.cpp.
 *
 * ## I/O model
 *
 * The engine calls feed_inbound() / take_outbound() — a memory-buffer
 * model, NOT a socket model.  wolfSSL is driven in non-blocking mode:
 *
 *   feed_inbound()  → copies bytes into an internal recv buffer
 *   wolfSSL_read()  → reads from the recv buffer (non-blocking)
 *   wolfSSL_write()  → buffers outbound data in send_buf_
 *   take_outbound()  → returns send_buf_ contents and clears it
 *
 * ## Certificates
 *
 * Uses a pre-generated ECC keypair + self-signed certificate from
 * wolfSSL's bundled certs (no runtime cert generation needed).
 * The fingerprint is computed from the loaded certificate's DER form
 * so it matches what SDP `a=fingerprint` advertises.
 */

#pragma once

#include <nimrtc/dtls/dtls.hpp>
#include <nimrtc/dtls/dtls_session_iface.hpp>   // PAL Slice 4 seam

// Forward declare instead of including wolfSSL headers here to keep the
// public header clean.  The .cpp file includes the real headers.
struct WOLFSSL;
struct WOLFSSL_CTX;
struct WOLFSSL_X509;

namespace nimrtc::dtls {

/** DtlsSession implementation using wolfSSL DTLS 1.2.
 *
 *  PAL Slice 4 (v0.11.0) refactor: now inherits `IDtlsSession` so the
 *  factory can hand the engine a `unique_ptr<IDtlsSession>` and the
 *  wolfSSL specifics stay behind the existing concrete surface that
 *  engine.cpp and e2e Case D rely on.  See
 *  `docs/plan/transport-selection.md` §5.2 / §6.1.
 *
 *  Existing public surface is preserved verbatim:
 *    - open/close/feed_inbound/take_outbound/tick
 *    - state/is_connected/state_name
 *    - local_fingerprint/set_peer_fingerprint
 *    - set_role/srtp_keying_material/stats
 *
 *  New IDtlsSession methods are layered alongside (they delegate to
 *  the existing primitives where possible; see the .cpp for the
 *  dispatch).
 */
class DtlsSessionWolfSSL : public IDtlsSession {
public:
    /** cfg.srtp_profile is ignored — wolfSSL always negotiates
     *  SRTP_AES128_CM_SHA1_80 (RFC 5764 mandatory profile). */
    explicit DtlsSessionWolfSSL(Config cfg);
    ~DtlsSessionWolfSSL();

    DtlsSessionWolfSSL(const DtlsSessionWolfSSL&)            = delete;
    DtlsSessionWolfSSL& operator=(const DtlsSessionWolfSSL&) = delete;
    DtlsSessionWolfSSL(DtlsSessionWolfSSL&&)                 noexcept;
    DtlsSessionWolfSSL& operator=(DtlsSessionWolfSSL&&)      noexcept;

    /** Initialise wolfSSL context and load the ECC certificate.
     *  @return ok() on success. */
    core::Result<void> open() noexcept override;

    void close() noexcept;

    /** Feed inbound DTLS record bytes from the ICE transport.
     *  @return bytes consumed from `bytes`. */
    std::size_t feed_inbound(std::span<const std::uint8_t> bytes,
                              const DtlsAddr& from) noexcept override;

    /** Drain outbound DTLS records produced by wolfSSL. */
    std::vector<DtlsRecord> take_outbound() noexcept override;

    /** Drive the DTLS retransmit timer (RFC 6347 §4.2.4).
     *
     *  wolfSSL's non-blocking DTLS code defers retransmits to an external
     *  caller via `wolfSSL_dtls_got_timeout()`; without this pump, the
     *  handshake can stall indefinitely when the peer is slow or any
     *  HelloVerifyRequest cookie exchange is in flight.  Call this from
     *  the engine tick loop at ~50 ms cadence while the handshake has
     *  not yet completed.
     *
     *  Safe to call when state() is Connected / Failed / Closed — becomes
     *  a no-op. */
    void tick() noexcept override;

    DtlsState state() const noexcept override;
    bool is_connected() const noexcept override;

    static const char* state_name(DtlsState s) noexcept;

    /** Local certificate fingerprint (RFC 8122).  Valid after open(). */
    const Fingerprint& local_fingerprint() const noexcept override;

    void set_peer_fingerprint(std::string algo,
                               std::vector<std::uint8_t> value) noexcept override;

    void set_role(DtlsRole r) noexcept override;

    /** SRTP keying material — valid after state() == Connected.
     *  TPAL-4 (v0.11.0): override of `IDtlsSession::srtp_keying_material()` —
     *  the engine calls this through the seam, never through the concrete. */
    std::optional<SrtpKeyingMaterial> srtp_keying_material() const noexcept override;

    // -----------------------------------------------------------------
    // IDtlsSession (PAL Slice 4) — seam methods layered on top of the
    // existing primitives above.  Kept in a separate block so reviewers
    // can see at a glance which methods are new seam surface vs.
    // existing engine-facing surface.
    // -----------------------------------------------------------------

    /** @override IDtlsSession — accepts the seam Role enum.  Converts
     *  to DtlsRole and delegates to the existing set_role(DtlsRole). */
    void set_role(Role role) noexcept override;

    /** @override IDtlsSession — accepts a raw 32-byte SHA-256 span.
     *  Forwards to set_peer_fingerprint("sha-256", value).  Other
     *  lengths (including empty) clear the pin and log a warning. */
    void set_peer_fingerprint(
        std::span<const std::uint8_t> raw_sha256) noexcept override;

    /** @override IDtlsSession — equivalent to open().  Idempotent. */
    void start() noexcept override;

    /** @override IDtlsSession — equivalent to tick().  wolfSSL's
     *  non-blocking DTLS defers retransmits to an external pump; this
     *  is what the engine tick loop calls at ~50 ms cadence. */
    void pump() noexcept override;

    /** @override IDtlsSession — registers a one-shot callback fired
     *  with kOk on Connected, or a non-zero Status on Failed.  The
     *  callback is invoked at most once per handshake attempt; the
     *  registration auto-clears on fire. */
    void on_handshake_complete(
        IDtlsSession::OnCompleteCb cb) noexcept override;

    /** @override IDtlsSession — writes the 60-byte RFC 5764 §4.2
     *  keying material into `out`.  Returns kErrNotReady if the
     *  handshake has not yet reached Connected. */
    plugins::Status export_srtp_key_material(
        std::span<std::uint8_t, 60> out) noexcept override;

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
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nimrtc::dtls
