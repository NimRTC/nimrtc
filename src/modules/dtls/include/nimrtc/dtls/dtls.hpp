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

namespace nimrtc::dtls {

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------

constexpr std::size_t kFingerprintHashLen = 32;        // SHA-256 = 32 bytes
constexpr std::size_t kSrtpKeyLen         = 16;        // AES-CM-128
constexpr std::size_t kSrtpSaltLen        = 14;
constexpr std::size_t kSrtpMasterKeyLen   = 16;
constexpr std::size_t kSrtpMasterSaltLen  = 14;

// DTLS record types (RFC 6347 §4.1)
constexpr std::uint8_t kDtlsHandshakeContentType = 22;
constexpr std::uint8_t kDtlsAlertContentType     = 21;
constexpr std::uint8_t kDtlsChangeCipherSpec     = 20;
constexpr std::uint8_t kDtlsAppData              = 23;

// DTLS handshake message types (subset we care about)
constexpr std::uint8_t kHsClientHello  = 1;
constexpr std::uint8_t kHsServerHello  = 2;
constexpr std::uint8_t kHsHelloVerify  = 3;
constexpr std::uint8_t kHsCertificate  = 11;
constexpr std::uint8_t kHsServerKeyExchange = 12;
constexpr std::uint8_t kHsCertificateRequest = 13;
constexpr std::uint8_t kHsServerHelloDone = 14;
constexpr std::uint8_t kHsCertificateVerify = 15;
constexpr std::uint8_t kHsClientKeyExchange = 16;
constexpr std::uint8_t kHsFinished = 20;

// -----------------------------------------------------------------------------
// Types
// -----------------------------------------------------------------------------

enum class DtlsRole : std::uint8_t {
    Server,    // answering offer
    Client,    // initiating offer
};

enum class DtlsState : std::uint8_t {
    Closed,
    Initial,
    HelloVerify,
    HelloSent,
    HelloReceived,
    CertificateReceived,
    KeyExchange,
    ChangeCipherSpec,
    Finished,
    Connected,
    Failed,
};

enum class SrtpProfile : std::uint16_t {
    Aes128CmSha1_80 = 0x0001,
    Aes128CmSha1_32 = 0x0002,
    Aes128Gcm       = 0x0007,
    Aes256CmSha1_80 = 0x0003,
};

// -----------------------------------------------------------------------------
// Endpoint address (mirrors ICE-selected pair)
// -----------------------------------------------------------------------------

struct DtlsAddr {
    std::string host;       // IPv4 dotted-quad
    std::uint16_t port = 0;
};

// -----------------------------------------------------------------------------
// DTLS record (one application-visible message)
// -----------------------------------------------------------------------------

struct DtlsRecord {
    std::vector<std::uint8_t> bytes;
    DtlsAddr                  to;       // empty = use session's peer
};

// -----------------------------------------------------------------------------
// SRTP keying material (RFC 5764 §4.2)
// -----------------------------------------------------------------------------

struct SrtpKeyingMaterial {
    SrtpProfile               profile = SrtpProfile::Aes128CmSha1_80;

    std::array<std::uint8_t, kSrtpMasterKeyLen>  client_master_key{};
    std::array<std::uint8_t, kSrtpMasterSaltLen> client_master_salt{};
    std::array<std::uint8_t, kSrtpMasterKeyLen>  server_master_key{};
    std::array<std::uint8_t, kSrtpMasterSaltLen> server_master_salt{};

    /** Number of times each direction may rollover without re-keying
     *  (informational; libsrtp doesn't need this). */
    std::uint32_t            lifetime = 0;
};

// -----------------------------------------------------------------------------
// Local fingerprint (advertised in SDP)
// -----------------------------------------------------------------------------

struct Fingerprint {
    std::string algorithm;     // "sha-256"
    std::vector<std::uint8_t> bytes;     // raw hash
    std::string base64;        // base64-encoded (kept for legacy/debug; NOT for SDP)
    std::string hex_colon;     // colon-separated UPPER hex per RFC 8122 (SDP form)
};

// -----------------------------------------------------------------------------
// Config
// -----------------------------------------------------------------------------

struct Config {
    DtlsRole role = DtlsRole::Server;

    /** SDP peer fingerprint (must match the certificate the peer presents). */
    std::string peer_fingerprint_algo  = "sha-256";
    std::vector<std::uint8_t> peer_fingerprint_value;  // raw bytes

    /** Preferred SRTP protection profile.  Default = AES-128-CM-SHA1-80. */
    SrtpProfile srtp_profile = SrtpProfile::Aes128CmSha1_80;

    /** How long to retry HelloVerifyRequest (seconds).  0 = no retry. */
    std::uint32_t hello_verify_timeout_s = 3;

    /** MTU for outbound records (typical: 1200 to fit IPv6 path). */
    std::uint16_t mtu = 1200;

    /** Debug: enable verbose logging. */
    bool verbose = false;
};

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
class DtlsSession {
public:
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
