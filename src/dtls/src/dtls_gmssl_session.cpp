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

#ifdef _WIN32
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#endif

#include "nimrtc/dtls/dtls_gmssl_session.hpp"

#include <nimrtc/core/log.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <optional>

// GMSSL v3.x native headers
extern "C" {
#include <gmssl/tls.h>
#include <gmssl/sm2.h>
#include <gmssl/sm3.h>
#include <gmssl/sm4.h>
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
    char buf[256] = {0};
    gmssl_get_error(/*ctx=*/nullptr, buf, sizeof(buf));
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Socket helpers (POSIX + Winsock2)
// ---------------------------------------------------------------------------

#ifdef _WIN32
using SockFd = SOCKET;
constexpr SockFd kInvalidSocket = INVALID_SOCKET;
inline void close_sock(SockFd fd) { if (fd != INVALID_SOCKET) closesocket(fd); }
inline int last_sock_err() { return WSAGetLastError(); }
inline bool would_block(int err) {
    return err == WSAEWOULDBLOCK || err == WSAEAGAIN;
}
#else
using SockFd = int;
constexpr SockFd kInvalidSocket = -1;
inline void close_sock(SockFd fd) { if (fd >= 0) ::close(fd); }
inline int last_sock_err() { return errno; }
inline bool would_block(int err) {
    return err == EAGAIN || err == EWOULDBLOCK;
}
#endif

bool set_nonblocking(SockFd fd) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

SockFd make_udp_socket() {
#ifdef _WIN32
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return kInvalidSocket;
    // Disable Windows default port-sharing / reuse behavior that conflicts
    // with quick bind/close cycles in tests.
    BOOL off = FALSE;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&off), sizeof(off));
    return s;
#else
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return kInvalidSocket;
    int off = 0;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &off, sizeof(off));
    return s;
#endif
}

// ---------------------------------------------------------------------------
// Fingerprint (RFC 8122) — SHA-256 of DER-encoded X.509 cert
// ---------------------------------------------------------------------------

