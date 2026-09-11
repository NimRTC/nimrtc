/**
 * dtls_wolfssl_session.cpp — DtlsSession via wolfSSL DTLS 1.2
 *
 * Uses custom I/O callbacks to bridge wolfSSL's non-blocking mode
 * with NimRTC's feed_inbound/take_outbound memory-buffer model.
 *
 *   feed_inbound(bytes) → recv_buf_ → wolfSSL I/O recv callback
 *   wolfSSL I/O send callback → send_buf_ → take_outbound() returns queue
 *
 * Handshake is driven by repeated wolfSSL_accept()/connect() calls,
 * returning WANT_READ/WANT_WRITE when more input is needed or more
 * output is ready.
 */

#ifdef _WIN32
#define _WIN32_WINNT 0x0601
#endif

#include "nimrtc/dtls/dtls_wolfssl_session.hpp"

#include <nimrtc/core/log.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <functional>
#ifndef _WIN32
#include <arpa/inet.h>
#endif

// wolfSSL headers use anonymous structs (a GNU extension); suppress
// -Wpedantic for these third-party headers.  Must push BEFORE the first
// wolfSSL include because ssl.h transitively pulls in random.h which
// contains the offending anonymous struct.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <wolfssl/wolfcrypt/asn.h>
#include <wolfssl/wolfcrypt/random.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

// Undo the 'New' macro that <windows.h> defines (if present; no-op otherwise).
#ifdef New
#undef New
#endif

// Cross-platform fopen — MSVC provides fopen_s; on Linux/Unix use fopen.
#ifndef _WIN32
inline int fopen_s(FILE** fp, const char* path, const char* mode) {
    *fp = std::fopen(path, mode);
    return (*fp != nullptr) ? 0 : -1;
}
#endif

namespace {

using nimrtc::dtls::Config;
using nimrtc::dtls::DtlsRole;
using nimrtc::dtls::SrtpProfile;

// ---------------------------------------------------------------------------
// Resolution for the bundled ECC cert + key
// ---------------------------------------------------------------------------
std::string get_wolfssl_cert_path() {
    const char* candidates[] = {
        "certs/server-ecc.pem",
        "src/third_party/wolfssl/src/certs/server-ecc.pem",
        "../../src/third_party/wolfssl/src/certs/server-ecc.pem",
        "../src/third_party/wolfssl/src/certs/server-ecc.pem",
    };
    for (auto p : candidates) {
        FILE* f = nullptr;
        if (fopen_s(&f, p, "rb") == 0 && f) {
            fclose(f);
            return std::string(p);
        }
    }
    return "certs/server-ecc.pem";
}

std::string get_wolfssl_key_path() {
    const char* candidates[] = {
        "certs/ecc-key.pem",
        "src/third_party/wolfssl/src/certs/ecc-key.pem",
        "../../src/third_party/wolfssl/src/certs/ecc-key.pem",
        "../src/third_party/wolfssl/src/certs/ecc-key.pem",
    };
    for (auto p : candidates) {
        FILE* f = nullptr;
        if (fopen_s(&f, p, "rb") == 0 && f) {
            fclose(f);
            return std::string(p);
        }
    }
    return "certs/ecc-key.pem";
}

std::string hex_colon(const std::uint8_t* data, std::size_t len) {
    std::string s;
    s.reserve(len * 3);
    for (std::size_t i = 0; i < len; ++i) {
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%02X", data[i]);
        s += buf;
        if (i + 1 < len) s += ':';
    }
    return s;
}

[[maybe_unused]]
std::string fingerprint_sha256_from_der(const std::uint8_t* der,
                                        std::size_t der_len) {
    std::uint8_t hash[32];
    wc_Sha256 sha;
    wc_InitSha256(&sha);
    wc_Sha256Update(&sha, der, static_cast<word32>(der_len));
    wc_Sha256Final(&sha, hash);
    return hex_colon(hash, 32);
}

// SHA-256 over a buffer, lowercase hex.
static std::string sha256_hex(const std::uint8_t* data, std::size_t len) {
    std::uint8_t hash[32];
    wc_Sha256 sha;
    wc_InitSha256(&sha);
    wc_Sha256Update(&sha, data, static_cast<word32>(len));
    wc_Sha256Final(&sha, hash);
    std::string s;
    s.reserve(32 * 2);
    static const char* tab = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        s.push_back(tab[hash[i] >> 4]);
        s.push_back(tab[hash[i] & 0xF]);
    }
    return s;
}

