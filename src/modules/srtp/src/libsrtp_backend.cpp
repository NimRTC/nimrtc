/**
 * @file src/modules/srtp/src/libsrtp_backend.cpp
 * @brief Libsrtp-backed ISrtpSessionBackend adapter (v1.3 X2).
 */

#include <nimrtc/srtp/libsrtp_backend.hpp>

#include <exception>

#include <nimrtc/core/error.hpp>

namespace nimrtc::srtp {

namespace {

CryptoSuite suite_from_id(std::uint32_t suite_id) noexcept {
    if (suite_id == to_libsrtp_profile(CryptoSuite::Aes128CmSha1_32)) {
        return CryptoSuite::Aes128CmSha1_32;
    }
    if (suite_id == to_libsrtp_profile(CryptoSuite::Aes128Gcm_128)) {
        return CryptoSuite::Aes128Gcm_128;
    }
    if (suite_id == to_libsrtp_profile(CryptoSuite::Aes256CmSha1_80)) {
        return CryptoSuite::Aes256CmSha1_80;
    }
    return CryptoSuite::Aes128CmSha1_80;
}

Config config_for(CryptoSuite suite) {
    Config cfg;
    cfg.suite = suite;
    cfg.enable_auth = true;
    cfg.enable_encryption = true;
    return cfg;
}

}  // namespace

LibsrtpSessionBackend::LibsrtpSessionBackend() = default;
LibsrtpSessionBackend::~LibsrtpSessionBackend() = default;

core::Result<void> LibsrtpSessionBackend::init_outbound(
    std::span<const std::uint8_t> master_key,
    std::span<const std::uint8_t> master_salt,
    std::uint32_t suite_id) noexcept {
    try {
        return session_.init_from_master_key(config_for(suite_from_id(suite_id)),
                                             master_key, master_salt);
    } catch (const std::exception& e) {
        return core::Result<void>::fail(core::ErrorCode::CryptoError,
                                        e.what());
    } catch (...) {
        return core::Result<void>::fail(core::ErrorCode::CryptoError,
                                        "SRTP backend: init_outbound failed");
    }
}

core::Result<void> LibsrtpSessionBackend::init_inbound(
    std::span<const std::uint8_t> master_key,
    std::span<const std::uint8_t> master_salt,
    std::uint32_t suite_id) noexcept {
    try {
        return session_.init_from_master_key_inbound(config_for(suite_from_id(suite_id)),
                                                     master_key, master_salt);
    } catch (const std::exception& e) {
        return core::Result<void>::fail(core::ErrorCode::CryptoError,
                                        e.what());
    } catch (...) {
        return core::Result<void>::fail(core::ErrorCode::CryptoError,
                                        "SRTP backend: init_inbound failed");
    }
}

core::Result<core::ByteSpan> LibsrtpSessionBackend::protect_rtp(
    core::ByteSpan header_and_plaintext) noexcept {
    try {
        // SrtpSession ignores the ssrc/timestamp hints (reads them from
        // the RTP header itself); the seam drops those parameters.
        return session_.protect_rtp(header_and_plaintext, 0, 0);
    } catch (const std::exception& e) {
        return core::Result<core::ByteSpan>::fail(core::ErrorCode::CryptoError,
                                                  e.what());
    } catch (...) {
        return core::Result<core::ByteSpan>::fail(core::ErrorCode::CryptoError,
                                                  "SRTP backend: protect failed");
    }
}

core::Result<core::ByteSpan> LibsrtpSessionBackend::unprotect_rtp(
    core::ByteSpan srtp_packet) noexcept {
    try {
        return session_.unprotect_rtp(srtp_packet, nullptr, nullptr);
    } catch (const std::exception& e) {
        return core::Result<core::ByteSpan>::fail(core::ErrorCode::CryptoError,
                                                  e.what());
    } catch (...) {
        return core::Result<core::ByteSpan>::fail(core::ErrorCode::CryptoError,
                                                  "SRTP backend: unprotect failed");
    }
}

core::Result<core::ByteSpan> LibsrtpSessionBackend::protect_rtcp(
    core::ByteSpan rtcp_packet) noexcept {
    try {
        return session_.protect_rtcp(rtcp_packet);
    } catch (const std::exception& e) {
        return core::Result<core::ByteSpan>::fail(core::ErrorCode::CryptoError,
                                                  e.what());
    } catch (...) {
        return core::Result<core::ByteSpan>::fail(core::ErrorCode::CryptoError,
                                                  "SRTP backend: protect_rtcp failed");
    }
}

core::Result<core::ByteSpan> LibsrtpSessionBackend::unprotect_rtcp(
    core::ByteSpan srtcp_packet) noexcept {
    try {
        return session_.unprotect_rtcp(srtcp_packet);
    } catch (const std::exception& e) {
        return core::Result<core::ByteSpan>::fail(core::ErrorCode::CryptoError,
                                                  e.what());
    } catch (...) {
        return core::Result<core::ByteSpan>::fail(core::ErrorCode::CryptoError,
                                                  "SRTP backend: unprotect_rtcp failed");
    }
}

std::uint32_t LibsrtpSessionBackend::current_roc() const noexcept {
    return session_.current_roc();
}

void LibsrtpSessionBackend::reset() noexcept {
    session_.reset();
}

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

}  // namespace nimrtc::srtp
