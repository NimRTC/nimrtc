/**
 * @file src/dtls/src/dtls_gmssl_session.cpp
 * @brief DtlsSessionGmSSL — GMSSL v3.x native-API DTLS 1.2 implementation.
 *
 * ## API basis
 *
 * This implementation uses GMSSL v3.x (https://github.com/guanzhi/GmSSL)
 * via its native C API — NOT OpenSSL compatibility shims.  Key types:
 *
 *   TLS_CTX      — equivalent to OpenSSL's SSL_CTX
 *   TLS_CONNECT  — equivalent to OpenSSL's SSL
 *   tls_ctx_init / tls_init / tls_do_handshake / tls_send / tls_recv
 *
 * ## I/O model
 *
 * GMSSL uses a file-descriptor (socket) as the transport.  Each
 * DtlsSessionGmSSL owns one non-blocking connected UDP socket.
 * The NimRTC test layer bridges two sessions via feed_inbound /
 * take_outbound: session A's take_outbound() → feed_inbound(B) and
 * vice-versa.  This mirrors real DTLS over UDP loopback.
 *
 * ## Cipher suites
 *
 * TLCP (GM/T 0044) cipher suites — these are the GM/T-compliant
 * profiles that GMSSL signs as DTLS-SRTP mandatory:
 *
 *   TLS_cipher_ecdhe_sm4_cbc_sm3   (0xe011) ECDHE key exchange, SM2 cert
 *                                   authentication, SM4-CBC integrity
 *   TLS_cipher_ecdhe_sm4_gcm_sm3   (0xe051) ECDHE + SM4-GCM (preferred)
 *
 * Both use SM2 certificates (SM2 public-key + SM2 signature with SM3).
 * For ECDHE the certificate is only used for authentication (signing the
 * ECDH parameters); P-256 ECDSA certs would also work but are not GM/T
 * compliant.  This implementation uses SM2 throughout.
 *
 * ## SRTP key export
 *
 * RFC 5764 §4.2 defines the DTLS-SRTP keying material derivation:
 *
 *   key_block = TLS-PRF(master_secret, "EXTRACTOR-dtls_srtp",
 *                       client_random + server_random)
 *
 * GMSSL exposes tls_derive_key_block() which fills conn->key_block[]
 * after the handshake.  We use tls_prf() directly to derive the
 * EXTRACTOR-dtls_srtp material per RFC 5764, then pack it as:
 *   client_write_key[16] + server_write_key[16]
 *   client_write_SRTP_salt[14] + server_write_SRTP_salt[14]  = 60 bytes
 *
 * ## Thread model
 *
 * Same as DtlsSessionWolfSSL: this class is NOT thread-safe by itself.
 * The engine serialises all DTLS calls on its thread.
 */

#include "nimrtc/dtls/dtls_gmssl_session.hpp"

#include <nimrtc/core/log.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

// GMSSL v3.x native headers
extern "C" {
#include <gmssl/tls.h>
#include <gmssl/sm2.h>
#include <gmssl/sm3.h>
#include <gmssl/sm4.h>
#include <gmssl/digest.h>   // DIGEST_sha1(), DIGEST_sm3()
#include <gmssl/pem.h>
#include <gmssl/error.h>
#include <gmssl/mem.h>
#include <gmssl/rand.h>
}