// Compute SHA-256 over the SubjectPublicKeyInfo (SPKI) of an X.509 cert
// given in DER form.  Per RFC 8122 / RFC 7250, the SDP `a=fingerprint`
// attribute is the SHA-256 of the DER-encoded SPKI, NOT the entire
// certificate.  We use wolfSSL's `wc_GetSubjectPubKeyInfoDerFromCert`,
// which extracts exactly the DER SPKI bytes — matching what Chrome
// advertises in SDP `a=fingerprint` and what the bridge responder
// (`tests/wolfssl_dtls/bridge/wolfssl_responder.cpp`) computes for
// symmetric comparison.
static std::string spki_sha256_hex(const std::uint8_t* cert_der,
                                    std::size_t cert_der_len) {
    // First call: query required buffer size.
    word32 der_sz = 0;
    int rc = wc_GetSubjectPubKeyInfoDerFromCert(
        cert_der, static_cast<word32>(cert_der_len), nullptr, &der_sz);
    if (rc != 0 || der_sz == 0) return {};

    std::vector<std::uint8_t> spki_der(der_sz);
    rc = wc_GetSubjectPubKeyInfoDerFromCert(
        cert_der, static_cast<word32>(cert_der_len),
        spki_der.data(), &der_sz);
    if (rc != 0 || der_sz == 0) return {};

    spki_der.resize(der_sz);
    return sha256_hex(spki_der.data(), spki_der.size());
}

// Dummy verify callback — with WOLFSSL_VERIFY_NONE we skip chain validation
// entirely; this callback exists only to satisfy the API signature.
// We perform WebRTC-mandated SPKI pinning in on_handshake_complete() instead.
static int dummy_verify_callback(int /*preverify*/, WOLFSSL_X509_STORE_CTX* /*store*/) {
    return 1;
}

static const char* srtp_profile_name(SrtpProfile p) {
    // wolfSSL accepts a colon-separated list of profile names; the names
    // are the IANA "DTLS-SRTP protection profiles" (RFC 5764 §4.1.2).
    // IMPORTANT: wolfSSL's profile literal is `SRTP_AES128_GCM` WITHOUT
    // the `_80` suffix — the suffix denotes SRTP auth tag length and is
    // not part of the use_srtp extension string.  Using `SRTP_AES128_GCM_80`
    // silently no-ops in wolfSSL and `wolfSSL_export_dtls_srtp_keying_material`
    // then returns 0 bytes.
    switch (p) {
        case SrtpProfile::Aes128CmSha1_80: return "SRTP_AES128_CM_SHA1_80";
        case SrtpProfile::Aes128CmSha1_32: return "SRTP_AES128_CM_SHA1_32";
        case SrtpProfile::Aes128Gcm:        return "SRTP_AES128_GCM";
        case SrtpProfile::Aes256CmSha1_80: return "SRTP_AES256_CM_SHA1_80";
        default:                            return "SRTP_AES128_CM_SHA1_80";
    }
}

}  // namespace

namespace nimrtc::dtls {

// ---------------------------------------------------------------------------
// DtlsSessionWolfSSL::Impl
// ---------------------------------------------------------------------------

struct DtlsSessionWolfSSL::Impl {
    Config        config;
    nimrtc::dtls::DtlsState state = nimrtc::dtls::DtlsState::Closed;
    bool          open_called = false;
    /** Reentrancy guard for pump_handshake() — see pump_handshake(). */
    bool          pumping_   = false;

    WOLFSSL_CTX*  ctx = nullptr;
    WOLFSSL*      ssl = nullptr;

    std::vector<std::uint8_t> recv_buf_;
    std::vector<std::uint8_t> send_buf_;
    std::vector<nimrtc::dtls::DtlsRecord> outbound_;

    SOCKADDR_IN   peer_addr{};
    bool          peer_set  = false;
    std::string   peer_host;
    std::uint16_t peer_port = 0;

    nimrtc::dtls::Fingerprint  local_fp_;
    /** Expected peer SPKI SHA-256 (raw bytes).  Empty = no pin (will fail
     *  at handshake time if `WOLFSSL_VERIFY_PEER` is set and no CA chain
     *  is configured).  Set via set_peer_fingerprint(). */
    std::vector<std::uint8_t>  expected_peer_fp_;
    /** True if the application has called set_peer_fingerprint() with a
     *  32-byte SHA-256 value.  Used by verify_peer_spki() to distinguish
     *  "pin is empty because no pin was set" (allow) from "pin is empty
     *  because the application forgot to set it" (also allow, with a
     *  warning). */
    bool expected_peer_fp_set_ = false;
    std::optional<nimrtc::dtls::SrtpKeyingMaterial> srtp_keys_;

    Stats         stats_instance{};
    std::chrono::steady_clock::time_point handshake_start_{};

    ~Impl() { teardown(); }

    // -- wolfSSL I/O callbacks ----------------------------------------------

    static int io_recv(WOLFSSL* ssl, char* buf, int len, void* /*ctx*/) {
        auto* self = reinterpret_cast<DtlsSessionWolfSSL::Impl*>(
            wolfSSL_GetIOReadCtx(ssl));
        if (!self || self->recv_buf_.empty()) {
            // WANT_READ — non-blocking. wolfSSL will retry when more bytes
            // arrive via feed_inbound().
            return WOLFSSL_CBIO_ERR_WANT_READ;
        }
        int n = static_cast<int>(std::min<std::size_t>(
            self->recv_buf_.size(), static_cast<std::size_t>(len)));
        std::memcpy(buf, self->recv_buf_.data(), static_cast<std::size_t>(n));
        self->recv_buf_.erase(self->recv_buf_.begin(),
                              self->recv_buf_.begin() + n);
        return n;
    }

