/**
 * turn_tls.cpp — the wolfSSL side of the TURN-over-TLS transport seam.
 *
 * See turn_tls.hpp for the contract.  The three things this file has to
 * get right:
 *
 *   1. The socket is non-blocking.  wolfSSL reports a would-block as
 *      WOLFSSL_ERROR_WANT_READ / WANT_WRITE, which is translated into
 *      the seam's "0 = would block" return value.
 *
 *   2. The handshake is incremental.  wolfSSL_connect() returns
 *      WANT_READ/WANT_WRITE until it completes; the seam's handshake()
 *      returns 0 in that case so libjuice can retry.
 *
 *   3. The socket is owned by the session once it is wrapped, and is
 *      closed exactly once.
 */

#include "turn_tls.hpp"

#include <nimrtc/core/log.hpp>

#ifdef _WIN32
#define _WIN32_WINNT 0x0601
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
#include <wolfssl/error-ssl.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

namespace nimrtc::ice {

namespace {

/// The socket type differs between Windows and POSIX.
#ifdef _WIN32
using RawSocket = SOCKET;
constexpr RawSocket kInvalidSocket = INVALID_SOCKET;
#else
using RawSocket = int;
constexpr RawSocket kInvalidSocket = -1;
#endif

void close_raw_socket(RawSocket sock) {
    if (sock == kInvalidSocket) return;
#ifdef _WIN32
    ::closesocket(sock);
#else
    ::close(sock);
#endif
}

} // namespace

bool TurnTlsClient::reference_is_ip_literal(const std::string& name) {
    if (name.empty()) return false;

    // An IPv6 literal may be bracketed ("[::1]").
    std::string s = name;
    if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
        s = s.substr(1, s.size() - 2);
    }

    struct in_addr v4 {};
    if (::inet_pton(AF_INET, s.c_str(), &v4) == 1) return true;

    struct in6_addr v6 {};
    if (::inet_pton(AF_INET6, s.c_str(), &v6) == 1) return true;

    return false;
}

/// The per-connection state behind the `session` handle.
struct TurnTlsClient::Session {
    WOLFSSL* ssl = nullptr;
    RawSocket sock = kInvalidSocket;

    /// The name the certificate is verified against.
    std::string server_name;
};

TurnTlsClient::TurnTlsClient(Config config) : config_(std::move(config)) {
    WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfTLSv1_2_client_method());
    if (!ctx) {
        last_error_ = "wolfSSL_CTX_new failed";
        NIMRTC_LOG_ERROR("TURN/TLS: " << last_error_);
        return;
    }

    // wolfSSL defaults to TLS 1.2 here because the method is pinned to
    // v1_2.  RFC 6062 does not require a particular version, but TURN
    // servers in the field overwhelmingly speak TLS 1.2, and coturn's
    // --no-tlsv1 / --no-tlsv1_1 configuration matches.

    if (config_.verify_peer) {
        if (!config_.ca_file.empty()) {
            if (wolfSSL_CTX_load_verify_locations(ctx, config_.ca_file.c_str(), nullptr) !=
                WOLFSSL_SUCCESS) {
                last_error_ = "failed to load the CA file " + config_.ca_file;
                NIMRTC_LOG_ERROR("TURN/TLS: " << last_error_);
                wolfSSL_CTX_free(ctx);
                return;
            }
        }

        wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_PEER, nullptr);
    } else {
        // Explicitly insecure, for local experiments only.  The log
        // line is not optional: a silently unverified TLS connection is
        // exactly the kind of defect this transport exists to avoid.
        NIMRTC_LOG_WARN(
            "TURN/TLS: certificate verification is DISABLED - the TURN credentials "
            "can be intercepted");
        wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_NONE, nullptr);
    }

    if (!config_.client_cert_file.empty()) {
        if (wolfSSL_CTX_use_certificate_file(ctx, config_.client_cert_file.c_str(),
                                             WOLFSSL_FILETYPE_PEM) != WOLFSSL_SUCCESS) {
            last_error_ = "failed to load the client certificate " + config_.client_cert_file;
            NIMRTC_LOG_ERROR("TURN/TLS: " << last_error_);
            wolfSSL_CTX_free(ctx);
            return;
        }
    }

    if (!config_.client_key_file.empty()) {
        if (wolfSSL_CTX_use_PrivateKey_file(ctx, config_.client_key_file.c_str(),
                                            WOLFSSL_FILETYPE_PEM) != WOLFSSL_SUCCESS) {
            last_error_ = "failed to load the client key " + config_.client_key_file;
            NIMRTC_LOG_ERROR("TURN/TLS: " << last_error_);
            wolfSSL_CTX_free(ctx);
            return;
        }
    }

    ctx_ = ctx;
    usable_ = true;

    ops_.ctx = this;
    ops_.connect = &TurnTlsClient::connect_impl;
    ops_.handshake = &TurnTlsClient::handshake_impl;
    ops_.read = &TurnTlsClient::read_impl;
    ops_.write = &TurnTlsClient::write_impl;
    ops_.close = &TurnTlsClient::close_impl;
}

