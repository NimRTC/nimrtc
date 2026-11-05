/**
 * @file nimrtc/plugins/srtp_backend.hpp
 * @brief ISrtpSessionBackend — pluggable SRTP crypto backend seam (v1.3 X2).
 *
 * The built-in backend is libsrtp (factory id "libsrtp",
 * `nimrtc::srtp::LibsrtpBackendFactory`). SM4-GCM is explicitly OUT OF
 * SCOPE for v1.3 — the seam exists so a v1.4+ backend can be added without
 * touching callers. SRTP still uses the international AES suites; see the
 * README scope note ("DTLS/TLCP via GMSSL, SRTP still international").
 *
 * L2: interface + factory only, no implementation here.
 */

#include <nimrtc/core/api_version.hpp>
#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/error.hpp>
#include <nimrtc/plugins/base.hpp>

#ifndef NIMRTC_PLUGINS_SRTP_BACKEND_HPP
#define NIMRTC_PLUGINS_SRTP_BACKEND_HPP

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// Backend session (one SRTP stream direction)
// ---------------------------------------------------------------------------

class NIMRTC_API_UNSTABLE ISrtpSessionBackend {
public:
    virtual ~ISrtpSessionBackend() = default;

    virtual core::Result<void> init_outbound(std::span<const std::uint8_t> master_key,
                                             std::span<const std::uint8_t> master_salt,
                                             std::uint32_t suite_id) noexcept = 0;
    virtual core::Result<void> init_inbound(std::span<const std::uint8_t> master_key,
                                            std::span<const std::uint8_t> master_salt,
                                            std::uint32_t suite_id) noexcept = 0;

    virtual core::Result<core::ByteSpan> protect_rtp(
        core::ByteSpan header_and_plaintext) noexcept = 0;
    virtual core::Result<core::ByteSpan> unprotect_rtp(
        core::ByteSpan srtp_packet) noexcept = 0;
    virtual core::Result<core::ByteSpan> protect_rtcp(
        core::ByteSpan rtcp_packet) noexcept = 0;
    virtual core::Result<core::ByteSpan> unprotect_rtcp(
        core::ByteSpan srtcp_packet) noexcept = 0;

    virtual std::uint32_t current_roc() const noexcept = 0;
    virtual void reset() noexcept = 0;
};

// ---------------------------------------------------------------------------
// Backend factory
// ---------------------------------------------------------------------------

class NIMRTC_API_UNSTABLE ISrtpBackendFactory {
public:
    virtual ~ISrtpBackendFactory() = default;
    virtual std::string_view id() const noexcept = 0;
    virtual std::string_view name() const noexcept = 0;
    virtual std::unique_ptr<ISrtpSessionBackend> create() const noexcept = 0;
};

}  // namespace nimrtc::plugins

#endif  // NIMRTC_PLUGINS_SRTP_BACKEND_HPP
