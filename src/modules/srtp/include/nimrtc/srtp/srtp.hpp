#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/error.hpp>

// =============================================================================
// nimrtc::srtp
// -----------------------------------------------------------------------------
// SRTP (Secure Real-time Transport Protocol) — RFC 3711 implementation.
//
// P1 delivers:
//   - libsrtp2 integration (when vendor library is available)
//   - Session creation with crypto suite configuration
//   - Encrypt/decrypt RTP and RTCP packets
//   - Key derivation and rollover
//
// ## Crypto Suites
//
// Supported profiles (RFC 4568):
//   - AES-CM-128-HMAC-SRTP-80  (SRTP_AES128_CM_SHA1_80)
//   - AES-CM-128-HMAC-SRTP-32  (SRTP_AES128_CM_SHA1_32)
//   - AES-GCM-128-SRTP-64       (SRTP_AES128_GCM_128BIT_AUTH)
//   - AES-GCM-256-SRTP-64       (SRTP_AES256_CM_SHA1_80)
//
// ## Thread Safety
//
// SrtpSession is NOT thread-safe. Caller must serialize encrypt/decrypt calls.
// Multiple sessions (one per SSRC) can exist concurrently.
// =============================================================================
namespace nimrtc::srtp {

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------

// SRTP MKI (Master Key Identifier) length
constexpr std::size_t kMkiLength = 4;

// Maximum ROC (Roll Over Counter) value before wrap
constexpr std::uint32_t kMaxRoc = 0x7FFFFFFF;

// -----------------------------------------------------------------------------
// Types
// -----------------------------------------------------------------------------

// Crypto suite (SRTP protection profile)
enum class CryptoSuite {
    Aes128CmSha1_80,   // AES-128-CM, HMAC-SHA1-80 auth (default)
    Aes128CmSha1_32,   // AES-128-CM, HMAC-SHA1-32 auth
    Aes128Gcm_128,     // AES-128-GCM, no auth tag (AEAD)
    Aes256CmSha1_80,   // AES-256-CM, HMAC-SHA1-80 auth
};

// Convert suite to libsrtp profile id
std::uint32_t to_libsrtp_profile(CryptoSuite suite);

// -----------------------------------------------------------------------------
// Keying Material
// -----------------------------------------------------------------------------

struct KeyingMaterial {
    std::vector<std::uint8_t> master_salt;   // 14 bytes for AES-CM
    std::vector<std::uint8_t> master_key;    // 16 or 32 bytes
    std::vector<std::uint8_t> mki;            // optional MKI
    std::vector<std::uint8_t> auth_key;       // for HMAC suites

    bool is_valid() const {
        return !master_key.empty() &&
               !master_salt.empty() &&
               master_key.size() >= 16;
    }
};

// -----------------------------------------------------------------------------
// Session Config
// -----------------------------------------------------------------------------

struct Config {
    // Crypto suite to use
    CryptoSuite suite = CryptoSuite::Aes128CmSha1_80;

    // Session keys (will be derived from master key)
    std::vector<std::uint8_t> session_key_rtp;
    std::vector<std::uint8_t> session_salt_rtp;
    std::vector<std::uint8_t> session_key_rtcp;
    std::vector<std::uint8_t> session_salt_rtcp;

    // Authentication
    bool enable_auth = true;
    std::size_t auth_tag_length = 10;  // bytes (80 bits for SHA1-80)

    // Encryption
    bool enable_encryption = true;

    // ROC (Roll Over Counter) — for processing buffered packets
    std::uint32_t initial_roc = 0;

    // Window size for replay detection (packets)
    std::uint64_t replay_window_size = 64;
};

// -----------------------------------------------------------------------------
// SrtpSession
// -----------------------------------------------------------------------------

class SrtpSession {
public:
    SrtpSession();
    ~SrtpSession();

    SrtpSession(const SrtpSession&)            = delete;
    SrtpSession& operator=(const SrtpSession&) = delete;
    SrtpSession(SrtpSession&&)                 noexcept;
    SrtpSession& operator=(SrtpSession&&)      noexcept;