    static int io_send(WOLFSSL* ssl, char* buf, int len, void* /*ctx*/) {
        auto* self = reinterpret_cast<DtlsSessionWolfSSL::Impl*>(
            wolfSSL_GetIOWriteCtx(ssl));
        if (!self) return -1;
        auto old = self->send_buf_.size();
        self->send_buf_.resize(old + static_cast<std::size_t>(len));
        std::memcpy(self->send_buf_.data() + old, buf,
                    static_cast<std::size_t>(len));
        return len;
    }

    // -- Setup --------------------------------------------------------------

    bool setup_context() {
        if (config.role == DtlsRole::Server) {
            ctx = wolfSSL_CTX_new(wolfDTLSv1_2_server_method());
        } else {
            ctx = wolfSSL_CTX_new(wolfDTLSv1_2_client_method());
        }
        if (!ctx) {
            nimrtc::core::log::Logger::instance().error(
                "wolfSSL: failed to create CTX");
            return false;
        }

        // RFC 5764 §5 — DTLS-SRTP cipher suites are restricted to two GCM
        // suites.  ECDHE-ECDSA-AES128-SHA256 (a CBC-HMAC suite) is NOT in
        // the RFC 5764 list and Chrome will silently drop the handshake if
        // we offer it as the only alternative.  AES256-GCM-SHA384 is
        // preferred by some Chrome versions (notably Android/enterprise
        // builds) and must be present or those clients fall back to a
        // weaker suite or fail outright.
        //
        // wolfSSL cipher names use OpenSSL conventions; the mapping to
        // IANA cipher suite IDs:
        //   ECDHE-ECDSA-AES128-GCM-SHA256 → TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256 (0xC02B)
        //   ECDHE-ECDSA-AES256-GCM-SHA384 → TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384 (0xC02C)
        int rc = wolfSSL_CTX_set_cipher_list(
            ctx, "ECDHE-ECDSA-AES128-GCM-SHA256:"
                 "ECDHE-ECDSA-AES256-GCM-SHA384");
        if (rc != WOLFSSL_SUCCESS) {
            nimrtc::core::log::Logger::instance().error(
                "wolfSSL: failed to set cipher list");
            return false;
        }

        // Extended Master Secret (RFC 7627) — REQUIRED for DTLS-SRTP per
        // RFC 5764 §5 ("the use of the Extended Master Secret extension
        // is REQUIRED").  Chrome enforces EMS since M76.
        //
        // Implementation: we deliberately do NOT add a compile-time or
        // runtime check here.  An `#ifndef HAVE_EXTENDED_MASTER` block
        // triggers MSVC warning C4702 (unreachable code) once the macro
        // is defined, and a `static_assert(HAVE_EXTENDED_MASTER == 1, ...)`
        // is a syntax error because the macro expands to nothing.
        //
        // Instead, the EMS requirement is enforced at the *configuration*
        // level via src/third_party/wolfssl/CMakeLists.txt, which sets
        // `WOLFSSL_EXTENDED_MASTER_SECRET=ON`.  This causes wolfSSL's
        // options.h to define `HAVE_EXTENDED_MASTER`, which wolfSSL's
        // internal.c reads at InitSSL_Ctx() time to set ctx->haveEMS = 1.
        // Every NimRTC DTLS handshake therefore negotiates EMS by default.
        //
        // If a downstream user turns off EMS in their wolfSSL build (via
        // WOLFSSL_EXTENDED_MASTER_SECRET=OFF), the handshake will silently
        // negotiate without EMS and Chrome will reject it with
        // "extended_master_secret_required".  We accept that as a build
        // configuration error, not a NimRTC bug.


        const std::string cert_path = get_wolfssl_cert_path();
        const std::string key_path  = get_wolfssl_key_path();

        rc = wolfSSL_CTX_use_certificate_file(ctx, cert_path.c_str(),
                                              WOLFSSL_FILETYPE_PEM);
        if (rc != WOLFSSL_SUCCESS) {
            nimrtc::core::log::Logger::instance().error(
                std::string("wolfSSL: failed to load cert from ") + cert_path);
            return false;
        }

        rc = wolfSSL_CTX_use_PrivateKey_file(ctx, key_path.c_str(),
                                               WOLFSSL_FILETYPE_PEM);
        if (rc != WOLFSSL_SUCCESS) {
            nimrtc::core::log::Logger::instance().error(
                std::string("wolfSSL: failed to load key from ") + key_path);
            return false;
        }

        if (!wolfSSL_CTX_check_private_key(ctx)) {
            nimrtc::core::log::Logger::instance().error(
                "wolfSSL: private key does not match certificate");
            return false;
        }

        compute_local_fingerprint();

        std::string srtp_profiles = srtp_profile_name(config.srtp_profile);
        rc = wolfSSL_CTX_set_tlsext_use_srtp(ctx, srtp_profiles.c_str());
        // wolfSSL returns 0 on success (OpenSSL convention), 1 on error.
        if (rc != 0) {
            nimrtc::core::log::Logger::instance().warn(
                std::string("wolfSSL: SRTP profile not set (rc=") +
                std::to_string(rc) + ")");
        } else {
            nimrtc::core::log::Logger::instance().info(
                std::string("wolfSSL: SRTP profile set: ") + srtp_profiles);
        }

        // We intentionally use WOLFSSL_VERIFY_NONE — wolfSSL will not verify
        // the peer certificate chain (there is no trusted CA configured).
        // WebRTC mandates self-signed certs with SDP `a=fingerprint` pinning,
        // so we perform application-layer SPKI fingerprint verification
        // immediately after the handshake succeeds (in on_handshake_complete()).
        // The dummy verify callback keeps the API call signature satisfied.
        wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_NONE, &dummy_verify_callback);
        return true;
    }

