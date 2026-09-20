/**
 * @file nimrtc/dtls/dtls_types.hpp
 * @brief Common DTLS types — shared between the concrete dtls module
 *        and the PAL Slice 4 seam header.
 *
 * PAL Slice 4 / TPAL-4 (v0.11.0) engine integration requires the
 * `IDtlsSession` interface to expose the engine-facing surface
 * (`open()` / `state()` / `local_fingerprint()` /
 * `srtp_keying_material()` / `feed_inbound()` / `take_outbound()` /
 * `tick()` / `set_role(DtlsRole)` / `set_peer_fingerprint(...)` /
 * `is_connected()`).  These methods reference the concrete types
 * (`DtlsRole`, `DtlsState`, `DtlsAddr`, `DtlsRecord`,
 * `SrtpKeyingMaterial`, `Fingerprint`, `Config`) which historically
 * lived in `nimrtc/dtls/dtls.hpp`.
 *
 * To break the new circular include
 *   dtls.hpp            ←→  dtls_session_iface.hpp
 *   (concrete module)        (PAL Slice 4 seam)
 * both files now include this header instead of each other.  The
 * concrete module owns the `DtlsSession` / `DtlsSessionWolfSSL`
 * classes; the seam owns the `IDtlsSession` /
 * `IDtlsSessionFactory` interface + `Role` enum.
 *
 * No new types are introduced — every declaration here was already in
 * `dtls.hpp`; we just moved them to a header that's safe for both
 * consumers to include.
 *
 * @note P1 — types hoisted as part of PAL Slice 4 (v0.11.0).
 */

#ifndef NIMRTC_DTLS_TYPES_HPP
#define NIMRTC_DTLS_TYPES_HPP

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

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
// Enums
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

} // namespace nimrtc::dtls

#endif // NIMRTC_DTLS_TYPES_HPP