    // Create session from crypto suite and master key material.
    // Returns error if keys are invalid for the chosen suite.
    // Configured as OUTBOUND by default (sender side).
    core::Result<void> init_from_master_key(const Config& config,
                                            std::span<const std::uint8_t> master_key,
                                            std::span<const std::uint8_t> master_salt);

    /** Same as init_from_master_key but configures the session as INBOUND
     *  (receiver side).  Use this for the peer that consumes SRTP packets. */
    core::Result<void> init_from_master_key_inbound(const Config& config,
                                                    std::span<const std::uint8_t> master_key,
                                                    std::span<const std::uint8_t> master_salt);

    // Create session from pre-derived session keys (used in DTLS-SRTP).
    core::Result<void> init_from_session_keys(const Config& config);

    // Encrypt RTP packet in-place.
    // Returns encrypted packet data, or error on failure.
    // Updates internal ROC on success.
    core::Result<core::ByteSpan>
    protect_rtp(core::ByteSpan header_and_plaintext,
               std::uint32_t ssrc,
               std::uint32_t rtp_timestamp);

    // Decrypt SRTP packet in-place.
    // Returns decrypted payload, or error on failure (e.g. auth failure, replay).
    core::Result<core::ByteSpan>
    unprotect_rtp(core::ByteSpan srtp_packet,
                  std::uint32_t* out_ssrc,
                  std::uint32_t* out_timestamp);

    // Encrypt RTCP packet in-place.
    core::Result<core::ByteSpan>
    protect_rtcp(core::ByteSpan rtcp_packet);

    // Decrypt RTCP packet in-place.
    core::Result<core::ByteSpan>
    unprotect_rtcp(core::ByteSpan srtcp_packet);

    // Get current ROC (for sending in RTCP SRESC report).
    std::uint32_t current_roc() const;

    // Get stats
    struct Stats {
        std::uint64_t rtp_packets_encrypted = 0;
        std::uint64_t rtp_packets_decrypted = 0;
        std::uint64_t rtcp_packets_encrypted = 0;
        std::uint64_t rtcp_packets_decrypted = 0;
        std::uint64_t decryption_failures = 0;
        std::uint64_t replay_attacks_dropped = 0;
    };
    Stats stats() const;

    // Reset session state
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// -----------------------------------------------------------------------------
// SrtpContext
// -----------------------------------------------------------------------------

// Top-level SRTP context managing multiple SSRC sessions.
// Used by the transport layer to handle multiple streams.
class SrtpContext {
public:
    SrtpContext();
    ~SrtpContext();

    // Derive keys for the REMOTE participant (DTLS-SRTP key derivation).
    // Stores the peer's master key/salt for INBOUND session creation.
    void derive_keys_for_remote(std::span<const std::uint8_t> srtp_master_key,
                               std::span<const std::uint8_t> srtp_master_salt,
                               CryptoSuite suite);

    // Derive keys for the LOCAL side — the server master key/salt from the
    // DTLS-SRTP handshake is NimRTC's OWN key for OUTBOUND SRTP.
    // Must be called IN ADDITION TO derive_keys_for_remote to fully install
    // a bidirectional SRTP context.
    void derive_keys_for_local(std::span<const std::uint8_t> srtp_master_key,
                               std::span<const std::uint8_t> srtp_master_salt,
                               CryptoSuite suite);

    // Get or create a session for given SSRC and direction.
    // direction=true  → OUTBOUND: uses our own (local) master key.
    // direction=false → INBOUND : uses the remote peer's master key.
    // Creates session with ROC=0 if not exists.
    SrtpSession* get_session(std::uint32_t ssrc, bool outgoing);

    // Remove session for given SSRC (e.g., on BYE).
    void remove_session(std::uint32_t ssrc);

    // Get combined stats from all sessions.
    SrtpSession::Stats total_stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nimrtc::srtp
