/**
 * @file nimrtc/srtp/libsrtp_backend.hpp
 * @brief Libsrtp-backed ISrtpSessionBackend adapter (v1.1 X2).
 *
 * Routes the existing libsrtp implementation (`SrtpSession`) through the
 * `plugins::ISrtpSessionBackend` seam. Default behaviour is unchanged:
 * the adapter owns one `SrtpSession` and delegates every call to it.
 * SM4-GCM is out of scope for v1.1; a v1.2+ backend implements the same
 * seam without touching callers.
 */

#pragma once

#include <nimrtc/core/api_version.hpp>

#include <nimrtc/plugins/srtp_backend.hpp>
#include <nimrtc/srtp/srtp.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace nimrtc::srtp {

/**
 * @brief libsrtp-backed seam session (one SRTP stream direction).
 *
 * Owns a single `SrtpSession`; `init_outbound` / `init_inbound` install
 * the master key with the matching polarity, the protect/unprotect calls
 * delegate directly. Not thread-safe (same contract as `SrtpSession`).
 */
class NIMRTC_API_UNSTABLE LibsrtpSessionBackend : public plugins::ISrtpSessionBackend {
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
    SrtpSession session_;
};

/**
 * @brief Factory for the built-in libsrtp backend (id "libsrtp").
 */
class NIMRTC_API_UNSTABLE LibsrtpBackendFactory : public plugins::ISrtpBackendFactory {
public:
    std::string_view id() const noexcept override;
    std::string_view name() const noexcept override;
    std::unique_ptr<plugins::ISrtpSessionBackend> create() const noexcept override;
};

}  // namespace nimrtc::srtp