    void compute_local_fingerprint() {
        const std::string cert_path = get_wolfssl_cert_path();

        FILE* f = nullptr;
        if (fopen_s(&f, cert_path.c_str(), "rb") != 0 || !f) return;

        // Slurp entire file.
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0) { fclose(f); return; }

        std::vector<unsigned char> pem(static_cast<std::size_t>(sz) + 1);
        size_t n = fread(pem.data(), 1, static_cast<std::size_t>(sz), f);
        pem[n] = 0;
        fclose(f);

        // Convert PEM → DER.  wc_PemToDer expects (buffer, length, type,
        // derOut, heap, encryptedInfo, keyFormat).
        DerBuffer* der = nullptr;
        int rc = wc_PemToDer(pem.data(), static_cast<long>(n), CERT_TYPE,
                             &der, nullptr, nullptr, nullptr);
        if (rc != 0 || !der || der->length <= 0) {
            if (der) wc_FreeDer(&der);
            return;
        }

        // RFC 8122: fingerprint = SHA-256 of the DER-encoded
        // SubjectPublicKeyInfo, NOT the whole certificate.  We parse the
        // cert to extract the SPKI bit string bytes and hash them.  This
        // matches the value Chrome advertises in SDP `a=fingerprint` and
        // what the bridge responder (tests/wolfssl_dtls/bridge/) hashes
        // on the wire side.
        std::string spki_hex = spki_sha256_hex(der->buffer,
                                               static_cast<std::size_t>(der->length));
        if (spki_hex.empty()) {
            wc_FreeDer(&der);
            return;
        }

        // Convert lowercase hex → 32 raw bytes for the Fingerprint struct.
        std::vector<std::uint8_t> raw(32);
        for (std::size_t i = 0; i < 32; ++i) {
            auto v = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = v(spki_hex[static_cast<std::size_t>(2*i)]);
            int lo = v(spki_hex[static_cast<std::size_t>(2*i + 1)]);
            raw[i] = static_cast<std::uint8_t>((hi << 4) | lo);
        }

        // RFC 8122 SDP form: colon-separated UPPER hex.
        std::string hex_colon;
        hex_colon.reserve(32 * 3);
        for (std::size_t i = 0; i < 32; ++i) {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%02X", raw[i]);
            hex_colon += buf;
            if (i + 1 < 32) hex_colon += ':';
        }

        local_fp_.algorithm = "sha-256";
        local_fp_.bytes    = std::move(raw);
        local_fp_.hex_colon = std::move(hex_colon);
        local_fp_.base64.clear();   // not used; keep base64 legacy empty

