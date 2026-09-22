/**
 * @file nimrtc/dtls/dtls_gmssl_session.hpp
 * @brief DtlsSession implementation backed by GMSSL (国密 SSL) DTLS 1.2.
 *
 * GMSSL is a fork of OpenSSL 1.1.1 that adds Chinese national
 * cryptographic algorithm (GM/T 0003 / GM/T 0004) support: SM2
 * (椭圆曲线公钥密码), SM3 (杂凑), SM4 (分组密码).  As a DTLS 1.2
 * backend it exposes an OpenSSL-compatible API
 * (`SSL_CTX_new` / `SSL_new` / `SSL_do_handshake` / `SSL_write` /
 * `SSL_read` / `BIO_dgram` / `wolfSSL_*` analogues), so the
 * implementation mirrors the wolfSSL-backed `DtlsSessionWolfSSL` line
 * for line, swapping the wolfSSL primitives for GMSSL ones.
 *
 * ## Cipher suite selection
 *
 * GMSSL supports the two RFC 5764 §5 DTLS-SRTP cipher suites:
 *
 *   - ECDHE-ECDSA-AES128-GCM-SHA256 (0xC02B) — universal compatibility
 *     (Chrome, Firefox, Safari all accept this on the WebRTC data path).
 *   - ECDHE-ECDSA-AES256-GCM-SHA384 (0xC02C) — preferred by some
 *     Chrome / enterprise builds.
 *
 * SM2/SM3/SM4 cipher suites (`ECDHE-SM2-SM4-GCM-SM3` etc.) are NOT
 * registered with IANA / WebRTC peers and will be rejected by every
 * mainstream browser.  They are listed here for documentation only;
 * we deliberately do NOT enable them in the default cipher list so
 * that a GmSSL-backed NimRTC instance still interoperates with
 * Chrome / Firefox / Safari.  Operators who require the SM suite for
 * a closed domestic deployment should reconfigure GMSSL's cipher list
 * via a downstream fork / runtime override; this default preserves
 * "plug in GMSSL, get Chrome interop for free".
 *
 * ## I/O model
 *
 * Same memory-buffer model as the wolfSSL backend (see
 * `dtls_wolfssl_session.hpp` for the full description):
 *
 *   feed_inbound()  → GMSSL recv-MEM BIO ← bytes
 *   SSL_do_handshake() / SSL_write() / SSL_read() drive the state machine
 *   GMSSL send-MEM BIO  → take_outbound() returns queue
 *
 * GMSSL uses non-blocking I/O via the BIO pair (BIO_s_dgram or the
 * BIO_s_mem variant), driven by `BIO_set_mem_eof_return()` to make
 * `SSL_read`/`SSL_write` return `-1` with `SSL_ERROR_WANT_READ` /
 * `SSL_ERROR_WANT_WRITE` when more bytes are needed.
 *
 * ## Certificates
 *
 * Reuses the same bundled ECC cert + key as wolfSSL (P-256 + self-signed)
 * so the SDP `a=fingerprint` interop with Chrome is unchanged.  GMSSL
 * natively understands X.509 + ECC P-256, so no cert regeneration is
 * needed when swapping backends; only the TLS library underneath
 * changes.
 *
 * ## Thread model
 *
 * Like `DtlsSessionWolfSSL`, this class is NOT thread-safe by itself.
 * The engine serialises all DTLS calls on the engine thread; the GMSSL
 * implementation is driven from there via feed_inbound / take_outbound
 * / pump / on_handshake_complete.
 *
 * @note P1 — Slice 2 of the v0.11.0 multi-backend DTLS work, gated on
 *       ADR-013 (GMSSL as a second default DTLS backend).  Inherits
 *       `IDtlsSession` exactly like the wolfSSL backend so the engine
 *       holds `unique_ptr<IDtlsSession>` regardless of which backend
 *       the user picks via `EngineConfig::dtls_name`.
 */

#pragma once

#include <nimrtc/dtls/dtls.hpp>
#include <nimrtc/dtls/dtls_session_iface.hpp>   // PAL Slice 4 seam

// Forward declare instead of including GMSSL / OpenSSL headers here to
// keep the public header clean.  The .cpp file includes the real
// headers.  GMSSL uses `SSL` / `SSL_CTX` / `SSL_METHOD` / `BIO` /
// `X509` — exactly the OpenSSL 1.1.x naming.
struct ssl_st;
struct ssl_ctx_st;
struct bio_st;
struct x509_st;

namespace nimrtc::dtls {

/**
 * @brief DtlsSession implementation using GMSSL (国密 SSL) DTLS 1.2.
 *
 * PAL Slice 4 (v0.11.0): inherits `IDtlsSession` so the factory can
 * hand the engine a `unique_ptr<IDtlsSession>` and the GMSSL specifics
 * stay behind the existing engine-facing surface (open / state /
 * local_fingerprint / srtp_keying_material / feed_inbound /
 * take_outbound / tick / set_role(DtlsRole) /
 * set_peer_fingerprint(algo, value)).
 *
 * The seam-method additions (`start` / `pump` /
 * `on_handshake_complete` / `export_srtp_key_material`) are layered
 * alongside as `override` wrappers, mirroring the wolfSSL backend
 * exactly — see ADR-013 §"Seam parity with wolfSSL".
 *
 * @see docs/adr/ADR-013-gmssl-dtls-backend.md (decision rationale).
 */
class DtlsSessionGmSSL : public IDtlsSession {
public:
    explicit DtlsSessionGmSSL(Config cfg);
    ~DtlsSessionGmSSL();

    DtlsSessionGmSSL(const DtlsSessionGmSSL&)            = delete;
    DtlsSessionGmSSL& operator=(const DtlsSessionGmSSL&) = delete;
    DtlsSessionGmSSL(DtlsSessionGmSSL&&)                 noexcept;
    DtlsSessionGmSSL& operator=(DtlsSessionGmSSL&&)      noexcept;

    // ---- Lifecycle (IDtlsSession overrides) -------------------------------
    core::Result<void> open() noexcept override;
    void close() noexcept;

    // ---- Wire I/O (IDtlsSession overrides) --------------------------------
    std::size_t feed_inbound(std::span<const std::uint8_t> bytes,
                              const DtlsAddr& from) noexcept override;
    std::vector<DtlsRecord> take_outbound() noexcept override;
    void tick() noexcept override;

    // ---- Status (IDtlsSession overrides) ----------------------------------
    DtlsState state() const noexcept override;
    bool is_connected() const noexcept override;

    static const char* state_name(DtlsState s) noexcept;

    // ---- Local fingerprint / peer pin / role / SRTP ----------------------
    const Fingerprint& local_fingerprint() const noexcept override;
    void set_peer_fingerprint(std::string algo,
                               std::vector<std::uint8_t> value) noexcept override;
    void set_role(DtlsRole r) noexcept override;
    std::optional<SrtpKeyingMaterial>
    srtp_keying_material() const noexcept override;

    // ---- IDtlsSession (PAL Slice 4) seam methods --------------------------
    void set_role(Role role) noexcept override;
    void set_peer_fingerprint(
        std::span<const std::uint8_t> raw_sha256) noexcept override;
    void start() noexcept override;
    void pump() noexcept override;
    void on_handshake_complete(OnCompleteCb cb) noexcept override;
    plugins::Status export_srtp_key_material(
        std::span<std::uint8_t, 60> out) noexcept override;

    // ---- Stats (mirrors DtlsSessionWolfSSL::Stats) ------------------------
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