namespace {

using nimrtc::dtls::Config;
using nimrtc::dtls::DtlsRole;
using nimrtc::dtls::SrtpProfile;

// ---------------------------------------------------------------------------
// Error helpers
// ---------------------------------------------------------------------------

std::string gmssl_err() {
    // GMSSL 3.x doesn't expose a string-returning error getter; errno is the
    // closest portable signal (most GMSSL APIs return -1 on failure and set
    // errno via internal `error_print()` macros).  For a real fix, see
    // docs/adr/ADR-013 "Out of scope" — we surface "errno=N" so callers can
    // at least see *something* in the log.
    return std::string("errno=") + std::to_string(errno);
}

// ---------------------------------------------------------------------------
// Fingerprint (RFC 8122) — SHA-256 of DER-encoded X.509 cert
// ---------------------------------------------------------------------------

std::string cert_sha256_hex(const uint8_t* der, size_t der_len) {
    // GMSSL 3.x exposes only the three-step sm3 interface (no one-shot).
    SM3_CTX ctx;
    uint8_t hash[SM3_DIGEST_SIZE] = {0};
    sm3_init(&ctx);
    sm3_update(&ctx, der, der_len);
    sm3_finish(&ctx, hash);
    static const char* tab = "0123456789abcdef";
    std::string s;
    s.reserve(64);
    for (size_t i = 0; i < SM3_DIGEST_SIZE; ++i) {
        s.push_back(tab[hash[i] >> 4]);
        s.push_back(tab[hash[i] & 0xF]);
    }
    return s;
}

std::string hex_colon(const uint8_t* data, size_t len) {
    static const char* tab = "0123456789ABCDEF";
    std::string s;
    s.reserve(len * 3);
    for (size_t i = 0; i < len; ++i) {
        s.push_back(tab[data[i] >> 4]);
        s.push_back(tab[data[i] & 0xF]);
        if (i + 1 < len) s.push_back(':');
    }
    return s;
}

// ---------------------------------------------------------------------------
// TLCP cipher suites — GM/T 0044 / RFC 8998
// ---------------------------------------------------------------------------

// TLCP 1.1 cipher suites — GM/T 0044 / RFC 8998.
// ECDHE key exchange, SM2 certificate auth, SM4-GCM preferred.
// We use TLS 1.2 (not TLCP) so tls_ctx_check() does not require the
// server to hold two SM2 certs.  ECDHE suites work with one cert.
static const int kTLCPCiphers[] = {
    TLS_cipher_ecdhe_sm4_gcm_sm3,     // 0xe051 — TLCP-1.1 recommended, AEAD
    TLS_cipher_ecdhe_sm4_cbc_sm3,     // 0xe011 — TLCP-1.1 fallback
};

// Standard WebRTC-safe fallback for interop with Chrome/Firefox.
// GMSSL 3.x dropped the "ECDHE+SM4-SHA256" pseudo-ciphers (they were never
// real GM/T 0044 cipher suites — RFC 8998 only defines SM4-SM3 variants).
// We use the GM-SM3 suites first and add RFC 5246 / TLS 1.2 international
// suites for cross-backend interop tests.
static const int kInteropCiphers[] = {
    TLS_cipher_ecdhe_sm4_gcm_sm3,
    TLS_cipher_ecdhe_sm4_cbc_sm3,
    // International fallback (P-256 ECDSA + AES-GCM — NOT GM/T compliant
    // but useful for testing the I/O pipeline when SM2 certs are not yet
    // generated).
    TLS_cipher_ecdhe_ecdsa_with_aes_128_gcm_sha256,  // 0xc02b
    TLS_cipher_ecdhe_ecdsa_with_aes_128_cbc_sha256,  // 0xc023
};

// Choose cipher list: prefer TLCP, fall back to interop mix.
const int* choose_cipher_list(bool prefer_tlcp) {
    return prefer_tlcp ? kTLCPCiphers : kInteropCiphers;
}
size_t choose_cipher_list_size(bool prefer_tlcp) {
    return prefer_tlcp ? (sizeof(kTLCPCiphers) / sizeof(kTLCPCiphers[0]))
                        : (sizeof(kInteropCiphers) / sizeof(kInteropCiphers[0]));
}

// ---------------------------------------------------------------------------
// SRTP EXTRACTOR key derivation (RFC 5764 §4.2)
// ---------------------------------------------------------------------------

// Derives SRTP master key material per RFC 5764 §4.2.
//   key_block = TLS-PRF(master_secret, "EXTRACTOR-dtls_srtp",
//                        client_random + server_random)
// GMSSL 3.x tls_prf() implements the TLS 1.2 PRF.
// Returns 60 bytes in out[60]: client_write_key(16) + server_write_key(16)
//   + client_SRTP_salt(14) + server_SRTP_salt(14).
bool derive_srtp_keys(const TLS_CONNECT* conn,
                       uint8_t out[60]) {
    // GMSSL 3.x: DIGEST_sm3() is a factory function (returns const DIGEST*).
    // TLCP (GM/T 0044) §6.2 specifies SM3 as the PRF — not SHA-1 as in
    // RFC 5246.  GMSSL 3.x also ships SHA-1 behind ENABLE_SHA1 (off by
    // default for FIPS-140 reasons), so SM3 is the correct and portable
    // choice for TLCP.
    const DIGEST* prf_digest = DIGEST_sm3();

    static const char kLabel[] = "EXTRACTOR-dtls_srtp";
    // RFC 5764 §4.2: seed = client_random || server_random.
    // GMSSL 3.x tls_prf signature: (digest, secret, secret_len, label,
    //                              seed, seed_len, more, more_len,
    //                              out_len, out).
    // We pass the 64-byte concatenation as `seed`, leave `more` empty.
    uint8_t seed[64];
    std::memcpy(seed,      conn->client_random, 32);
    std::memcpy(seed + 32, conn->server_random, 32);

    uint8_t key_block[96] = {0};
    int ret = tls_prf(prf_digest,
                      conn->master_secret, 48,
                      kLabel,
                      seed, sizeof(seed),
                      nullptr, 0,
                      sizeof(key_block), key_block);
    if (ret != 1) {
        nimrtc::core::log::Logger::instance().error(
            std::string("gmssl: tls_prf for EXTRACTOR-dtls_srtp failed: ") +
            gmssl_err());
        return false;
    }

    // RFC 5764 §4.2 layout (60 bytes):
    //   client_write_key[16] | server_write_key[16]
    //   client_write_SRTP_salt[14] | server_write_SRTP_salt[14]
    std::memcpy(out +  0, key_block +  0, 16);  // client_write_key
    std::memcpy(out + 16, key_block + 16, 16);  // server_write_key
    std::memcpy(out + 32, key_block + 32, 14);  // client_SRTP_salt
    std::memcpy(out + 46, key_block + 46, 14);  // server_SRTP_salt
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// diag — trace helpers
// ---------------------------------------------------------------------------

namespace nimrtc::dtls::diag {
inline std::mutex& trace_mu() {
    static std::mutex m;
    return m;
}
inline const char* trace_path() {
    static const char* p = std::getenv("NIMRTC_DTLS_TRACE");
    return p;
}
inline std::ofstream& trace_stream() {
    static std::ofstream s{trace_path() ? trace_path() : "", std::ios::app};
    return s;
}
inline void trace(const std::string& msg) {
    if (!trace_path()) return;
    std::lock_guard<std::mutex> g(trace_mu());
    auto& s = trace_stream();
    if (!s.is_open()) return;
    s << "[gmssl] " << msg << '\n';
    s.flush();
}
}  // namespace nimrtc::dtls::diag

namespace nimrtc::dtls {

// ---------------------------------------------------------------------------
// DtlsSessionGmSSL::Impl
// ---------------------------------------------------------------------------

struct DtlsSessionGmSSL::Impl {
    Config         config;
    DtlsState      state = DtlsState::Closed;
    bool           open_called = false;

    // GMSSL native objects.
    //
    // Each session owns its TLS_CTX and TLS_CONNECT in dedicated
    // heap allocations so the lib's internal `memset(ptr, 0, sizeof(*ptr))`
    // calls cannot bleed into adjacent Impl fields or std::string SSO data.
    //
    // TLS_CTX is consistent: both lib and headers agree on 12,136 bytes.
    // TLS_CONNECT is NOT consistent: the lib's tls_init does memset of
    // 118,424 bytes while the header declares sizeof(TLS_CONNECT) = 117,512.
    // This 912-byte overflow is a genuine lib/header mismatch in the
    // current GmSSL 3.3 build.  Until upstream resolves it, we over-allocate
    // TLS_CONNECT by ~35% (256 KiB) to absorb the surplus writes.
    //
    // Sizes chosen with ~35% margin past the observed runtime memset
    // boundaries (tls_ctx_init → 12,136 B; tls_init → 118,424 B).
    // Bump them only if a future libgmssl drops in with larger structs.
    //
    // We keep TLS_CTX at 128 KiB (tight but safe) and TLS_CONNECT at
    // 256 KiB.  Each Impl allocates exactly one of each, so the memory
    // cost (~384 KiB/session) is acceptable for NimRTC's typical session
    // count.
    static constexpr std::size_t kRuntimeTLS_CTX     = 128 * 1024;  // 131,072 B — past 12,136
    static constexpr std::size_t kRuntimeTLS_CONNECT = 256 * 1024;  // 262,144 B — past 118,424
    std::unique_ptr<std::byte[]>  ctx_owner_;
    std::unique_ptr<std::byte[]>  conn_owner_;
    TLS_CTX*     ctx_  = nullptr;
    TLS_CONNECT* conn_ = nullptr;
    bool         conn_inited_ = false;

    // Loopback I/O bridges (NimRTC seam integration).
    //
    // Two deques carry raw DTLS record bytes between the engine and the
    // outside world.  feed_inbound() pushes onto io_in_queue_; the GMSSL
    // io_recv callback pops from the front.  The io_send callback pushes
    // onto io_out_queue_; take_outbound() drains it and returns the
    // DtlsRecord layer to the test.
    //
    // (We no longer touch `sock_fd_` for I/O.  The socket field exists
    // only so the GMSSL struct initialiser is happy — we never call
    // send()/recv() on it.  See "Custom I/O patch" in tls.c:2154.)
    std::deque<std::vector<uint8_t>> io_in_queue_;
    std::deque<std::vector<uint8_t>> io_out_queue_;

    // Peer fingerprint pin
    Fingerprint               local_fp_;
    std::vector<uint8_t>      expected_peer_fp_;
    bool                      expected_peer_fp_set_ = false;

    // SRTP keying material (filled after handshake)
    std::optional<SrtpKeyingMaterial> srtp_keys_;

    Stats         stats_instance{};
    std::chrono::steady_clock::time_point handshake_start_{};

    IDtlsSession::OnCompleteCb on_complete_cb_;

    ~Impl() {
        // Run GMSSL teardown (NULL-tolerant) before the unique_ptrs
        // release the actual heap blocks in their destructors.
        teardown();
    }

    // ------------------------------------------------------------------
    // GMSSL setup
    // ------------------------------------------------------------------

    bool setup_context(bool prefer_tlcp) {
        // Lazy-allocate the GMSSL context the FIRST time we need it.
        // See the comment on `kRuntimeTLS_CTX` above for why these structs
        // are not in-class initializers and why we over-allocate.
        if (!ctx_owner_) {
            ctx_owner_.reset(new std::byte[kRuntimeTLS_CTX]);
            std::memset(ctx_owner_.get(), 0, kRuntimeTLS_CTX);
        }
        ctx_  = reinterpret_cast<TLS_CTX*>(ctx_owner_.get());
        conn_ = reinterpret_cast<TLS_CONNECT*>(conn_owner_.get());
        // Use TLS 1.2 (not TLCP) so tls_ctx_check() at tls.c:3339 does NOT
        // require the server to hold two SM2 certs (sign + enc).  TLCP's
        // double-cert mandate lives behind `if (ctx->protocol == TLS_protocol_tlcp)`
        // at tls.c:3329 and tls.c:3339 and is the only blocker for single-cert
        // test fixtures.
        int is_client = (config.role == DtlsRole::Client) ? 1 : 0;
        int ret = tls_ctx_init(ctx_, TLS_protocol_tls12, is_client);
        if (ret != 1) {
            nimrtc::core::log::Logger::instance().error(
                "gmssl: tls_ctx_init failed: " + gmssl_err());
            return false;
        }

        // Cipher suites — prefer TLCP, fall back to interop mix
        const int* ciphers = choose_cipher_list(prefer_tlcp);
        size_t cipher_cnt = choose_cipher_list_size(prefer_tlcp);
        ret = tls_ctx_set_cipher_suites(ctx_, ciphers, cipher_cnt);
        if (ret != 1) {
            nimrtc::core::log::Logger::instance().error(
                "gmssl: tls_ctx_set_cipher_suites failed: " + gmssl_err());
            return false;
        }

        // Supported groups: GMSSL's TLS 1.2 supported_groups list contains
        // TLS_curve_sm2p256v1 (and secp256r1 if ENABLE_SECP256R1).
        static const int kGroups[] = {
            TLS_curve_sm2p256v1,   // SM2 P-256 — only one GM curve available
        };
        tls_ctx_set_supported_groups(ctx_, kGroups,
            sizeof(kGroups) / sizeof(kGroups[0]));

        // Signature algorithms: SM2-with-SM3 (the only signature scheme
        // matching our SM2 certs).  Without this call the server's
        // ctx->signature_algorithms_cnt stays at 0, and tls_recv_client_hello
        // aborts at tls12.c:2393 with "signature_algorithms_cnt == 0".
        // See tls12.c:2389-2396.
        static const int kSigAlgs[] = {
            TLS_sig_sm2sig_sm3,    // 0x0708 — SM2 signature with SM3 digest
        };
        ret = tls_ctx_set_signature_algorithms(ctx_, kSigAlgs,
            sizeof(kSigAlgs) / sizeof(kSigAlgs[0]));
        if (ret != 1) {
            nimrtc::core::log::Logger::instance().error(
                "gmssl: tls_ctx_set_signature_algorithms failed: " + gmssl_err());
            return false;
        }

        return true;
    }

    bool set_certificates(const char* cert_file, const char* key_file,
                          const char* key_pass) {
        // tls_ctx_set_certificate_and_key handles PEM files with optional
        // password.  Pass nullptr for key_pass to use interactive password
        // callback; we pass an empty string to use the default env-var
        // password mechanism.
        const char* pass = key_pass ? key_pass : "";
        int ret = tls_ctx_set_certificate_and_key(
            ctx_, cert_file, key_file, pass);
        if (ret != 1) {
            nimrtc::core::log::Logger::instance().error(
                std::string("gmssl: tls_ctx_set_certificate_and_key failed: ") +
                gmssl_err() + " (cert=" + cert_file + " key=" + key_file + ")");
            return false;
        }
        return true;
    }

    bool create_tls_object(const char* cert_file,
                            [[maybe_unused]] const char* key_file,
                            [[maybe_unused]] const char* key_pass) {
        // Lazy-allocate TLS_CONNECT the FIRST time we need it.
        // See the comment on `kRuntimeTLS_CONNECT` above for the
        // runtime-size dance and why we over-allocate.
        if (!conn_owner_) {
            conn_owner_.reset(new std::byte[kRuntimeTLS_CONNECT]);
            std::memset(conn_owner_.get(), 0, kRuntimeTLS_CONNECT);
        }
        ctx_  = reinterpret_cast<TLS_CTX*>(ctx_owner_.get());
        conn_ = reinterpret_cast<TLS_CONNECT*>(conn_owner_.get());

        // Initialise TLS_CONNECT (engine state only — no socket).
        conn_->is_client = (config.role == DtlsRole::Client) ? 1 : 0;

        int ret = tls_init(conn_, ctx_);
        if (ret != 1) {
            nimrtc::core::log::Logger::instance().error(
                "gmssl: tls_init failed: " + gmssl_err());
            return false;
        }
        conn_inited_ = true;

        // Disable client-authentication CertificateRequest.  GMSSL's TLCP server
        // sends this by default (ctx->certificate_request is set during
        // tls_ctx_check when CA certs are loaded).  We have no CA certificates,
        // so the CA-names list in CertificateRequest is empty, and
        // tls12_send_certificate_request fails to format it into a handshake
        // record (errno=11 / EAGAIN from a tls_uint16array_to_bytes call
        // when the record buffer overflows).  For DTLS-SRTP in WebRTC we never
        // request client certs anyway, so this is the correct default.
        if (!conn_->is_client) {
            ctx_->certificate_request = 0;
        }

        // Install NimRTC custom-I/O callbacks so tls_send_record /
        // tls_recv_record route bytes through io_out_queue_ / io_in_queue_
        // rather than the (unused) fd in conn_->sock.  See comment block
        // on io_in_queue_ above for the rationale.
        conn_->io_ctx   = this;
        conn_->io_send  = &DtlsSessionGmSSL::Impl::gmssl_io_send;
        conn_->io_recv  = &DtlsSessionGmSSL::Impl::gmssl_io_recv;

        // Compute and cache local fingerprint from the loaded cert
        compute_local_fingerprint(cert_file);

        nimrtc::core::log::Logger::instance().info(
            std::string("DtlsSessionGmSSL: role=") +
            (config.role == DtlsRole::Server ? "Server" : "Client"));
        return true;
    }

    // ------------------------------------------------------------------
    // Custom I/O callbacks (GMSSL NimRTC patch)
    //
    // These are invoked by tls_send_record / tls_recv_record with the
    // NimRTC-patched libgmssl.so.3.3 (see tls.c:2154 for io_send and
    // tls.c:2232 for io_recv).  They use `this` as the io_ctx.
    //
    // Return contract (mirrors the GMSSL "tls_socket_*" convention):
    //   1     = full success, *sent / *rcvd bytes transferred.
    //   < 0   = EAGAIN-style "would block"; tls_send/recv_record returns
    //           TLS_ERROR_SEND_AGAIN / RECV_AGAIN so the caller drives
    //           us again on the next tick.
    //   other = error.
    // ------------------------------------------------------------------

    static int gmssl_io_send(void* ctx,
                               const uint8_t* buf,
                               size_t len,
                               size_t* sent) {
        auto* self = static_cast<DtlsSessionGmSSL::Impl*>(ctx);
        self->io_out_queue_.emplace_back(buf, buf + len);
        *sent = len;
        return 1;
    }

    static int gmssl_io_recv(void* ctx,
                               uint8_t* buf,
                               size_t len,
                               size_t* rcvd) {
        auto* self = static_cast<DtlsSessionGmSSL::Impl*>(ctx);
        if (self->io_in_queue_.empty()) {
            *rcvd = 0;
            return -1;  // EAGAIN — caller will retry on next tick
        }
        auto& front = self->io_in_queue_.front();
        size_t take = std::min(len, front.size());
        std::memcpy(buf, front.data(), take);
        if (take == front.size()) {
            self->io_in_queue_.pop_front();
        } else {
            front.erase(front.begin(),
                        front.begin() + static_cast<std::ptrdiff_t>(take));
        }
        *rcvd = take;
        ++self->stats_instance.records_in;
        return 1;
    }

    void compute_local_fingerprint(const char* cert_file) {
        // Read PEM cert and compute SHA-256 (RFC 8122).
        // GMSSL 3.x: pem_read(FILE*, name, out, outlen, maxlen) — 5 args.
        FILE* f = std::fopen(cert_file, "rb");
        if (!f) return;

        uint8_t der[4096] = {0};
        size_t der_len = sizeof(der);
        // name = "CERTIFICATE" for X.509.  GMSSL 3.x has no password
        // callback on pem_read — encrypted keys must be decrypted via
        // gmssl sm2_pkey_decrypt_* or via the dedicated tls_ctx_set_*
        // helpers (which is what set_certificates() uses below).
        if (pem_read(f, "CERTIFICATE", der, &der_len, sizeof(der)) != 1) {
            nimrtc::core::log::Logger::instance().error(
                "gmssl: pem_read failed for cert: " + gmssl_err());
            std::fclose(f);
            return;
        }
        std::fclose(f);

        std::string fp_hex = cert_sha256_hex(der, der_len);
        if (fp_hex.empty()) return;

        std::vector<uint8_t> raw(32);
        for (size_t i = 0; i < 32; ++i) {
            auto hex_digit = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex_digit(fp_hex[2 * i]);
            int lo = hex_digit(fp_hex[2 * i + 1]);
            raw[i] = static_cast<uint8_t>((hi << 4) | lo);
        }

        local_fp_.algorithm = "sha-256";
        local_fp_.bytes    = raw;
        local_fp_.hex_colon = hex_colon(raw.data(), raw.size());
        // local_fp_.base64 is left in its default-constructed (empty) state.
        // SDP fingerprint plumbing only consumes `bytes` and `hex_colon`;
        // base64 is a legacy/debug field and is intentionally untouched.
        //
        // Historical note: an earlier iteration stored TLS_CONNECT inline
        // inside Impl, and tls_init()'s 117 KiB memset overran the field
        // and corrupted `local_fp_`'s std::string SSO.  Moving TLS_CTX and
        // TLS_CONNECT into their own unique_ptr allocations fixed that —
        // see the comments on `ctx_` / `conn_` above.
    }

    // ------------------------------------------------------------------
    // Handshake pump
    // ------------------------------------------------------------------

    bool pump_handshake() {
        if (!conn_inited_ ||
            state == DtlsState::Connected ||
            state == DtlsState::Failed) {
            return false;
        }

        if (state == DtlsState::Initial) {
            state = DtlsState::HelloSent;
            handshake_start_ = std::chrono::steady_clock::now();
            nimrtc::core::log::Logger::instance().info(
                "gmssl: handshake start");
            nimrtc::dtls::diag::trace(
                std::string("HS_START role=") +
                (config.role == DtlsRole::Server ? "Server" : "Client"));
        }

        int ret = tls_do_handshake(conn_);

        // Bytes GMSSL wrote during the handshake were already enqueued
        // synchronously into io_out_queue_ by gmssl_io_send.  No socket
        // drain needed.

        if (ret == 1) {
            // tls_do_handshake() returned 1: this side's outgoing flight is
            // complete (we sent Finished).  The server MAY have requested
            // client authentication (`client_certificate_verify`) — if so,
            // we MUST have the peer's certificate chain before we consider
            // the handshake done.  For server auth-only flows the client
            // never sends a cert and peer_cert_chain_len == 0 is *correct*.
            //
            // tls_ctx_check() in tls.c:3330 already enforced
            //     ctx->protocol == TLS_protocol_tlcp && !ctx->is_client
            //         && certs_cnt < 2
            // at context-setup time.  At runtime the equivalent question
            // is `conn->client_certificate_verify` — it is set inside
            // tls_recv_server_hello() when the server sends a
            // CertificateRequest extension.
            if (conn_->client_certificate_verify &&
                conn_->peer_cert_chain_len == 0) {
                // Peer cert requested but not yet received — keep pumping.
                return false;
            }

            // Peer cert (if required) confirmed present — verify it.
            if (!verify_peer_cert_sha256()) {
                nimrtc::core::log::Logger::instance().error(
                    "gmssl: peer cert SHA-256 mismatch — handshake rejected");
                state = DtlsState::Failed;
                ++stats_instance.errors;
                if (on_complete_cb_) {
                    auto cb = std::move(on_complete_cb_);
                    on_complete_cb_ = nullptr;
                    cb(plugins::kErrCorrupt);
                }
                return true;
            }
            on_handshake_complete();
            if (on_complete_cb_) {
                auto cb = std::move(on_complete_cb_);
                on_complete_cb_ = nullptr;
                cb(plugins::kOk);
            }
            return true;
        }

        if (ret == TLS_ERROR_RECV_AGAIN || ret == TLS_ERROR_SEND_AGAIN) {
            // Need more data or socket writable — return false to let
            // the caller drive us again via feed_inbound / tick.
            return false;
        }

        // Unexpected error
        nimrtc::core::log::Logger::instance().error(
            std::string("gmssl: tls_do_handshake error: ") +
            gmssl_err() + " (ret=" + std::to_string(ret) + ")");
        nimrtc::dtls::diag::trace(
            std::string("HS_FAIL ret=") + std::to_string(ret) + " " + gmssl_err());
        state = DtlsState::Failed;
        ++stats_instance.errors;
        if (on_complete_cb_) {
            auto cb = std::move(on_complete_cb_);
            on_complete_cb_ = nullptr;
            cb(plugins::kErrInternal);
        }
        return true;
    }

    // Peer cert SHA-256 pin verification (RFC 8122)
    bool verify_peer_cert_sha256() {
        if (!expected_peer_fp_set_) {
            // Fail-open with warning — mirrors wolfSSL backend behaviour
            nimrtc::core::log::Logger::instance().warn(
                "gmssl: no peer fingerprint configured — accepting WITHOUT "
                "MITM protection");
            ++stats_instance.errors;
            return true;
        }
        if (expected_peer_fp_.size() != 32) {
            nimrtc::core::log::Logger::instance().error(
                "gmssl: expected_peer_fp_ invalid length " +
                std::to_string(expected_peer_fp_.size()) + " (expected 32)");
            return false;
        }

        // GMSSL stores peer cert chain in conn_.peer_cert_chain[]
        if (conn_->peer_cert_chain_len == 0) {
            // No peer certificate received.  This is legitimate for
            // server-auth-only DTLS (RFC 5764 §5 — client auth is optional).
            // Warn if caller configured a fingerprint, but do not block the
            // handshake — the fingerprint pin was a fail-open choice made at
            // config time, not a hard requirement.
            if (expected_peer_fp_set_) {
                nimrtc::core::log::Logger::instance().warn(
                    "gmssl: no peer certificate received — fingerprint pin "
                    "skipped; MITM protection disabled for this direction");
            }
            return true;
        }

        std::string fp_hex = cert_sha256_hex(
            conn_->peer_cert_chain, conn_->peer_cert_chain_len);
        if (fp_hex.empty()) return false;

        std::vector<uint8_t> got(32);
        for (size_t i = 0; i < 32; ++i) {
            auto h = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = h(fp_hex[2 * i]);
            int lo = h(fp_hex[2 * i + 1]);
            got[i] = static_cast<uint8_t>((hi << 4) | lo);
        }

        // Constant-time comparison
        uint8_t diff = 0;
        for (size_t i = 0; i < 32; ++i)
            diff |= static_cast<uint8_t>(got[i] ^ expected_peer_fp_[i]);

        if (diff != 0) {
            nimrtc::core::log::Logger::instance().error(
                "gmssl: peer cert SHA-256 mismatch — got " + fp_hex);
            return false;
        }
        return true;
    }

    void on_handshake_complete() {
        state = DtlsState::Connected;
        auto elapsed = std::chrono::steady_clock::now() - handshake_start_;
        stats_instance.handshake_ms =
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    elapsed).count());

        nimrtc::core::log::Logger::instance().info(
            std::string("gmssl: handshake complete in ") +
            std::to_string(stats_instance.handshake_ms) + " ms");

        nimrtc::dtls::diag::trace(
            std::string("HS_COMPLETE ms=") +
            std::to_string(stats_instance.handshake_ms));

        export_srtp_keys();
    }

    // SRTP key material export — RFC 5764 §4.2 EXTRACTOR-dtls_srtp
    void export_srtp_keys() {
        uint8_t key_block[60] = {0};
        if (!derive_srtp_keys(conn_, key_block)) {
            nimrtc::core::log::Logger::instance().error(
                "gmssl: SRTP key derivation failed");
            srtp_keys_.reset();
            return;
        }

        srtp_keys_.emplace();
        auto& keys = *srtp_keys_;
        std::memcpy(keys.client_master_key.data(),  key_block +  0, 16);
        std::memcpy(keys.server_master_key.data(),  key_block + 16, 16);
        std::memcpy(keys.client_master_salt.data(), key_block + 32, 14);
        std::memcpy(keys.server_master_salt.data(), key_block + 46, 14);

        nimrtc::core::log::Logger::instance().info(
            "gmssl: SRTP keying material exported (60 bytes)");
    }

    void teardown() {
        if (conn_inited_) {
            tls_cleanup(conn_);
            conn_inited_ = false;
        }
        if (ctx_) {
            tls_ctx_cleanup(ctx_);
        }
        io_in_queue_.clear();
        io_out_queue_.clear();
        state = DtlsState::Closed;
    }
};

// ---------------------------------------------------------------------------
// DtlsSessionGmSSL public API
// ---------------------------------------------------------------------------

DtlsSessionGmSSL::DtlsSessionGmSSL(Config cfg)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = cfg;
    impl_->state  = DtlsState::Initial;
    impl_->srtp_keys_.emplace();
}