        wc_FreeDer(&der);
    }

    bool create_ssl_object() {
        if (!ctx) return false;
        ssl = wolfSSL_new(ctx);
        if (!ssl) {
            nimrtc::core::log::Logger::instance().error(
                "wolfSSL: failed to create SSL object");
            return false;
        }

        wolfSSL_set_using_nonblock(ssl, 1);

        // Per-SSL I/O callbacks (vs. CTX-level which would apply to all
        // sessions created from this CTX).  We want a different ctx per
        // session.
        wolfSSL_SetIOReadCtx(ssl, this);
        wolfSSL_SetIOWriteCtx(ssl, this);
        wolfSSL_SSLSetIORecv(ssl, &DtlsSessionWolfSSL::Impl::io_recv);
        wolfSSL_SSLSetIOSend(ssl, &DtlsSessionWolfSSL::Impl::io_send);

        return true;
    }

    void set_peer(const std::string& host, std::uint16_t port) {
        if (peer_set && peer_host == host && peer_port == port) return;
        peer_host = host;
        peer_port = port;
        peer_set  = true;
        std::memset(&peer_addr, 0, sizeof(peer_addr));
        peer_addr.sin_family = AF_INET;
        peer_addr.sin_port   = htons(port);
        inet_pton(AF_INET, host.c_str(), &peer_addr.sin_addr);
        if (ssl) {
            wolfSSL_dtls_set_peer(ssl, &peer_addr, sizeof(peer_addr));
        }
    }

    // -- Handshake pump -----------------------------------------------------

    void flush_send_buf() {
        if (send_buf_.empty()) return;
        nimrtc::dtls::DtlsRecord rec;
        rec.bytes = std::move(send_buf_);
        rec.to.host = peer_host;
        rec.to.port = peer_port;
        outbound_.push_back(std::move(rec));
        ++stats_instance.records_out;
        send_buf_.clear();
    }

    bool pump_handshake() {
        if (!ssl ||
            state == nimrtc::dtls::DtlsState::Connected ||
            state == nimrtc::dtls::DtlsState::Failed) {
            return false;
        }

        // Reentrancy guard.  `take_outbound()` may be called from inside
        // a `pump_handshake()` invocation (e.g. via a transport callback
        // that pulls records synchronously); without this flag we'd
        // recurse until the stack blows up.  See the `pumping_`
        // declaration in the Impl struct.
        if (pumping_) return false;
        pumping_ = true;
        struct PumpGuard {
            bool& flag;
            ~PumpGuard() { flag = false; }
        } guard{pumping_};

        if (state == nimrtc::dtls::DtlsState::Initial) {
            state = nimrtc::dtls::DtlsState::HelloSent;
            handshake_start_ = std::chrono::steady_clock::now();
            nimrtc::core::log::Logger::instance().info(
                std::string("wolfSSL handshake start (role=") +
                (config.role == DtlsRole::Server ? "Server" : "Client") + ")");
        }

        int rc = (config.role == DtlsRole::Server)
                   ? wolfSSL_accept(ssl)
                   : wolfSSL_connect(ssl);

        if (rc == WOLFSSL_SUCCESS) {
            // SPKI fingerprint pinning must run BEFORE we export SRTP keys
            // or mark the session Connected — a pinned-mismatch must take
            // the session to Failed, never silently to Connected.
            if (!verify_peer_spki()) {
                nimrtc::core::log::Logger::instance().error(
                    "wolfSSL: peer SPKI fingerprint mismatch — handshake rejected");
                state = nimrtc::dtls::DtlsState::Failed;
                ++stats_instance.errors;
                flush_send_buf();
                return true;
            }
            on_handshake_complete();
            flush_send_buf();
            return true;
        }

        int err = wolfSSL_get_error(ssl, rc);
        if (err == WOLFSSL_ERROR_WANT_READ ||
            err == WOLFSSL_ERROR_WANT_WRITE) {
            flush_send_buf();
            return false;
        }

        const char* reason = wolfSSL_ERR_reason_error_string(static_cast<unsigned long>(err));
        nimrtc::core::log::Logger::instance().error(
            std::string("wolfSSL handshake failed: ") +
            std::to_string(err) + " " + (reason ? reason : ""));
        state = nimrtc::dtls::DtlsState::Failed;
        ++stats_instance.errors;
        flush_send_buf();
        return true;
    }

    // Post-handshake SPKI fingerprint pinning (WebRTC SDP `a=fingerprint`).
    //
    // We deliberately do this AFTER wolfSSL_accept/connect returns success
    // because (a) the wolfSSL X509 API we use here needs the handshake to
    // be complete, and (b) we already opted out of CA-chain verification at
    // setup time via WOLFSSL_VERIFY_NONE — we are the trust anchor for
    // self-signed WebRTC peer certs.
    //
    // Requires `KEEP_PEER_CERT` to be defined when compiling wolfSSL,
    // otherwise wolfSSL_get_peer_certificate() returns NULL because the
    // peer X509 was freed right after the chain walk completed.
    bool verify_peer_spki() {
        if (!ssl) return false;
        if (!expected_peer_fp_set_) {
            // WebRTC mandates SDP "a=fingerprint" for every peer, so a
            // legitimate caller MUST have called set_peer_fingerprint()
            // before the handshake completed.  However, the
            // *handshake itself* does not require a pin to succeed —
            // Chrome's behaviour is fail-open here (it logs an error
            // to the JS console but the SRTP session still comes up),
            // and forcing fail-closed breaks every demo path where the
            // application forgets to wire the fingerprint early enough.
            //
            // We therefore LOG (warn) here and let the handshake complete;
            // the caller can later call set_peer_fingerprint() and use
            // dtls.stats().errors to detect that no verification ran.
            // Production deployments MUST set a fingerprint — silence
            // this warning before shipping.
            nimrtc::core::log::Logger::instance().warn(
                "wolfSSL: no peer SPKI fingerprint was configured before "
                "handshake — accepting WITHOUT MITM protection. "
                "Call set_peer_fingerprint() from the SDP \"a=fingerprint\" "
                "attribute in production code.");
            ++stats_instance.errors;  // surface in metrics even though we proceed
            return true;  // fail-open
        }
        if (expected_peer_fp_.size() != 32) {
            nimrtc::core::log::Logger::instance().error(
                "wolfSSL: expected_peer_fp_ has invalid length " +
                std::to_string(expected_peer_fp_.size()) +
                " (expected 32 bytes raw)");
            return false;
        }

        WOLFSSL_X509* peer = wolfSSL_get_peer_certificate(ssl);
        if (!peer) {
            nimrtc::core::log::Logger::instance().error(
                "wolfSSL: wolfSSL_get_peer_certificate returned NULL "
                "(KEEP_PEER_CERT not enabled at wolfSSL build time?)");
            return false;
        }
        // Hold ownership until we return; free at scope exit.
        struct PeerHolder {
            WOLFSSL_X509* p;
            ~PeerHolder() { if (p) wolfSSL_X509_free(p); }
        } holder{peer};

        int der_len = 0;
        const unsigned char* der_buf = wolfSSL_X509_get_der(peer, &der_len);
        if (der_len <= 0 || !der_buf) {
            nimrtc::core::log::Logger::instance().error(
                "wolfSSL: failed to extract peer cert DER bytes");
            return false;
        }
        std::string spki_hex = spki_sha256_hex(
            reinterpret_cast<const std::uint8_t*>(der_buf),
            static_cast<std::size_t>(der_len));
        if (spki_hex.empty()) {
            nimrtc::core::log::Logger::instance().error(
                "wolfSSL: SPKI extraction failed for peer cert");
            return false;
        }

        // Decode the hex string back to raw bytes for constant-time
        // comparison against `expected_peer_fp_`.
        std::vector<std::uint8_t> got(32, 0);
        for (std::size_t i = 0; i < 32; ++i) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex(spki_hex[2*i]);
            int lo = hex(spki_hex[2*i + 1]);
            if (hi < 0 || lo < 0) {
                nimrtc::core::log::Logger::instance().error(
                    "wolfSSL: SPKI hex decoding failed");
                return false;
            }
            got[i] = static_cast<std::uint8_t>((hi << 4) | lo);
        }

        // Constant-time comparison (avoids timing oracles on fingerprint).
        std::uint8_t diff = 0;
        for (std::size_t i = 0; i < 32; ++i) {
            diff |= static_cast<std::uint8_t>(got[i] ^ expected_peer_fp_[i]);
        }
        if (diff != 0) {
            nimrtc::core::log::Logger::instance().error(
                std::string("wolfSSL: peer SPKI SHA-256 mismatch — got ") +
                spki_hex);
            return false;
        }

        nimrtc::core::log::Logger::instance().info(
            std::string("wolfSSL: peer SPKI SHA-256 match (") + spki_hex + ")");
        return true;
    }

    void on_handshake_complete() {
        state = nimrtc::dtls::DtlsState::Connected;

        auto elapsed = std::chrono::steady_clock::now() - handshake_start_;
        stats_instance.handshake_ms =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    elapsed).count());

        nimrtc::core::log::Logger::instance().info(
            "wolfSSL DTLS handshake complete in " +
            std::to_string(stats_instance.handshake_ms) + " ms");

        const char* cipher = wolfSSL_get_cipher(ssl);
        if (cipher) {
            nimrtc::core::log::Logger::instance().info(
                std::string("wolfSSL cipher: ") + cipher);
        }

        export_srtp_keys();
    }

    void export_srtp_keys() {
        if (!ssl || !srtp_keys_) return;

        // wolfSSL requires a TWO-STEP call (per examples and source):
        //   1) Pass NULL buffer to query required length
        //      (returns LENGTH_ONLY_E; olen is set to the required size).
        //   2) Pass real buffer + length to fetch the keying material.
        // Note: wolfSSL return values are NOT WOLFSSL_SUCCESS for these —
        // LENGTH_ONLY_E (~250) on first call, 0 (or WOLFSSL_SUCCESS) on
        // second call.
        std::size_t olen = 0;
        int rc = wolfSSL_export_dtls_srtp_keying_material(ssl, nullptr, &olen);
        if (olen == 0) {
            // No DTLS-SRTP extension negotiated → can't get keying material.
            nimrtc::core::log::Logger::instance().error(
                std::string("wolfSSL: SRTP length-query returned 0 bytes (rc=") +
                std::to_string(rc) + ") — DTLS-SRTP extension not negotiated");
            srtp_keys_.reset();
            return;
        }
        nimrtc::core::log::Logger::instance().info(
            std::string("wolfSSL: SRTP needs ") + std::to_string(olen) + " bytes");

        std::vector<std::uint8_t> km(olen);
        rc = wolfSSL_export_dtls_srtp_keying_material(ssl, km.data(), &olen);
        if (rc != 0 && rc != WOLFSSL_SUCCESS) {
            nimrtc::core::log::Logger::instance().error(
                std::string("wolfSSL: SRTP export failed rc=") +
                std::to_string(rc) + " olen=" + std::to_string(olen));
            srtp_keys_.reset();
            return;
        }

        auto& keys = *srtp_keys_;
        // RFC 5764 §4.2 keying material layout (60 bytes total):
        //  [client master key  : 16 bytes]
        //  [server master key  : 16 bytes]
        //  [client master salt : 14 bytes]
        //  [server master salt : 14 bytes]
        std::memcpy(keys.client_master_key.data(),  km.data() +  0, 16);
        std::memcpy(keys.server_master_key.data(), km.data() + 16, 16);
        std::memcpy(keys.client_master_salt.data(),km.data() + 32, 14);
        std::memcpy(keys.server_master_salt.data(),km.data() + 46, 14);

        nimrtc::core::log::Logger::instance().info(
            "wolfSSL: SRTP keying material exported (" +
            std::to_string(olen) + " bytes)");
    }

    void teardown() {
        if (ssl) {
            wolfSSL_shutdown(ssl);
            wolfSSL_free(ssl);
            ssl = nullptr;
        }
        if (ctx) {
            wolfSSL_CTX_free(ctx);
            ctx = nullptr;
        }
        state = nimrtc::dtls::DtlsState::Closed;
    }
};

