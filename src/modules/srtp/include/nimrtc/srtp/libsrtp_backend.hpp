/**
 * @file nimrtc/srtp/libsrtp_backend.hpp
 * @brief Libsrtp-backed ISrtpSessionBackend adapter (v1.1 X2; wired in
 *        v1.2.0).
 *
 * The built-in crypto backend for the SRTP family.  It owns one libsrtp
 * `srtp_t` context and implements the
 * `plugins::ISrtpSessionBackend` seam directly — so the libsrtp-specific
 * code lives *here*, and `srtp::SrtpSession` (the engine-facing API)
 * only speaks the seam.
 *
 * Registered under the id "libsrtp" by
 * `nimrtc::srtp::register_default_plugins()`; the engine selects it
 * through `EngineConfig::srtp_backend_name`.
 *
 * SM4-GCM is out of scope — a v1.2+ national-crypto backend implements
 * the same seam without touching this file or any caller.
 */

#pragma once

#include <nimrtc/core/api_version.hpp>

#include <nimrtc/plugins/srtp_backend.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

// The libsrtp context type (`srtp_t`) is a vendor typedef of a pointer to
// an opaque struct.  The header keeps it as `void*` so it stays free of
// the vendor include; the .cpp casts it to the real type.
namespace nimrtc::srtp {

/**
 * @brief libsrtp-backed seam session (one SRTP stream direction).
 *
 * Owns a single libsrtp crypto context; `init_outbound` / `init_inbound`
 * install the master key with the matching polarity, the protect /
 * unprotect calls delegate directly.  Not thread-safe (same contract as
 * every other SRTP session).
 */
class NIMRTC_API_UNSTABLE LibsrtpSessionBackend
    : public plugins::ISrtpSessionBackend {
public:
    LibsrtpSessionBackend();
    ~LibsrtpSessionBackend() override;

    LibsrtpSessionBackend(const LibsrtpSessionBackend&) = delete;
    LibsrtpSessionBackend& operator=(const LibsrtpSessionBackend&) = delete;

    core::Result<void> init_outbound(std::span<const std::uint8_t> master_key,
                                     std::span<const std::uint8_t> master_salt,
                                     std::uint32_t suite_id) noexcept override;
    core::Result<void> init_inbound(std::span<const std::uint8_t> master_key,
                                    std::span<const std::uint8_t> master_salt,
                                    std::uint32_t suite_id) noexcept override;

    /** The suite ids this backend understands.
     *
     *  These are the **seam** suite ids (`plugins::SrtpCryptoSuite`),
     *  NOT libsrtp's own `srtp_profile_t` values.  The two numbering
     *  schemes differ:
     *
     *      seam id                              libsrtp profile id
     *      -------------------------------     -----------------------
     *      kAes128CmSha1_80 (0)                srtp_profile_aes128_cm_sha1_80 (2)
     *      kAes128CmSha1_32 (1)                srtp_profile_aes128_cm_sha1_32 (3)
     *      kAes128Gcm_128   (2)                srtp_profile_aead_aes_128_gcm  (10)
     *      kAes256CmSha1_80 (3)                srtp_profile_aes256_cm_sha1_80 (6)
     *
     *  `srtp::to_libsrtp_profile()` returns the *libsrtp* id and is a
     *  public API in its own right — it must NOT be fed to
     *  `init_outbound` / `init_inbound`.  Doing so silently selects the
     *  wrong protection profile (and, for id 1, selects a 32-bit-auth
     *  profile where 80-bit auth was asked for).
     *
     *  @see plugins::SrtpCryptoSuite for the seam-side enum. */

    core::Result<core::ByteSpan> protect_rtp(
        core::ByteSpan header_and_plaintext) noexcept override;
    core::Result<core::ByteSpan> unprotect_rtp(
        core::ByteSpan srtp_packet) noexcept override;
    core::Result<core::ByteSpan> protect_rtcp(
        core::ByteSpan rtcp_packet) noexcept override;
    core::Result<core::ByteSpan> unprotect_rtcp(
        core::ByteSpan srtcp_packet) noexcept override;

    std::uint32_t current_roc() const noexcept override;
    void reset() noexcept override;

private:
    void*                    session_ = nullptr;
    std::uint32_t            suite_id_ = 0;

    /// Scratch buffer for the *protect* direction.
    ///
    /// The buffer is grown to fit and then reused for the next packet,
    /// so the steady-state protect path performs no allocation.  The
    /// buffer is never shrunk back — `reset()` clears it outright.
    std::vector<std::uint8_t> work_buf_;   // protect output

    /// Scratch buffer for the *unprotect* direction — see `work_buf_`.
    ///
    /// Before Phase 3 this was a fresh `std::vector` per packet on the
    /// `SrtpSession::unprotect_rtp()` path (one heap allocation per
    /// inbound RTP packet).  The Phase 2 refactor moved the crypto
    /// behind the `ISrtpSessionBackend` seam, and this member is where
    /// the reuse landed.
    std::vector<std::uint8_t> out_buf_;    // unprotect output
};

/**
 * @brief Factory for the built-in libsrtp backend (id "libsrtp").
 */
class NIMRTC_API_UNSTABLE LibsrtpBackendFactory
    : public plugins::ISrtpBackendFactory {
public:
    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;
    std::unique_ptr<plugins::ISrtpSessionBackend> create() const noexcept override;
};

} // namespace nimrtc::srtp