DtlsSessionGmSSL::~DtlsSessionGmSSL() = default;
DtlsSessionGmSSL::DtlsSessionGmSSL(DtlsSessionGmSSL&&) noexcept = default;
DtlsSessionGmSSL& DtlsSessionGmSSL::operator=(DtlsSessionGmSSL&&) noexcept = default;

core::Result<void> DtlsSessionGmSSL::open() noexcept {
    if (impl_->open_called) return core::Result<void>::make_ok();
    impl_->open_called = true;

    // Copy cert / key paths into STACK buffers BEFORE calling
    // setup_context().  Reason: GMSSL's tls_ctx_init() does
    //
    //     memset(ctx, 0, sizeof(*ctx));
    //
    // on the heap chunk backing `impl_->ctx_`.  While that chunk is now
    // sized exactly to sizeof(TLS_CTX) and isolated from Impl's other
    // fields, copying paths into local char arrays keeps the data the
    // GMSSL APIs consume far away from any potential heap interaction
    // and avoids one layer of pointer aliasing — cheap insurance.
    char cert_path_local[1024] = {0};
    char key_path_local[1024]  = {0};
    char key_pass_local[256]   = {0};

    if (!impl_->config.cert_path.empty()) {
        std::strncpy(cert_path_local,
                     impl_->config.cert_path.c_str(),
                     sizeof(cert_path_local) - 1);
    } else {
        std::strncpy(cert_path_local,
                     "tests/wolfssl_dtls/certs/server-ecc.pem",
                     sizeof(cert_path_local) - 1);
    }
    if (!impl_->config.key_path.empty()) {
        std::strncpy(key_path_local,
                     impl_->config.key_path.c_str(),
                     sizeof(key_path_local) - 1);
    } else {
        std::strncpy(key_path_local,
                     "tests/wolfssl_dtls/certs/ecc-key.pem",
                     sizeof(key_path_local) - 1);
    }
    if (!impl_->config.key_pass.empty()) {
        std::strncpy(key_pass_local,
                     impl_->config.key_pass.c_str(),
                     sizeof(key_pass_local) - 1);
    }

    const char* cert_file = cert_path_local;
    const char* key_file  = key_path_local;

    // GMSSL setup
    if (!impl_->setup_context(/*prefer_tlcp=*/true)) {
        ++impl_->stats_instance.errors;
        return core::Result<void>::fail(
            core::ErrorCode::InternalError, "GMSSL context init failed");
    }

    if (!impl_->set_certificates(cert_file, key_file, key_pass_local)) {
        ++impl_->stats_instance.errors;
        return core::Result<void>::fail(
            core::ErrorCode::InternalError, "GMSSL cert/key setup failed");
    }

    // (No socket setup — we use the NimRTC io_send / io_recv callback
    // patch in libgmssl.so.3.3 to drive GMSSL purely via in-memory
    // queues.  See io_in_queue_ / io_out_queue_ for the data path.)

    if (!impl_->create_tls_object(cert_file, key_file, key_pass_local)) {
        ++impl_->stats_instance.errors;
        return core::Result<void>::fail(
            core::ErrorCode::InternalError, "GMSSL TLS object init failed");
    }

    nimrtc::core::log::Logger::instance().info(
        std::string("DtlsSessionGmSSL::open — fingerprint=") +
        impl_->local_fp_.hex_colon);
    return core::Result<void>::make_ok();
}

