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

#define _WIN32_WINNT 0x0601

#include "nimrtc/dtls/dtls_wolfssl_session.hpp"

#include <nimrtc/core/log.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <functional>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <wolfssl/wolfcrypt/asn.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")

// Undo the 'New' macro that <windows.h> defines (if present; no-op otherwise).
#ifdef New
#undef New
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

std::string fingerprint_sha256_from_der(const std::uint8_t* der,
                                        std::size_t der_len) {
    std::uint8_t hash[32];
    wc_Sha256 sha;
    wc_InitSha256(&sha);
    wc_Sha256Update(&sha, der, static_cast<word32>(der_len));
    wc_Sha256Final(&sha, hash);
    return hex_colon(hash, 32);
}

const char* srtp_profile_name(SrtpProfile p) {
    switch (p) {
        case SrtpProfile::Aes128CmSha1_80: return "SRTP_AES128_CM_SHA1_80";
        case SrtpProfile::Aes128CmSha1_32: return "SRTP_AES128_CM_SHA1_32";
        case SrtpProfile::Aes128Gcm:        return "SRTP_AES128_GCM_80";
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
        std::memcpy(buf, self->recv_buf_.data(), n);
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

        int rc = wolfSSL_CTX_set_cipher_list(
            ctx, "ECDHE-ECDSA-AES128-GCM-SHA256:"
                 "ECDHE-ECDSA-AES128-SHA256");
        if (rc != WOLFSSL_SUCCESS) {
            nimrtc::core::log::Logger::instance().error(
                "wolfSSL: failed to set cipher list");
            return false;
        }

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

        std::string srtp_profiles = std::string(srtp_profile_name(config.srtp_profile))
                                  + ":" + srtp_profile_name(config.srtp_profile);
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

        wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_NONE, nullptr);
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

        local_fp_.algorithm = "sha-256";
        local_fp_.bytes.assign(der->buffer,
                               der->buffer + der->length);
        local_fp_.hex_colon =
            fingerprint_sha256_from_der(der->buffer,
                                        static_cast<std::size_t>(der->length));

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

        const char* reason = wolfSSL_ERR_reason_error_string(err);
        nimrtc::core::log::Logger::instance().error(
            std::string("wolfSSL handshake failed: ") +
            std::to_string(err) + " " + (reason ? reason : ""));
        state = nimrtc::dtls::DtlsState::Failed;
        ++stats_instance.errors;
        flush_send_buf();
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
    // we drained the previous batch.
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
        std::string /*algo*/,
        std::vector<std::uint8_t> /*value*/) noexcept {
    // wolfSSL has WOLFSSL_VERIFY_NONE — no trust chain.
    // WebRTC pins at the SDP level (engine verifies a=fingerprint matches
    // the signaling-channel record before passing to DTLS).
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