TurnTlsClient::~TurnTlsClient() {
    if (ctx_) {
        wolfSSL_CTX_free(static_cast<WOLFSSL_CTX*>(ctx_));
        ctx_ = nullptr;
    }
}

void* TurnTlsClient::connect_impl(void* ctx, int socket_fd, const char* host, std::uint16_t port,
                                  const char* server_name) {
    auto* self = static_cast<TurnTlsClient*>(ctx);
    if (!self || !self->ctx_) return nullptr;

    // The seam's contract: "the implementation takes ownership of the
    // socket: on success the session owns the socket, and on failure the
    // implementation must close it."  Every exit path below therefore
    // closes the socket exactly once, and the guard makes that hard to
    // get wrong.
    struct SocketGuard {
        RawSocket sock;
        ~SocketGuard() { close_raw_socket(sock); }
    } socket_guard{static_cast<RawSocket>(socket_fd)};

    auto* ssl = wolfSSL_new(static_cast<WOLFSSL_CTX*>(self->ctx_));
    if (!ssl) {
        self->last_error_ = "wolfSSL_new failed";
        NIMRTC_LOG_ERROR("TURN/TLS: " << self->last_error_);
        return nullptr;
    }

    auto* session = new (std::nothrow) Session();
    if (!session) {
        wolfSSL_free(ssl);
        return nullptr;
    }

    // The session owns the socket from here on.
    session->sock = socket_guard.sock;

    if (wolfSSL_set_fd(ssl, static_cast<int>(socket_fd)) != WOLFSSL_SUCCESS) {
        self->last_error_ = "wolfSSL_set_fd failed";
        NIMRTC_LOG_ERROR("TURN/TLS: " << self->last_error_);
        delete session;
        wolfSSL_free(ssl);
        return nullptr;
    }

    // The name the certificate is checked against.  An explicit server
    // name wins; otherwise the host that was resolved is used, which is
    // what a TURN client configured with a host name expects.
    const char* name = server_name && *server_name ? server_name : host;
    session->server_name = name ? name : "";

    if (self->config_.check_hostname && !session->server_name.empty()) {
        // The certificate may carry the name in two different SAN forms:
        // a `dNSName` for a host name, and an `iPAddress` for an IP
        // literal.  RFC 6125 §6.4.4 says the IP form must be used when
        // the reference identity is an IP address -- and a client that
        // points at 127.0.0.1 is *not* allowed to fall back to matching
        // the `dNSName` "localhost", because that would let a
        // certificate for a different name satisfy an IP reference.
        if (reference_is_ip_literal(session->server_name)) {
            // The IP literal is passed unbracketed: wolfSSL compares the
            // ASCII form against the `iPAddress` SAN bytes, and "[::1]"
            // is not the form the certificate carries.
            std::string ip = session->server_name;
            if (ip.size() >= 2 && ip.front() == '[' && ip.back() == ']') {
                ip = ip.substr(1, ip.size() - 2);
            }
            if (wolfSSL_X509_VERIFY_PARAM_set1_ip_asc(wolfSSL_get0_param(ssl), ip.c_str()) !=
                WOLFSSL_SUCCESS) {
                self->last_error_ = "failed to set the peer IP " + session->server_name;
                NIMRTC_LOG_ERROR("TURN/TLS: " << self->last_error_);
                delete session;
                wolfSSL_free(ssl);
                return nullptr;
            }
        } else {
            // wolfSSL_set1_host checks the certificate's SAN dNSName entries
            // against the given name.  It is the TLS 1.3 / wolfSSL-native
            // spelling of X509_VERIFY_PARAM_set1_host.
            if (wolfSSL_set1_host(ssl, session->server_name.c_str()) != WOLFSSL_SUCCESS) {
                self->last_error_ = "failed to set the peer name " + session->server_name;
                NIMRTC_LOG_ERROR("TURN/TLS: " << self->last_error_);
                delete session;
                wolfSSL_free(ssl);
                return nullptr;
            }
        }
    }

    // SNI: the middlebox may only route the connection if the SNI
    // matches what it is configured for.
    if (!session->server_name.empty()) {
        if (wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, session->server_name.c_str(),
                            static_cast<word16>(session->server_name.size())) !=
            WOLFSSL_SUCCESS) {
            self->last_error_ = "failed to set SNI " + session->server_name;
            NIMRTC_LOG_ERROR("TURN/TLS: " << self->last_error_);
            delete session;
            wolfSSL_free(ssl);
            return nullptr;
        }
    }

    NIMRTC_LOG_INFO("TURN/TLS: session created for " << session->server_name << ":" << port);

    session->ssl = ssl;

    // Ownership moves to the session; the guard must not close it.
    socket_guard.sock = kInvalidSocket;
    return session;
}