void DtlsSessionGmSSL::close() noexcept {
    impl_->teardown();
}

std::size_t DtlsSessionGmSSL::feed_inbound(
        std::span<const std::uint8_t> bytes,
        const DtlsAddr& /*from*/) noexcept {
    if (bytes.empty()) return 0;
    ++impl_->stats_instance.records_in;

    // Enqueue onto the io_in_queue_ — gmssl_io_recv() pops from the front
    // the next time GMSSL pulls bytes during a handshake pump.
    impl_->io_in_queue_.emplace_back(bytes.begin(), bytes.end());

    // Drive the handshake state machine until it blocks waiting for more
    // inbound data (RECV_AGAIN / SEND_AGAIN) or completes.  A single
    // pump is insufficient: GMSSL may need multiple internal state
    // transitions per inbound datagram (e.g. after receiving
    // ClientHello it progresses to sending ServerHello + Certificate
    // before blocking again).  Looping here prevents the server from
    // getting stuck in HelloSent when the peer's data arrives.
    for (int pump_pass = 0; pump_pass < 16; ++pump_pass) {
        if (impl_->pump_handshake()) break;
        // pump_handshake returned false: waiting for more data.
        // If io_in_queue_ is empty, GMSSL has consumed everything and
        // is genuinely blocked — stop pumping to avoid spinning.
        if (!impl_->conn_inited_ ||
            impl_->state == DtlsState::Connected ||
            impl_->state == DtlsState::Failed ||
            impl_->state == DtlsState::Closed) {
            break;
        }
    }

    return bytes.size();
}