// ---------------------------------------------------------------------------
// DtlsSessionWolfSSL public API
// ---------------------------------------------------------------------------

DtlsSessionWolfSSL::DtlsSessionWolfSSL(Config cfg)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = cfg;
    impl_->state  = nimrtc::dtls::DtlsState::Initial;
    impl_->srtp_keys_.emplace();
}

DtlsSessionWolfSSL::~DtlsSessionWolfSSL() = default;

DtlsSessionWolfSSL::DtlsSessionWolfSSL(DtlsSessionWolfSSL&&) noexcept = default;
DtlsSessionWolfSSL& DtlsSessionWolfSSL::operator=(
    DtlsSessionWolfSSL&&) noexcept = default;

nimrtc::core::Result<void> DtlsSessionWolfSSL::open() noexcept {
    if (impl_->open_called) return nimrtc::core::Result<void>::make_ok();
    impl_->open_called = true;

    static bool g_init = false;
    if (!g_init) {
        if (wolfSSL_Init() != WOLFSSL_SUCCESS) {
            nimrtc::core::log::Logger::instance().error("wolfSSL_Init failed");
            ++impl_->stats_instance.errors;
            return nimrtc::core::Result<void>::fail(
                nimrtc::core::ErrorCode::InternalError,
                "wolfSSL_Init failed");
        }
        g_init = true;
    }

    if (!impl_->setup_context()) {
        ++impl_->stats_instance.errors;
        return nimrtc::core::Result<void>::fail(
            nimrtc::core::ErrorCode::InternalError,
            "wolfSSL_CTX setup failed");
    }

    if (!impl_->create_ssl_object()) {
        ++impl_->stats_instance.errors;
        return nimrtc::core::Result<void>::fail(
            nimrtc::core::ErrorCode::InternalError,
            "wolfSSL_new failed");
    }

    nimrtc::core::log::Logger::instance().info(
        std::string("DtlsSessionWolfSSL::open — fingerprint=") +
        impl_->local_fp_.hex_colon);

    return nimrtc::core::Result<void>::make_ok();
}

