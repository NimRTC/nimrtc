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
    std::vector<std::uint8_t> work_buf_;   // protect output
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