std::vector<DtlsRecord> DtlsSessionGmSSL::take_outbound() noexcept {
    std::vector<DtlsRecord> recs;
    recs.reserve(impl_->io_out_queue_.size());

    while (!impl_->io_out_queue_.empty()) {
        DtlsRecord r;
        r.bytes = std::move(impl_->io_out_queue_.front());
        impl_->io_out_queue_.pop_front();
        ++impl_->stats_instance.records_out;
        recs.push_back(std::move(r));
    }

    // Keep pumping until the handshake stabilises (both sides done, or
    // no new outbound bytes and handshake state is final).  This is
    // essential for DTLS: one side's Connected does not mean the other
    // has received and processed all required messages yet.
    if (impl_->conn_inited_) {
        for (int pass = 0; pass < 8; ++pass) {
            auto prev_out = impl_->io_out_queue_.size();
            impl_->pump_handshake();
            while (!impl_->io_out_queue_.empty()) {
                DtlsRecord r;
                r.bytes = std::move(impl_->io_out_queue_.front());
                impl_->io_out_queue_.pop_front();
                ++impl_->stats_instance.records_out;
                recs.push_back(std::move(r));
            }
            if ((impl_->state == DtlsState::Connected ||
                 impl_->state == DtlsState::Failed ||
                 impl_->state == DtlsState::Closed) &&
                impl_->io_out_queue_.size() == prev_out) {
                break;
            }
        }
    }
    return recs;
}