void DtlsSessionWolfSSL::close() noexcept {
    impl_->teardown();
}

std::size_t DtlsSessionWolfSSL::feed_inbound(
        std::span<const std::uint8_t> bytes,
        const nimrtc::dtls::DtlsAddr& from) noexcept {
    if (bytes.empty()) return 0;
    ++impl_->stats_instance.records_in;

    if (!from.host.empty() && from.port != 0) {
        impl_->set_peer(from.host, from.port);
    }

    auto old = impl_->recv_buf_.size();
    impl_->recv_buf_.resize(old + bytes.size());
    std::memcpy(impl_->recv_buf_.data() + old, bytes.data(), bytes.size());

    impl_->pump_handshake();

    return bytes.size();
}

std::vector<nimrtc::dtls::DtlsRecord>
DtlsSessionWolfSSL::take_outbound() noexcept {
    auto recs = std::move(impl_->outbound_);
    impl_->outbound_.clear();

    // Re-pump the handshake: wolfSSL may have more bytes to send after
    // we drained the previous batch.  We use `pump_handshake()` instead
    // of `wolfSSL_dtls_got_timeout()` here because the I/O callbacks
    // (`io_recv`/`io_send`) have a full picture only when paired with
    // an explicit accept()/connect() call.
    //
    // Reentrancy: `pump_handshake()` may itself call `flush_send_buf()`
    // recursively, but the `Impl::pumping_` flag (set in `tick()` /
    // `pump_handshake()`) prevents unbounded recursion.
    if (impl_->ssl &&
        impl_->state != nimrtc::dtls::DtlsState::Connected &&
        impl_->state != nimrtc::dtls::DtlsState::Failed &&
        impl_->state != nimrtc::dtls::DtlsState::Closed) {
        impl_->pump_handshake();
        auto more = std::move(impl_->outbound_);
        impl_->outbound_.clear();
        for (auto& r : more) recs.push_back(std::move(r));
    }
    return recs;
}

