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

// Forward declare instead of including wolfSSL headers here to keep the
// public header clean.  The .cpp file includes the real headers.
struct WOLFSSL;
struct WOLFSSL_CTX;
struct WOLFSSL_X509;

namespace nimrtc::dtls {

/** DtlsSession implementation using wolfSSL DTLS 1.2. */
class DtlsSessionWolfSSL {
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
    core::Result<void> open() noexcept;

    void close() noexcept;

    /** Feed inbound DTLS record bytes from the ICE transport.
     *  @return bytes consumed from `bytes`. */
    std::size_t feed_inbound(std::span<const std::uint8_t> bytes,
                              const DtlsAddr& from) noexcept;

    /** Drain outbound DTLS records produced by wolfSSL. */
    std::vector<DtlsRecord> take_outbound() noexcept;

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
    void tick() noexcept;

    DtlsState state() const noexcept;
    bool is_connected() const noexcept;

    static const char* state_name(DtlsState s) noexcept;

    /** Local certificate fingerprint (RFC 8122).  Valid after open(). */
    const Fingerprint& local_fingerprint() const noexcept;

    void set_peer_fingerprint(std::string algo,
                               std::vector<std::uint8_t> value) noexcept;

    void set_role(DtlsRole r) noexcept;

    /** SRTP keying material — valid after state() == Connected. */
    std::optional<SrtpKeyingMaterial> srtp_keying_material() const noexcept;

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