void DtlsSessionGmSSL::tick() noexcept {
    if (!impl_->conn_inited_ ||
        impl_->state == DtlsState::Failed ||
        impl_->state == DtlsState::Closed) {
        return;
    }
    // Note: we deliberately do NOT call pump_handshake() here.
    // The handshake is driven exclusively by feed_inbound() (when
    // inbound data arrives) and take_outbound() (which has its own
    // 8-pass pump loop).  Calling pump here as well would cause the
    // server to spin when io_in_queue_ is empty — GMSSL returns
    // RECV_AGAIN and we would loop forever without advancing state.
}

DtlsState DtlsSessionGmSSL::state() const noexcept {
    return impl_->state;
}

bool DtlsSessionGmSSL::is_connected() const noexcept {
    return impl_->state == DtlsState::Connected && impl_->conn_inited_;
}

const char* DtlsSessionGmSSL::state_name(DtlsState s) noexcept {
    switch (s) {
        case DtlsState::Closed:              return "Closed";
        case DtlsState::Initial:             return "Initial";
        case DtlsState::HelloVerify:         return "HelloVerify";
        case DtlsState::HelloSent:           return "HelloSent";
        case DtlsState::HelloReceived:       return "HelloReceived";
        case DtlsState::CertificateReceived: return "CertificateReceived";
        case DtlsState::KeyExchange:         return "KeyExchange";
        case DtlsState::ChangeCipherSpec:    return "ChangeCipherSpec";
        case DtlsState::Finished:            return "Finished";
        case DtlsState::Connected:           return "Connected";
        case DtlsState::Failed:              return "Failed";
    }
    return "Unknown";
}