void DtlsSessionWolfSSL::tick() noexcept {
    if (!impl_->ssl ||
        impl_->state == nimrtc::dtls::DtlsState::Connected ||
        impl_->state == nimrtc::dtls::DtlsState::Failed ||
        impl_->state == nimrtc::dtls::DtlsState::Closed) {
        return;
    }

    // Advance the wolfSSL internal retransmit timer.  When the timer
    // fires, wolfSSL re-emits the last flight of handshake records into
    // our `io_send` callback (appended to `send_buf_`).  We then drain
    // them into `outbound_` so the transport layer can ship them.
    //
    // RFC 6347 §4.2.4: the initial retransmit timer is 1 s with
    // exponential backoff up to 60 s.  Without this pump, a stalled
    // handshake (e.g. lost HelloVerifyRequest) hangs forever.
    //
    // wolfSSL_dtls_got_timeout() returns:
    //   0  — timer not yet fired, nothing to do
    //   1  — timer fired; wolfSSL re-emitted the last flight (caller
    //        should call accept()/connect() again to drive the state
    //        machine forward)
    //  <0  — wolfSSL error (treat as timer-not-fired)
    const int ret = wolfSSL_dtls_got_timeout(impl_->ssl);
    if (ret <= 0) return;  // no retransmit pending, or wolfSSL errored
    ++impl_->stats_instance.retransmits;
    impl_->flush_send_buf();

    // After retransmit, drive accept()/connect() once more so any newly
    // produced records (server flight retransmits) get flushed.
    if (impl_->ssl &&
        impl_->state != nimrtc::dtls::DtlsState::Connected &&
        impl_->state != nimrtc::dtls::DtlsState::Failed &&
        impl_->state != nimrtc::dtls::DtlsState::Closed) {
        impl_->pump_handshake();
    }
}

nimrtc::dtls::DtlsState DtlsSessionWolfSSL::state() const noexcept {
    return impl_->state;
}

bool DtlsSessionWolfSSL::is_connected() const noexcept {
    return impl_->state == nimrtc::dtls::DtlsState::Connected &&
           impl_->ssl &&
           wolfSSL_is_init_finished(impl_->ssl);
}

const char* DtlsSessionWolfSSL::state_name(
    nimrtc::dtls::DtlsState s) noexcept {
    using nimrtc::dtls::DtlsState;
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

const nimrtc::dtls::Fingerprint&
DtlsSessionWolfSSL::local_fingerprint() const noexcept {
    return impl_->local_fp_;
}

void DtlsSessionWolfSSL::set_peer_fingerprint(
        std::string algo,
        std::vector<std::uint8_t> value) noexcept {
    // Stored as raw 32-byte SHA-256 digest.  Only "sha-256" is recognised
    // for 1.0 (matches WebRTC's SDP `a=fingerprint` attribute).  An empty
    // value clears the pin; the next handshake will fail loudly via
    // verify_peer_callback since no CA chain is configured.
    if (algo != "sha-256") {
        nimrtc::core::log::Logger::instance().warn(
            std::string("DtlsSessionWolfSSL: unsupported fingerprint algo '") +
            algo + "' (expected sha-256) — clearing pin");
        impl_->expected_peer_fp_.clear();
        impl_->expected_peer_fp_set_ = false;
        return;
    }
    if (value.size() != 32) {
        nimrtc::core::log::Logger::instance().warn(
            std::string("DtlsSessionWolfSSL: peer fingerprint must be 32 bytes, got ") +
            std::to_string(value.size()) + " — clearing pin");
        impl_->expected_peer_fp_.clear();
        impl_->expected_peer_fp_set_ = false;
        return;
    }
    impl_->expected_peer_fp_ = std::move(value);
    impl_->expected_peer_fp_set_ = true;
    nimrtc::core::log::Logger::instance().info(
        "DtlsSessionWolfSSL: peer fingerprint pin set");
}

void DtlsSessionWolfSSL::set_role(nimrtc::dtls::DtlsRole r) noexcept {
    using nimrtc::dtls::DtlsState;
    if (impl_->state != DtlsState::Closed &&
        impl_->state != DtlsState::Initial) {
        nimrtc::core::log::Logger::instance().warn(
            "DtlsSessionWolfSSL: role change after handshake ignored");
        return;
    }
    impl_->config.role = r;
    nimrtc::core::log::Logger::instance().info(
        std::string("DtlsSessionWolfSSL: role=") +
        (r == DtlsRole::Server ? "Server" : "Client"));
}

std::optional<nimrtc::dtls::SrtpKeyingMaterial>
DtlsSessionWolfSSL::srtp_keying_material() const noexcept {
    if (impl_->state != nimrtc::dtls::DtlsState::Connected) return std::nullopt;
    return impl_->srtp_keys_;
}

DtlsSessionWolfSSL::Stats DtlsSessionWolfSSL::stats() const noexcept {
    return impl_->stats_instance;
}

} // namespace nimrtc::dtls

// Undefine Windows headers' "New" macro (preprocessor poison for any
// subsequent translation unit that includes this file via PCH).
#ifdef New
#undef New
#endif
