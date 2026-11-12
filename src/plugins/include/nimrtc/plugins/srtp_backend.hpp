/**
 * @file nimrtc/plugins/srtp_backend.hpp
 * @brief ISrtpSessionBackend — pluggable SRTP crypto backend seam (v1.1 X2).
 *
 * The built-in backend is libsrtp (factory id "libsrtp",
 * `nimrtc::srtp::LibsrtpBackendFactory`). SM4-GCM is explicitly OUT OF
 * SCOPE for v1.1 — the seam exists so a v1.2+ backend can be added without
 * touching callers. SRTP still uses the international AES suites; see the
 * README scope note ("DTLS/TLCP via GMSSL, SRTP still international").
 *
 * ## Wired (v1.2.0, architecture remediation plan B7 / Q3)
 *
 * Before v1.2.0 this seam was a **fake seam**: the interface, the factory
 * and the libsrtp adapter all existed, but
 *
 *   - the registry had **no slot** for the family, so no id could ever be
 *     resolved, and
 *   - `srtp::SrtpContext` called libsrtp **directly**, so the adapter was
 *     never on the path.
 *
 * v1.2.0 fixes both: the registry gained the `srtp_backend` slot
 * (`core::PluginRegistry::register_srtp_backend` /
 * `get_srtp_backend` / `list_srtp_backends`, plus the
 * `NIMRTC_REGISTER_SRTP_BACKEND` macro), and `srtp::SrtpSession` now
 * routes every protect / unprotect through a
 * `plugins::ISrtpSessionBackend` instance.  The engine selects the
 * backend through `EngineConfig::srtp_backend_name` (default "libsrtp");
 * an unknown id is a **hard failure** at `open()` time — there is no
 * silent fallback to libsrtp.
 *
 * ## Session model
 *
 * One `ISrtpSessionBackend` instance is **one SRTP crypto context in one
 * direction** — the seam's `init_outbound` / `init_inbound` select the
 * polarity.  The engine's `srtp::SrtpContext` owns one instance per
 * (SSRC, direction) pair, which is exactly the model RFC 3711 needs
 * (a separate rollover counter and auth state per stream per direction).
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
// Crypto suite
// ---------------------------------------------------------------------------

/**
 * @brief The SRTP protection profiles a backend may implement.
 *
 * The values are **backend-independent** — they are the RFC 4568
 * profile names, not any particular library's enum.  A backend maps
 * them onto its own library.
 *
 * @note v1.2.0: the seam's `suite_id` parameter now carries one of these
 *       values.  Before v1.2.0 it carried a libsrtp profile id, which
 *       made the seam libsrtp-specific by accident.  The seam is
 *       `NIMRTC_API_UNSTABLE`, so the semantic change is allowed;
 *       `srtp::CryptoSuite` has the same ordering, so the conversion is
 *       a static_cast.
 */
enum class NIMRTC_API_UNSTABLE SrtpCryptoSuite : std::uint32_t {
    kAes128CmSha1_80 = 0,   ///< AES-128-CM, HMAC-SHA1-80 auth (default)
    kAes128CmSha1_32 = 1,   ///< AES-128-CM, HMAC-SHA1-32 auth
    kAes128Gcm_128   = 2,   ///< AES-128-GCM (AEAD)
    kAes256CmSha1_80 = 3,   ///< AES-256-CM, HMAC-SHA1-80 auth
};

// ---------------------------------------------------------------------------
// Backend session (one SRTP stream direction)
// ---------------------------------------------------------------------------

class NIMRTC_API_UNSTABLE ISrtpSessionBackend {
public:
    virtual ~ISrtpSessionBackend() = default;

    /** Install the master key for the **outbound** direction. */
    virtual core::Result<void> init_outbound(std::span<const std::uint8_t> master_key,
                                             std::span<const std::uint8_t> master_salt,
                                             std::uint32_t suite_id) noexcept = 0;
    /** Install the master key for the **inbound** direction. */
    virtual core::Result<void> init_inbound(std::span<const std::uint8_t> master_key,
                                            std::span<const std::uint8_t> master_salt,
                                            std::uint32_t suite_id) noexcept = 0;

    /**
     * @brief Install already-derived *session* keys (not master keys).
     *
     * Optional: a backend that cannot express session-key install
     * returns `ErrorCode::NotImplemented`, and the caller falls back to
     * the master-key path.  The default implementation does exactly
     * that.
     *
     * @note v1.2.0 (remediation plan B7 / Q3) — additive.
     */
    virtual core::Result<void> init_outbound_session_keys(
        std::span<const std::uint8_t> session_key,
        std::span<const std::uint8_t> session_salt,
        std::uint32_t suite_id) noexcept {
        (void)session_key;
        (void)session_salt;
        (void)suite_id;
        return core::Result<void>::fail(
            core::ErrorCode::NotImplemented,
            "SRTP backend: session-key install not supported");
    }

    /** @copydoc init_outbound_session_keys */
    virtual core::Result<void> init_inbound_session_keys(
        std::span<const std::uint8_t> session_key,
        std::span<const std::uint8_t> session_salt,
        std::uint32_t suite_id) noexcept {
        (void)session_key;
        (void)session_salt;
        (void)suite_id;
        return core::Result<void>::fail(
            core::ErrorCode::NotImplemented,
            "SRTP backend: session-key install not supported");
    }

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