int TurnTlsClient::handshake_impl(void* handle) {
    auto* session = static_cast<Session*>(handle);
    if (!session || !session->ssl) return -1;

    const int ret = wolfSSL_connect(session->ssl);
    if (ret == WOLFSSL_SUCCESS) {
        NIMRTC_LOG_INFO("TURN/TLS: handshake complete, cipher="
                          << wolfSSL_get_cipher_name(session->ssl));
        return 1;
    }

    const int err = wolfSSL_get_error(session->ssl, ret);
    if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) {
        // Not an error: the handshake needs another round trip.
        return 0;
    }

    char buf[WOLFSSL_MAX_ERROR_SZ];
    wolfSSL_ERR_error_string(err, buf);
    NIMRTC_LOG_ERROR("TURN/TLS: handshake failed: " << buf << " (" << err << ")");
    return -1;
}

int TurnTlsClient::read_impl(void* handle, char* buf, std::size_t size) {
    auto* session = static_cast<Session*>(handle);
    if (!session || !session->ssl || size > static_cast<std::size_t>(INT32_MAX)) return -1;

    const int ret = wolfSSL_read(session->ssl, buf, static_cast<int>(size));
    if (ret > 0) return ret;

    const int err = wolfSSL_get_error(session->ssl, ret);
    if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) {
        // The seam's "would block" value.
        return 0;
    }

    if (err == WOLFSSL_ERROR_ZERO_RETURN || err == WOLFSSL_ERROR_SYSCALL) {
        // The peer closed the connection.  This is a failure, not a
        // would-block: the seam's 0 means "retry later", and libjuice's
        // reader thread would otherwise poll a dead connection forever.
        NIMRTC_LOG_INFO("TURN/TLS: the peer closed the connection");
        return -1;
    }

    char errbuf[WOLFSSL_MAX_ERROR_SZ];
    wolfSSL_ERR_error_string(err, errbuf);
    NIMRTC_LOG_ERROR("TURN/TLS: read failed: " << errbuf << " (" << err << ")");
    return -1;
}

int TurnTlsClient::write_impl(void* handle, const char* buf, std::size_t size) {
    auto* session = static_cast<Session*>(handle);
    if (!session || !session->ssl || size > static_cast<std::size_t>(INT32_MAX)) return -1;

    const int ret = wolfSSL_write(session->ssl, buf, static_cast<int>(size));
    if (ret > 0) return ret;

    const int err = wolfSSL_get_error(session->ssl, ret);
    if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) {
        return 0;
    }

    char errbuf[WOLFSSL_MAX_ERROR_SZ];
    wolfSSL_ERR_error_string(err, errbuf);
    NIMRTC_LOG_ERROR("TURN/TLS: write failed: " << errbuf << " (" << err << ")");
    return -1;
}

void TurnTlsClient::close_impl(void* handle) {
    auto* session = static_cast<Session*>(handle);
    if (!session) return;

    if (session->ssl) {
        // A best-effort close_notify: the peer may already be gone, and
        // the shutdown must not block.
        wolfSSL_shutdown(session->ssl);
        wolfSSL_free(session->ssl);
    }

    close_raw_socket(session->sock);
    delete session;
}

} // namespace nimrtc::ice