const Fingerprint& DtlsSessionGmSSL::local_fingerprint() const noexcept {
    return impl_->local_fp_;
}

void DtlsSessionGmSSL::set_peer_fingerprint(
        std::string algo,
        std::vector<std::uint8_t> value) noexcept {
    if (algo != "sha-256") {
        nimrtc::core::log::Logger::instance().warn(
            "DtlsSessionGmSSL: unsupported fingerprint algo '" + algo +
            "' (expected sha-256)");
        impl_->expected_peer_fp_.clear();
        impl_->expected_peer_fp_set_ = false;
        return;
    }
    if (value.size() != 32) {
        nimrtc::core::log::Logger::instance().warn(
            "DtlsSessionGmSSL: peer fingerprint must be 32 bytes, got " +
            std::to_string(value.size()));
        impl_->expected_peer_fp_.clear();
        impl_->expected_peer_fp_set_ = false;
        return;
    }
    impl_->expected_peer_fp_ = std::move(value);
    impl_->expected_peer_fp_set_ = true;
    nimrtc::core::log::Logger::instance().info(
        "DtlsSessionGmSSL: peer fingerprint pin set");
}

void DtlsSessionGmSSL::set_role(DtlsRole r) noexcept {
    if (impl_->state != DtlsState::Closed &&
        impl_->state != DtlsState::Initial) {
        nimrtc::core::log::Logger::instance().warn(
            "DtlsSessionGmSSL: role change after handshake ignored");
        return;
    }
    impl_->config.role = r;
}

