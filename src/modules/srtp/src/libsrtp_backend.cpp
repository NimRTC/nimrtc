/**
 * @file src/modules/srtp/src/libsrtp_backend.cpp
 * @brief Libsrtp-backed ISrtpSessionBackend adapter (v1.1 X2; v1.2.0
 *        wired into the main path).
 *
 * Since v1.2.0 (architecture remediation plan B7 / Q3) this adapter is
 * what actually runs on the SRTP path: `srtp::SrtpSession` forwards
 * every protect / unprotect to it through the
 * `plugins::ISrtpSessionBackend` seam.
 *
 * The adapter owns one libsrtp `srtp_t` context and installs the master
 * key with the polarity the caller asked for (outbound -> `ssrc_any_outbound`,
 * inbound -> `ssrc_any_inbound`).
 */

#include <nimrtc/srtp/libsrtp_backend.hpp>

#include <mutex>

#include <srtp.h>

#include <nimrtc/core/error.hpp>
#include <nimrtc/core/log.hpp>

namespace nimrtc::srtp {

namespace {

// libsrtp init guard - srtp_init() must be called once before any other
// srtp_*.  Use std::call_once on first use.
std::once_flag g_srtp_init_once;

void ensure_srtp_init() {
    std::call_once(g_srtp_init_once, [] {
        srtp_err_status_t rc = srtp_init();
        if (rc != srtp_err_status_ok) {
            core::log::Logger::instance().error(
                "srtp_init() failed - SRTP unusable");
        } else {
            core::log::Logger::instance().debug(
                "libsrtp2 initialised");
        }
    });
}

srtp_profile_t to_lib_profile(std::uint32_t suite_id) noexcept {
    switch (suite_id) {
        case 1: return srtp_profile_aes128_cm_sha1_32;
        case 2: return srtp_profile_aead_aes_128_gcm;
        case 3: return srtp_profile_aes256_cm_sha1_80;
        default: return srtp_profile_aes128_cm_sha1_80;
    }
}

constexpr std::size_t kAuthTagLenSha1_80 = 10;
constexpr std::size_t kAuthTagLenSha1_32 = 4;

std::size_t auth_tag_len_for(std::uint32_t suite_id) noexcept {
    switch (suite_id) {
        case 1:  return kAuthTagLenSha1_32;
        case 2:  return 16;
        default: return kAuthTagLenSha1_80;
    }
}

/** Build a libsrtp policy with the requested key and SSRC polarity, and
 *  create the crypto context.  Shared by the outbound and inbound
 *  initialisers - the only difference is the SSRC selector. */
core::Result<void> create_context(std::span<const std::uint8_t> master_key,
                                  std::span<const std::uint8_t> master_salt,
                                  std::uint32_t suite_id,
                                  bool outbound,
                                  srtp_t& out) {
    ensure_srtp_init();

    if (master_key.empty() || master_salt.empty()) {
        return core::Result<void>::fail(core::ErrorCode::InvalidArgument,
                                        "SRTP backend: empty key/salt");
    }

    srtp_policy_t policy{};
    srtp_err_status_t rc = srtp_policy_create(&policy);
    if (rc != srtp_err_status_ok) {
        return core::Result<void>::fail(core::ErrorCode::CryptoError,
                                        "SRTP backend: srtp_policy_create failed");
    }

    srtp_ssrc_t ssrc_sel{};
    // The wildcard "any" selectors are what RFC 3711 needs here: the
    // SRTP context is per-(SSRC, direction), and the inbound context is
    // created before the peer's SSRC is known.
    ssrc_sel.type  = outbound ? ssrc_any_outbound : ssrc_any_inbound;
    ssrc_sel.value = 0;
    srtp_policy_set_ssrc(policy, ssrc_sel);
    srtp_policy_set_profile(policy, to_lib_profile(suite_id));
    srtp_policy_set_sec_serv(policy, sec_serv_conf_and_auth,
                                    sec_serv_conf_and_auth);

    // libsrtp v3 expects the master key and master salt SEPARATELY.
    rc = srtp_policy_add_key(policy,
                             master_key.data(),
                             master_key.size(),
                             master_salt.data(),
                             master_salt.size(),
                             /*mki=*/nullptr,
                             /*mki_len=*/0);
    if (rc != srtp_err_status_ok) {
        srtp_policy_destroy(policy);
        return core::Result<void>::fail(core::ErrorCode::CryptoError,
                                        "SRTP backend: srtp_policy_add_key failed");
    }

    srtp_t ctx = nullptr;
    rc = srtp_create(&ctx, policy);
    srtp_policy_destroy(policy);
    if (rc != srtp_err_status_ok) {
        return core::Result<void>::fail(core::ErrorCode::CryptoError,
                                        "SRTP backend: srtp_create failed");
    }
    out = ctx;
    return core::Result<void>::make_ok();
}

} // namespace

// ---------------------------------------------------------------------------
// LibsrtpSessionBackend - the concrete crypto context.
// ---------------------------------------------------------------------------

LibsrtpSessionBackend::LibsrtpSessionBackend() = default;
LibsrtpSessionBackend::~LibsrtpSessionBackend() = default;

core::Result<void> LibsrtpSessionBackend::init_outbound(
    std::span<const std::uint8_t> master_key,
    std::span<const std::uint8_t> master_salt,
    std::uint32_t suite_id) noexcept {
    srtp_t ctx = nullptr;
    auto rc = create_context(master_key, master_salt, suite_id,
                             /*outbound=*/true, ctx);
    if (!rc) return rc;
    session_ = ctx;
    suite_id_ = suite_id;
    return core::Result<void>::make_ok();
}

core::Result<void> LibsrtpSessionBackend::init_inbound(
    std::span<const std::uint8_t> master_key,
    std::span<const std::uint8_t> master_salt,
    std::uint32_t suite_id) noexcept {
    srtp_t ctx = nullptr;
    auto rc = create_context(master_key, master_salt, suite_id,
                             /*outbound=*/false, ctx);
    if (!rc) return rc;
    session_ = ctx;
    suite_id_ = suite_id;
    return core::Result<void>::make_ok();
}

core::Result<core::ByteSpan> LibsrtpSessionBackend::protect_rtp(
    core::ByteSpan header_and_plaintext) noexcept {
    if (!session_) {
        return core::Result<core::ByteSpan>::fail(
            core::ErrorCode::InvalidState, "SRTP backend: not initialised");
    }
    auto* ctx = static_cast<srtp_t>(session_);
    const std::size_t in_len = header_and_plaintext.size();
    std::size_t out_len = in_len + auth_tag_len_for(suite_id_);
    work_buf_.resize(out_len);

    srtp_err_status_t rc = srtp_protect(ctx,
                                       header_and_plaintext.data(),
                                       in_len,
                                       work_buf_.data(),
                                       &out_len,
                                       /*mki_index=*/0);
    if (rc != srtp_err_status_ok) {
        return core::Result<core::ByteSpan>::fail(core::ErrorCode::ProtocolError,
                                                  "SRTP backend: protect failed");
    }
    work_buf_.resize(out_len);
    return core::Result<core::ByteSpan>::ok(
        core::ByteSpan(work_buf_.data(), work_buf_.size()));
}

core::Result<core::ByteSpan> LibsrtpSessionBackend::unprotect_rtp(
    core::ByteSpan srtp_packet) noexcept {
    if (!session_) {
        return core::Result<core::ByteSpan>::fail(
            core::ErrorCode::InvalidState, "SRTP backend: not initialised");
    }
    auto* ctx = static_cast<srtp_t>(session_);
    out_buf_.resize(srtp_packet.size());
    std::size_t out_len = out_buf_.size();
    srtp_err_status_t rc = srtp_unprotect(ctx,
                                          srtp_packet.data(),
                                          srtp_packet.size(),
                                          out_buf_.data(),
                                          &out_len);
    if (rc != srtp_err_status_ok) {
        return core::Result<core::ByteSpan>::fail(core::ErrorCode::ProtocolError,
                                                  "SRTP backend: unprotect failed");
    }
    out_buf_.resize(out_len);
    return core::Result<core::ByteSpan>::ok(
        core::ByteSpan(out_buf_.data(), out_buf_.size()));
}

core::Result<core::ByteSpan> LibsrtpSessionBackend::protect_rtcp(
    core::ByteSpan rtcp_packet) noexcept {
    if (!session_) {
        return core::Result<core::ByteSpan>::ok(rtcp_packet);
    }
    auto* ctx = static_cast<srtp_t>(session_);
    std::size_t out_len = rtcp_packet.size() + auth_tag_len_for(suite_id_);
    work_buf_.resize(out_len);
    srtp_err_status_t rc = srtp_protect_rtcp(ctx,
                                              rtcp_packet.data(),
                                              rtcp_packet.size(),
                                              work_buf_.data(),
                                              &out_len,
                                              /*mki_index=*/0);
    if (rc != srtp_err_status_ok) {
        return core::Result<core::ByteSpan>::fail(core::ErrorCode::ProtocolError,
                                                  "SRTP backend: protect_rtcp failed");
    }
    work_buf_.resize(out_len);
    return core::Result<core::ByteSpan>::ok(
        core::ByteSpan(work_buf_.data(), work_buf_.size()));
}

core::Result<core::ByteSpan> LibsrtpSessionBackend::unprotect_rtcp(
    core::ByteSpan srtcp_packet) noexcept {
    if (!session_) {
        return core::Result<core::ByteSpan>::ok(srtcp_packet);
    }
    auto* ctx = static_cast<srtp_t>(session_);
    out_buf_.resize(srtcp_packet.size());
    std::size_t out_len = out_buf_.size();
    srtp_err_status_t rc = srtp_unprotect_rtcp(ctx,
                                                srtcp_packet.data(),
                                                srtcp_packet.size(),
                                                out_buf_.data(),
                                                &out_len);
    if (rc != srtp_err_status_ok) {
        return core::Result<core::ByteSpan>::fail(core::ErrorCode::ProtocolError,
                                                  "SRTP backend: unprotect_rtcp failed");
    }
    out_buf_.resize(out_len);
    return core::Result<core::ByteSpan>::ok(
        core::ByteSpan(out_buf_.data(), out_buf_.size()));
}

std::uint32_t LibsrtpSessionBackend::current_roc() const noexcept {
    if (!session_) return 0;
    std::uint32_t roc = 0;
    srtp_stream_get_roc(static_cast<srtp_t>(session_), /*ssrc=*/0, &roc);
    return roc;
}

void LibsrtpSessionBackend::reset() noexcept {
    if (session_) {
        srtp_dealloc(static_cast<srtp_t>(session_));
        session_ = nullptr;
    }
    work_buf_.clear();
    out_buf_.clear();
}

// ---------------------------------------------------------------------------
// LibsrtpBackendFactory
// ---------------------------------------------------------------------------

std::string_view LibsrtpBackendFactory::id() const noexcept {
    return "libsrtp";
}

std::string_view LibsrtpBackendFactory::name() const noexcept {
    return "libsrtp (RFC 3711, international AES suites)";
}

std::unique_ptr<plugins::ISrtpSessionBackend> LibsrtpBackendFactory::create() const noexcept {
    try {
        return std::make_unique<LibsrtpSessionBackend>();
    } catch (...) {
        return nullptr;
    }
}

} // namespace nimrtc::srtp
