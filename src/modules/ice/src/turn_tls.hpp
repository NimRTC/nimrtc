/**
 * turn_tls.hpp — a wolfSSL TLS client for the TURN-over-TLS control
 * connection (RFC 8656 section 12).
 *
 * The vendored libjuice owns the TURN protocol but not a TLS stack: it
 * would have to carry one in the MPL-2.0 vendored tree, which is the
 * wrong place for it.  Instead libjuice exposes a narrow transport seam
 * (see src/third_party/libjuice/patches/0002-turn-tls-transport.patch)
 * and this file supplies the TLS client behind it.
 *
 * The session is driven from libjuice's TURN reader thread:
 *
 *   - `connect()` wraps an already-connected, non-blocking TCP socket.
 *   - `handshake()` advances the handshake one step at a time and
 *     reports WANT_READ / WANT_WRITE back to the caller.
 *   - `read()` / `write()` transfer bytes, returning 0 when the
 *     operation would block.
 *
 * The socket is owned by this session: it is closed by `close()`.
 */

#ifndef NIMRTC_ICE_TURN_TLS_HPP
#define NIMRTC_ICE_TURN_TLS_HPP

#include <juice/juice.h>

#include <cstdint>
#include <string>

namespace nimrtc::ice {

/// The TLS client used for the TURN-over-TLS control connection.
///
/// One instance serves every TURN server the ICE agent talks to: the
/// session it creates carries the per-connection state.
class TurnTlsClient {
public:
    /// The trust anchors used to verify the TURN server certificate.
    struct Config {
        /// Path to a PEM file holding the CA certificates.
        ///
        /// When empty, the system trust store is used (wolfSSL's
        /// default).  A PEM path is the way to trust the local test CA
        /// that examples/turn-server/gen-certs.sh produces.
        std::string ca_file;

        /// Path to a PEM file holding a client certificate, for servers
        /// that require mutual TLS.  Empty means no client certificate.
        std::string client_cert_file;

        /// Path to the PEM file holding the client private key.
        std::string client_key_file;

        /// Whether the server certificate is verified at all.
        ///
        /// This defaults to true and must stay true in production: it
        /// is the only thing standing between the client and a
        /// man-in-the-middle that would harvest the TURN credentials.
        /// It exists for local experiments with self-signed
        /// certificates only.
        bool verify_peer = true;

        /// Whether the server certificate's host name is checked
        /// against the SNI.  Defaults to true.
        bool check_hostname = true;
    };

    explicit TurnTlsClient(Config config);
    ~TurnTlsClient();

    TurnTlsClient(const TurnTlsClient&) = delete;
    TurnTlsClient& operator=(const TurnTlsClient&) = delete;

    /// Whether the client is usable.
    ///
    /// A client without a working wolfSSL context cannot open a
    /// session; the ICE transport then reports TURN over TLS as
    /// unavailable instead of silently downgrading to plain TCP.
    [[nodiscard]] bool usable() const noexcept { return usable_; }

    /// The ops handed to libjuice.
    ///
    /// The returned pointer stays valid for the lifetime of this
    /// client, and libjuice copies the struct by value but keeps the
    /// `ctx` pointer, which points back at this object.
    [[nodiscard]] const juice_turn_tls_ops_t* ops() const noexcept { return &ops_; }

    /// The last error, for diagnostics.
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

    /// Whether @p name is an **IP literal** rather than a host name.
    ///
    /// RFC 6125 §6.4.4 splits the peer-identity check by the shape of
    /// the reference identity: a host name is matched against the
    /// certificate's `dNSName` SAN entries, an IP literal against its
    /// `iPAddress` SAN entries.  The two are **exclusive** — a client
    /// that points at `127.0.0.1` must not fall back to matching the
    /// `dNSName` `localhost`, because that would let a certificate
    /// issued for a different name satisfy an IP reference.
    ///
    /// A bracketed IPv6 literal (`[::1]`) counts as an IP literal; the
    /// brackets are stripped before the address is parsed.
    [[nodiscard]] static bool reference_is_ip_literal(const std::string& name);

private:
    /// The per-connection state behind the `session` handle.
    struct Session;

    Config config_;
    bool usable_ = false;
    std::string last_error_;

    /// The wolfSSL context.  Opaque here to keep the wolfSSL headers
    /// out of this header: the DTLS backend already includes them, and
    /// including them twice in one build is a common source of
    /// redefinition errors.
    void* ctx_ = nullptr;

    juice_turn_tls_ops_t ops_{};

    // The seam callbacks.  They are static so they can be handed to
    // libjuice as plain function pointers.
    static void* connect_impl(void* ctx, int socket_fd, const char* host, std::uint16_t port,
                              const char* server_name);
    static int handshake_impl(void* session);
    static int read_impl(void* session, char* buf, std::size_t size);
    static int write_impl(void* session, const char* buf, std::size_t size);
    static void close_impl(void* session);
};

} // namespace nimrtc::ice

#endif // NIMRTC_ICE_TURN_TLS_HPP