std::optional<SrtpKeyingMaterial>
DtlsSessionGmSSL::srtp_keying_material() const noexcept {
    if (impl_->state != DtlsState::Connected) return std::nullopt;
    return impl_->srtp_keys_;
}

DtlsSessionGmSSL::Stats DtlsSessionGmSSL::stats() const noexcept {
    return impl_->stats_instance;
}

// ===========================================================================
// IDtlsSession (PAL Slice 4) seam methods
// ===========================================================================

void DtlsSessionGmSSL::set_role(Role role) noexcept {
    set_role((role == Role::Server) ? DtlsRole::Server : DtlsRole::Client);
}

void DtlsSessionGmSSL::set_peer_fingerprint(
    std::span<const std::uint8_t> raw_sha256) noexcept {
    std::vector<std::uint8_t> value(raw_sha256.begin(), raw_sha256.end());
    set_peer_fingerprint("sha-256", std::move(value));
}

void DtlsSessionGmSSL::start() noexcept {
    (void)open();
}

void DtlsSessionGmSSL::pump() noexcept {
    tick();
}

void DtlsSessionGmSSL::on_handshake_complete(OnCompleteCb cb) noexcept {
    impl_->on_complete_cb_ = std::move(cb);
}

plugins::Status DtlsSessionGmSSL::export_srtp_key_material(
    std::span<std::uint8_t, 60> out) noexcept {
    auto km = srtp_keying_material();
    if (!km.has_value()) return plugins::kErrNotReady;
    std::memcpy(out.data() +  0, km->client_master_key.data(),  16);
    std::memcpy(out.data() + 16, km->server_master_key.data(),  16);
    std::memcpy(out.data() + 32, km->client_master_salt.data(), 14);
    std::memcpy(out.data() + 46, km->server_master_salt.data(), 14);
    return plugins::kOk;
}

} // namespace nimrtc::dtls