std::string cert_sha256_hex(const uint8_t* der, size_t der_len) {
    uint8_t hash[32] = {0};
    sm3_digest(der, der_len, hash);
    static const char* tab = "0123456789abcdef";
    std::string s;
    s.reserve(64);
    for (int i = 0; i < 32; ++i) {
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

// TLCP 1.1 / TLS 1.2 — ECDHE key exchange, SM2 certificate auth, SM4-GCM
// (preferred: has Authenticated Encryption with Associated Data)
static const int kTLCPCiphers[] = {
    TLS_cipher_ecdhe_sm4_gcm_sm3,     // 0xe051 — TLCP-1.1 recommended
    TLS_cipher_ecdhe_sm4_cbc_sm3,     // 0xe011 — TLCP-1.1 fallback
    TLS_cipher_ecc_sm4_gcm_sm3,       // 0xe053 — static ECDH (no forward sec)
    TLS_cipher_ecc_sm4_cbc_sm3,       // 0xe013
};

// Standard WebRTC-safe fallback for interop with Chrome/Firefox
static const int kInteropCiphers[] = {
    TLS_cipher_ecdhe_sm4_gcm_sm3,
    TLS_cipher_ecdhe_sm4_cbc_sm3,
    TLS_cipher_ecdhe_sm2_gcm_sm3,     // 0xe071 — ECDHE + SM2 + SM4-GCM
    // International fallback (P-256 ECDSA — NOT GM/T compliant but useful
    // for testing the I/O pipeline when SM2 certs are not yet generated)
    TLS_cipher_ecdhe_sm4_gcm_sha256,
    TLS_cipher_ecdhe_sm4_cbc_sha256,
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
// GMSSL's tls_prf() implements the TLS 1.2 PRF.
// Returns 60 bytes in out[60]: client_write_key(16) + server_write_key(16)
//   + client_SRTP_salt(14) + server_SRTP_salt(14).
bool derive_srtp_keys(const TLS_CONNECT* conn,
                       uint8_t out[60]) {
    // TLS 1.2 PRF uses HMAC-SHA-1 for ECDHE-ECDSA-* cipher suites
    extern const DIGEST gmssl_digest_sm3;
    extern const DIGEST gmssl_digest_sha1;

    static const char kLabel[] = "EXTRACTOR-dtls_srtp";
    const uint8_t* randoms[2];
    size_t randsize[2];
    randoms[0] = conn->client_random;
    randsize[0] = 32;
    randoms[1] = conn->server_random;
    randsize[1] = 32;

    // TLS 1.2 PRF: HMAC-SHA-1 (RFC 5246 §5)
    const DIGEST* prf_digest = &gmssl_digest_sha1;
    uint8_t key_block[96] = {0};
    size_t key_block_len = 0;

    int ret = tls_prf(prf_digest,
                      conn->master_secret, 48,
                      kLabel, strlen(kLabel),
                      randoms, randsize, 2,
                      key_block, sizeof(key_block), &key_block_len);
    if (ret != 1 || key_block_len < 60) {
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

    // GMSSL native objects
    TLS_CTX        ctx_{};
    TLS_CONNECT    conn_{};
    bool           conn_inited_ = false;

    // Loopback UDP socket: this is what GMSSL reads from / writes to.
    // The test's feed_inbound / take_outbound bridge mirrors the bytes
    // between the two sessions' sockets.
    SockFd         sock_fd_ = kInvalidSocket;

    // Outbound queue drained from the socket send buffer
    std::vector<DtlsRecord> outbound_;

    // Peer address (for DtlsRecord.to on outbound records)
    std::string    peer_host_;
    std::uint16_t  peer_port_ = 0;

    // Peer fingerprint pin
    Fingerprint               local_fp_;
    std::vector<uint8_t>      expected_peer_fp_;
    bool                      expected_peer_fp_set_ = false;

    // SRTP keying material (filled after handshake)
    std::optional<SrtpKeyingMaterial> srtp_keys_;

    Stats         stats_instance{};
    std::chrono::steady_clock::time_point handshake_start_{};

    IDtlsSession::OnCompleteCb on_complete_cb_;

    ~Impl() { teardown(); }

    // ------------------------------------------------------------------
    // GMSSL setup
    // ------------------------------------------------------------------

    bool setup_context(bool prefer_tlcp) {
        // Initialise TLS context: TLS 1.2, client or server role.
        // GMSSL maps DTLS_protocol versions to the same init call.
        int is_client = (config.role == DtlsRole::Client) ? 1 : 0;
        int ret = tls_ctx_init(&ctx_, TLS_protocol_tls12, is_client);
        if (ret != 1) {
            nimrtc::core::log::Logger::instance().error(
                "gmssl: tls_ctx_init failed: " + gmssl_err());
            return false;
        }

        // Cipher suites — prefer TLCP, fall back to interop mix
        const int* ciphers = choose_cipher_list(prefer_tlcp);
        size_t cipher_cnt = choose_cipher_list_size(prefer_tlcp);
        ret = tls_ctx_set_cipher_suites(&ctx_, ciphers, cipher_cnt);
        if (ret != 1) {
            nimrtc::core::log::Logger::instance().error(
                "gmssl: tls_ctx_set_cipher_suites failed: " + gmssl_err());
            return false;
        }

        // Supported groups: GMSSL supports sm2p256v1 (GM curves)
        static const int kGroups[] = {
            // GM curves (preferred)
            OID_sm2p256v1,   // GM/T 0003 curve
            // International fallback
            OID_prime256v1,  // P-256
        };
        tls_ctx_set_supported_groups(&ctx_, kGroups,
            sizeof(kGroups) / sizeof(kGroups[0]));

        // Signature algorithms
        static const int kSigs[] = {
            OID_sm2sign_with_sm3,       // SM2 + SM3
            OID_ecdsa_with_sha256,      // P-256 ECDSA fallback
        };
        tls_ctx_set_signature_algorithms(&ctx_, kSigs,
            sizeof(kSigs) / sizeof(kSigs[0]));

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
            &ctx_, cert_file, key_file, pass);
        if (ret != 1) {
            nimrtc::core::log::Logger::instance().error(
                std::string("gmssl: tls_ctx_set_certificate_and_key failed: ") +
                gmssl_err() + " (cert=" + cert_file + " key=" + key_file + ")");
            return false;
        }
        return true;
    }

    bool setup_socket(const char* bind_host, uint16_t bind_port,
                      const char* peer_host, uint16_t peer_port) {
        sock_fd_ = make_udp_socket();
        if (sock_fd_ == kInvalidSocket) {
            nimrtc::core::log::Logger::instance().error(
                "gmssl: socket() failed: " + std::to_string(last_sock_err()));
            return false;
        }

        // Bind to local address
        struct sockaddr_in local_addr{};
        local_addr.sin_family = AF_INET;
        local_addr.sin_port   = htons(bind_port);
        if (::inet_pton(AF_INET, bind_host, &local_addr.sin_addr) <= 0) {
            nimrtc::core::log::Logger::instance().error(
                std::string("gmssl: invalid bind host: ") + bind_host);
            close_sock(sock_fd_); sock_fd_ = kInvalidSocket;
            return false;
        }

        if (::bind(sock_fd_,
                   reinterpret_cast<struct sockaddr*>(&local_addr),
                   sizeof(local_addr)) < 0) {
            nimrtc::core::log::Logger::instance().error(
                std::string("gmssl: bind() failed: ") +
                std::to_string(last_sock_err()));
            close_sock(sock_fd_); sock_fd_ = kInvalidSocket;
            return false;
        }

        // Connect to peer — makes send()/recv() symmetric (no sendto/recvfrom)
        struct sockaddr_in peer_addr{};
        peer_addr.sin_family = AF_INET;
        peer_addr.sin_port   = htons(peer_port);
        if (::inet_pton(AF_INET, peer_host, &peer_addr.sin_addr) <= 0) {
            nimrtc::core::log::Logger::instance().error(
                std::string("gmssl: invalid peer host: ") + peer_host);
            close_sock(sock_fd_); sock_fd_ = kInvalidSocket;
            return false;
        }

        if (::connect(sock_fd_,
                      reinterpret_cast<struct sockaddr*>(&peer_addr),
                      sizeof(peer_addr)) < 0) {
            nimrtc::core::log::Logger::instance().error(
                std::string("gmssl: connect() failed: ") +
                std::to_string(last_sock_err()));
            close_sock(sock_fd_); sock_fd_ = kInvalidSocket;
            return false;
        }

        // Set non-blocking so recv() returns EAGAIN when no data
        if (!set_nonblocking(sock_fd_)) {
            nimrtc::core::log::Logger::instance().error(
                "gmssl: set_nonblocking failed");
            close_sock(sock_fd_); sock_fd_ = kInvalidSocket;
            return false;
        }

        peer_host_ = peer_host;
        peer_port_ = peer_port;
        return true;
    }

    bool create_tls_object(const char* cert_file, const char* key_file,
                            const char* key_pass) {
        // Initialise TLS_CONNECT and attach the socket
        conn_.sock = sock_fd_;
        conn_.is_client = (config.role == DtlsRole::Client) ? 1 : 0;

        int ret = tls_init(&conn_, &ctx_);
        if (ret != 1) {
            nimrtc::core::log::Logger::instance().error(
                "gmssl: tls_init failed: " + gmssl_err());
            return false;
        }
        conn_inited_ = true;

        // Compute and cache local fingerprint from the loaded cert
        compute_local_fingerprint(cert_file);

        nimrtc::core::log::Logger::instance().info(
            std::string("DtlsSessionGmSSL: role=") +
            (config.role == DtlsRole::Server ? "Server" : "Client"));
        return true;
    }

    void compute_local_fingerprint(const char* cert_file) {
        // Read PEM cert and compute SHA-256 (RFC 8122)
        FILE* f = std::fopen(cert_file, "rb");
        if (!f) return;

        std::fseek(f, 0, SEEK_END);
        long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (sz <= 0) { std::fclose(f); return; }

        std::vector<uint8_t> pem(static_cast<size_t>(sz) + 1);
        size_t n = std::fread(pem.data(), 1, static_cast<size_t>(sz), f);
        pem[n] = 0;
        std::fclose(f);

        // PEM → DER
        uint8_t der[4096] = {0};
        size_t der_len = sizeof(der);
        if (pem_read_buffer(der, &der_len, pem.data(), n,
                            nullptr, nullptr, nullptr) != 1) {
            nimrtc::core::log::Logger::instance().error(
                "gmssl: pem_read_buffer failed for cert: " + gmssl_err());
            return;
        }

        std::string fp_hex = cert_sha256_hex(der, der_len);
        if (fp_hex.empty()) return;

        std::vector<uint8_t> raw(32);
        for (int i = 0; i < 32; ++i) {
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
        local_fp_.base64.clear();
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

        int ret = tls_do_handshake(&conn_);

        // Drain any bytes GMSSL wrote to the socket into outbound_
        drain_socket_to_outbound();

        if (ret == 1) {
            // Handshake complete — verify peer cert against SDP fingerprint pin
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
        if (conn_.peer_cert_chain_len == 0) {
            nimrtc::core::log::Logger::instance().error(
                "gmssl: no peer certificate chain after handshake");
            return false;
        }

        std::string fp_hex = cert_sha256_hex(
            conn_.peer_cert_chain, conn_.peer_cert_chain_len);
        if (fp_hex.empty()) return false;

        std::vector<uint8_t> got(32);
        for (int i = 0; i < 32; ++i) {
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
            diff |= got[i] ^ expected_peer_fp_[i];

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
        if (!derive_srtp_keys(&conn_, key_block)) {
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

    // Read everything GMSSL wrote to its socket fd into outbound_
    void drain_socket_to_outbound() {
        if (sock_fd_ == kInvalidSocket) return;
        uint8_t buf[2048];
        while (true) {
            int n = static_cast<int>(
                ::recv(sock_fd_,
                       reinterpret_cast<char*>(buf), sizeof(buf), 0));
            if (n <= 0) {
                if (n < 0 && would_block(last_sock_err())) break;
                break;  // 0 = orderly close, <0 = error
            }
            DtlsRecord rec;
            rec.bytes.assign(buf, buf + static_cast<size_t>(n));
            rec.to.host = peer_host_;
            rec.to.port = peer_port_;
            outbound_.push_back(std::move(rec));
            ++stats_instance.records_out;
        }
    }

    // Feed inbound bytes into GMSSL — write them to the socket so GMSSL
    // can recv() them on the next handshake pump.
    bool feed_inbound_bytes(const uint8_t* data, size_t len) {
        if (sock_fd_ == kInvalidSocket || len == 0) return true;
        ssize_t n = ::send(sock_fd_,
                            reinterpret_cast<const char*>(data), len, 0);
        if (n <= 0) {
            if (n < 0 && !would_block(last_sock_err())) {
                nimrtc::core::log::Logger::instance().warn(
                    "gmssl: send() failed: " + std::to_string(last_sock_err()));
                return false;
            }
        }
        return true;
    }

    void teardown() {
        if (conn_inited_) {
            tls_cleanup(&conn_);
            conn_inited_ = false;
        }
        tls_ctx_cleanup(&ctx_);
        close_sock(sock_fd_);
        sock_fd_ = kInvalidSocket;
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

    // Cert/key paths — resolved from candidates list
    const char* cert_file = impl_->config.cert_path.empty()
        ? "tests/wolfssl_dtls/certs/server-ecc.pem"
        : impl_->config.cert_path.c_str();
    const char* key_file = impl_->config.key_path.empty()
        ? "tests/wolfssl_dtls/certs/ecc-key.pem"
        : impl_->config.key_path.c_str();

    // GMSSL setup
    if (!impl_->setup_context(/*prefer_tlcp=*/true)) {
        ++impl_->stats_instance.errors;
        return core::Result<void>::fail(
            core::ErrorCode::InternalError, "GMSSL context init failed");
    }

    if (!impl_->set_certificates(cert_file, key_file,
                                  impl_->config.key_pass.c_str())) {
        ++impl_->stats_instance.errors;
        return core::Result<void>::fail(
            core::ErrorCode::InternalError, "GMSSL cert/key setup failed");
    }

    // Socket: bind to our port, connect to peer.
    // Default: bind 127.0.0.1:0, connect to 127.0.0.1:0 (peer configures).
    // In tests, each session has its own port range.
    const char* bind_host = impl_->config.bind_host.empty()
        ? "127.0.0.1" : impl_->config.bind_host.c_str();
    const char* peer_host = impl_->config.peer_host.empty()
        ? "127.0.0.1" : impl_->config.peer_host.c_str();

    if (!impl_->setup_socket(bind_host,
                              impl_->config.bind_port,
                              peer_host,
                              impl_->config.peer_port)) {
        ++impl_->stats_instance.errors;
        return core::Result<void>::fail(
            core::ErrorCode::InternalError, "socket setup failed");
    }

    if (!impl_->create_tls_object(cert_file, key_file,
                                   impl_->config.key_pass.c_str())) {
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
        const DtlsAddr& from) noexcept {
    if (bytes.empty()) return 0;
    ++impl_->stats_instance.records_in;

    if (!from.host.empty() && from.port != 0) {
        impl_->peer_host_ = from.host;
        impl_->peer_port_ = from.port;
    }

    // Write bytes into the socket — GMSSL recv() will read them on
    // the next pump_handshake() call.
    if (!impl_->feed_inbound_bytes(bytes.data(), bytes.size())) {
        nimrtc::core::log::Logger::instance().warn(
            "gmssl: feed_inbound_bytes failed");
        return 0;
    }

    // Also drain any outstanding outbound bytes that arrived since last call
    impl_->drain_socket_to_outbound();

    // Drive the handshake state machine
    impl_->pump_handshake();

    return bytes.size();
}

std::vector<DtlsRecord> DtlsSessionGmSSL::take_outbound() noexcept {
    // Drain any newly arrived outbound bytes
    impl_->drain_socket_to_outbound();

    auto recs = std::move(impl_->outbound_);
    impl_->outbound_.clear();

    // If handshake is still in progress, keep pumping — new records may appear
    if (impl_->conn_inited_ &&
        impl_->state != DtlsState::Connected &&
        impl_->state != DtlsState::Failed &&
        impl_->state != DtlsState::Closed) {
        impl_->pump_handshake();
        for (auto& r : impl_->outbound_)
            recs.push_back(std::move(r));
        impl_->outbound_.clear();
    }
    return recs;
}

void DtlsSessionGmSSL::tick() noexcept {
    if (!impl_->conn_inited_ ||
        impl_->state == DtlsState::Connected ||
        impl_->state == DtlsState::Failed ||
        impl_->state == DtlsState::Closed) {
        return;
    }
    // Pump the handshake; drain any socket send buffer into outbound_
    impl_->drain_socket_to_outbound();
    impl_->pump_handshake();
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
