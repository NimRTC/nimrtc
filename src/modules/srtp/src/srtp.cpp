/**
 * @file src/modules/srtp/src/srtp.cpp
 * @brief SRTP — libsrtp2 integration.
 *
 * Wraps libsrtp2 (RFC 3711) for SRTP_AES128_CM_SHA1_80 — the profile
 * negotiated by the DTLS-SRTP extension (RFC 5764) used in NimRTC.
 *
 * ## Suite mapping (libsrtp enums)
 *
 *   SrtpSuite::Aes128CmSha1_80  ↔ srtp_profile_aes128_cm_sha1_80
 *   SrtpSuite::Aes128CmSha1_32  ↔ srtp_profile_aes128_cm_sha1_32
 *   SrtpSuite::Aes128Gcm_128    ↔ srtp_profile_aead_aes_128_gcm
 *   SrtpSuite::Aes256CmSha1_80  ↔ srtp_profile_aes256_cm_sha1_80
 *
 * ## Key/salt layout
 *
 * libsrtp's `srtp_policy_add_key()` takes a single byte buffer of
 * `key_len + salt_len`.  For AES-CM-128 (key_len=16, salt_len=14) the
 * buffer is 30 bytes: `<16-byte key><14-byte salt>`.
 *
 * When `enable_encryption` is true we add keys via `srtp_policy_add_key()`
 * directly from the master key + salt.  When false we add them but only
 * auth (placeholder, not exercised by NimRTC).
 *
 * ## Threading
 *
 * SrtpSession is NOT thread-safe — same as the rest of the engine.
 */

#include <nimrtc/srtp/srtp.hpp>

#include <cstring>
#include <mutex>
#include <unordered_map>

#include <srtp.h>

#include <nimrtc/core/error.hpp>
#include <nimrtc/core/log.hpp>

namespace nimrtc::srtp {

namespace {

// libsrtp init guard — srtp_init() must be called once before any other
// srtp_*.  Use std::call_once on first use.
std::once_flag g_srtp_init_once;

void ensure_srtp_init() {
    std::call_once(g_srtp_init_once, [] {
        srtp_err_status_t rc = srtp_init();
        if (rc != srtp_err_status_ok) {
            core::log::Logger::instance().error(
                "srtp_init() failed — SRTP unusable");
        } else {
            core::log::Logger::instance().debug(
                "libsrtp2 initialised");
        }
    });
}

srtp_profile_t to_lib_profile(CryptoSuite suite) noexcept {
    switch (suite) {
        case CryptoSuite::Aes128CmSha1_80: return srtp_profile_aes128_cm_sha1_80;
        case CryptoSuite::Aes128CmSha1_32: return srtp_profile_aes128_cm_sha1_32;
        case CryptoSuite::Aes128Gcm_128:   return srtp_profile_aead_aes_128_gcm;
        case CryptoSuite::Aes256CmSha1_80: return srtp_profile_aes256_cm_sha1_80;
    }
    return srtp_profile_aes128_cm_sha1_80;
}

constexpr std::size_t kAuthTagLenSha1_80 = 10;
constexpr std::size_t kAuthTagLenSha1_32 = 4;

std::size_t auth_tag_len_for(CryptoSuite suite) noexcept {
    switch (suite) {
        case CryptoSuite::Aes128CmSha1_80:
        case CryptoSuite::Aes256CmSha1_80: return kAuthTagLenSha1_80;
        case CryptoSuite::Aes128CmSha1_32: return kAuthTagLenSha1_32;
        case CryptoSuite::Aes128Gcm_128:   return 16;
    }
    return kAuthTagLenSha1_80;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// to_libsrtp_profile (kept for public-API compatibility)
// ---------------------------------------------------------------------------
std::uint32_t to_libsrtp_profile(CryptoSuite suite) {
    return static_cast<std::uint32_t>(to_lib_profile(suite));
}

// ---------------------------------------------------------------------------
// SrtpSession::Impl
// ---------------------------------------------------------------------------

struct SrtpSession::Impl {
    Config                                  config{};
    srtp_t                                  session = nullptr;
    bool                                    owns_session = true;  // we created it via srtp_create
    std::uint32_t                           bound_ssrc = 0;
    bool                                    is_outbound = true;
    Stats                                   stats_{};

    // Working buffer for protect (must hold plaintext + auth tag).
    std::vector<std::uint8_t>               work_buf_;

    ~Impl() {
        if (session) {
            srtp_dealloc(session);
            session = nullptr;
        }
    }

    /** Build a libsrtp policy + add a key.  libsrtp v3 expects the master
     *  key and master salt to be passed SEPARATELY (not concatenated). */
    bool install(std::span<const std::uint8_t> key,
                 std::span<const std::uint8_t> salt,
                 std::uint32_t ssrc,
                 bool outbound,
                 CryptoSuite suite,
                 srtp_sec_serv_t sec_serv) {
        ensure_srtp_init();

        if (session) {
            srtp_dealloc(session);
            session = nullptr;
        }
        owns_session = true;

        srtp_policy_t policy;
        srtp_err_status_t rc = srtp_policy_create(&policy);
        if (rc != srtp_err_status_ok) return false;

        srtp_ssrc_t ssrc_sel;
        // For tests that pre-allocate a session before knowing SSRC, use the
        // "any" wildcard: any-outbound for senders, any-inbound for receivers.
        // Real production code should pass the actual SSRC via ssrc_specific.
        if (outbound) {
            ssrc_sel.type  = ssrc_any_outbound;
            ssrc_sel.value = 0;
        } else {
            ssrc_sel.type  = ssrc_any_inbound;
            ssrc_sel.value = 0;
        }
        srtp_policy_set_ssrc(policy, ssrc_sel);
        srtp_policy_set_profile(policy, to_lib_profile(suite));
        srtp_policy_set_sec_serv(policy, sec_serv, sec_serv);

        rc = srtp_policy_add_key(policy,
                                 key.data(),
                                 key.size(),
                                 salt.data(),
                                 salt.size(),
                                 /*mki=*/nullptr,
                                 /*mki_len=*/0);
        if (rc != srtp_err_status_ok) {
            core::log::Logger::instance().error(
                "SRTP: srtp_policy_add_key failed rc=" +
                std::to_string(static_cast<int>(rc)));
            srtp_policy_destroy(policy);
            return false;
        }

        rc = srtp_create(&session, policy);
        srtp_policy_destroy(policy);
        if (rc != srtp_err_status_ok) {
            session = nullptr;
            return false;
        }

        bound_ssrc = ssrc;
        is_outbound = outbound;
        return true;
    }
};

SrtpSession::SrtpSession() : impl_(std::make_unique<Impl>()) {}
SrtpSession::~SrtpSession() = default;
SrtpSession::SrtpSession(SrtpSession&&) noexcept = default;
SrtpSession& SrtpSession::operator=(SrtpSession&&) noexcept = default;

core::Result<void> SrtpSession::init_from_master_key(
        const Config& config,
        std::span<const std::uint8_t> master_key,
        std::span<const std::uint8_t> master_salt) {
    impl_->config = config;
    if (master_key.empty() || master_salt.empty()) {
        return core::Result<void>::fail(
            core::ErrorCode::InvalidArgument, "SRTP: empty key/salt");
    }

    // Default bound SSRC = 0; library treats that as outbound-any, fine
    // for the stub usage; for test purposes we pass 0 here.
    if (!impl_->install(master_key, master_salt,
                        /*ssrc=*/0, /*outbound=*/true,
                        config.suite,
                        config.enable_encryption
                            ? sec_serv_conf_and_auth
                            : sec_serv_auth)) {
        return core::Result<void>::fail(
            core::ErrorCode::InvalidArgument,
            "SRTP: libsrtp install failed");
    }
    return core::Result<void>::make_ok();
}

core::Result<void> SrtpSession::init_from_master_key_inbound(
        const Config& config,
        std::span<const std::uint8_t> master_key,
        std::span<const std::uint8_t> master_salt) {
    impl_->config = config;
    if (master_key.empty() || master_salt.empty()) {
        return core::Result<void>::fail(
            core::ErrorCode::InvalidArgument, "SRTP: empty key/salt");
    }

    if (!impl_->install(master_key, master_salt,
                        /*ssrc=*/0, /*outbound=*/false,
                        config.suite,
                        config.enable_encryption
                            ? sec_serv_conf_and_auth
                            : sec_serv_auth)) {
        return core::Result<void>::fail(
            core::ErrorCode::InvalidArgument,
            "SRTP: libsrtp install (inbound) failed");
    }
    return core::Result<void>::make_ok();
}

core::Result<void> SrtpSession::init_from_session_keys(const Config& config) {
    // Same code path: libsrtp expects separate master key + master salt.
    impl_->config = config;
    if (config.session_key_rtp.empty() || config.session_salt_rtp.empty()) {
        return core::Result<void>::fail(
            core::ErrorCode::InvalidArgument, "SRTP: empty session keys");
    }
    if (!impl_->install(config.session_key_rtp, config.session_salt_rtp,
                        /*ssrc=*/0, true, config.suite,
                        config.enable_encryption ? sec_serv_conf_and_auth : sec_serv_auth)) {
        return core::Result<void>::fail(
            core::ErrorCode::InvalidArgument, "SRTP: libsrtp install failed");
    }
    return core::Result<void>::make_ok();
}

core::Result<core::ByteSpan> SrtpSession::protect_rtp(
        core::ByteSpan header_and_plaintext,
        std::uint32_t /*ssrc*/,
        std::uint32_t /*rtp_timestamp*/) {
    if (!impl_->session) {
        return core::Result<core::ByteSpan>::fail(
            core::ErrorCode::InvalidState, "SRTP: session not initialised");
    }
    std::size_t in_len = header_and_plaintext.size();
    std::size_t out_len = in_len + auth_tag_len_for(impl_->config.suite);
    impl_->work_buf_.resize(out_len);

    srtp_err_status_t rc = srtp_protect(impl_->session,
                                       header_and_plaintext.data(),
                                       in_len,
                                       impl_->work_buf_.data(),
                                       &out_len,
                                       /*mki_index=*/0);
    if (rc != srtp_err_status_ok) {
        ++impl_->stats_.decryption_failures;
        return core::Result<core::ByteSpan>::fail(
            core::ErrorCode::ProtocolError, "SRTP: srtp_protect failed");
    }

    ++impl_->stats_.rtp_packets_encrypted;
    impl_->work_buf_.resize(out_len);
    return core::Result<core::ByteSpan>::ok(
        core::ByteSpan(impl_->work_buf_.data(), impl_->work_buf_.size()));
}

core::Result<core::ByteSpan> SrtpSession::unprotect_rtp(
        core::ByteSpan srtp_packet,
        std::uint32_t* out_ssrc,
        std::uint32_t* out_timestamp) {
    if (!impl_->session) {
        return core::Result<core::ByteSpan>::fail(
            core::ErrorCode::InvalidState, "SRTP: session not initialised");
    }
    if (srtp_packet.size() < 12) {
        ++impl_->stats_.decryption_failures;
        return core::Result<core::ByteSpan>::fail(
            core::ErrorCode::ProtocolError, "SRTP: packet too short");
    }
    if (out_ssrc) {
        const auto* d = srtp_packet.data();
        *out_ssrc = (static_cast<std::uint32_t>(d[8]) << 24) |
                    (static_cast<std::uint32_t>(d[9]) << 16) |
                    (static_cast<std::uint32_t>(d[10]) << 8) |
                    static_cast<std::uint32_t>(d[11]);
    }
    if (out_timestamp) {
        const auto* d = srtp_packet.data();
        *out_timestamp = (static_cast<std::uint32_t>(d[4]) << 24) |
                         (static_cast<std::uint32_t>(d[5]) << 16) |
                         (static_cast<std::uint32_t>(d[6]) << 8) |
                         static_cast<std::uint32_t>(d[7]);
    }
    // Output buffer must be at least srtp_in_len (tags add a few bytes).
    std::vector<std::uint8_t> out_buf(srtp_packet.size());
    std::size_t out_len = out_buf.size();
    srtp_err_status_t rc = srtp_unprotect(impl_->session,
                                          srtp_packet.data(),
                                          srtp_packet.size(),
                                          out_buf.data(),
                                          &out_len);
    if (rc != srtp_err_status_ok) {
        ++impl_->stats_.decryption_failures;
        return core::Result<core::ByteSpan>::fail(
            core::ErrorCode::ProtocolError, "SRTP: srtp_unprotect failed");
    }
    ++impl_->stats_.rtp_packets_decrypted;

    // Stash output for return (work_buf_ reused for protect — use a stack-copy).
    static thread_local std::vector<std::uint8_t> tls_buf;
    tls_buf.assign(out_buf.begin(), out_buf.begin() + out_len);
    return core::Result<core::ByteSpan>::ok(
        core::ByteSpan(tls_buf.data(), tls_buf.size()));
}

core::Result<core::ByteSpan> SrtpSession::protect_rtcp(core::ByteSpan rtcp_packet) {
    if (!impl_->session) return core::Result<core::ByteSpan>::ok(rtcp_packet);
    std::size_t out_len = rtcp_packet.size() + auth_tag_len_for(impl_->config.suite);
    impl_->work_buf_.resize(out_len);
    srtp_err_status_t rc = srtp_protect_rtcp(impl_->session,
                                              rtcp_packet.data(),
                                              rtcp_packet.size(),
                                              impl_->work_buf_.data(),
                                              &out_len,
                                              /*mki_index=*/0);
    if (rc != srtp_err_status_ok) {
        return core::Result<core::ByteSpan>::fail(
            core::ErrorCode::ProtocolError, "SRTP: protect_rtcp failed");
    }
    impl_->work_buf_.resize(out_len);
    ++impl_->stats_.rtcp_packets_encrypted;
    return core::Result<core::ByteSpan>::ok(
        core::ByteSpan(impl_->work_buf_.data(), impl_->work_buf_.size()));
}

core::Result<core::ByteSpan> SrtpSession::unprotect_rtcp(core::ByteSpan srtcp_packet) {
    if (!impl_->session) return core::Result<core::ByteSpan>::ok(srtcp_packet);
    std::vector<std::uint8_t> out_buf(srtcp_packet.size());
    std::size_t out_len = out_buf.size();
    srtp_err_status_t rc = srtp_unprotect_rtcp(impl_->session,
                                                srtcp_packet.data(),
                                                srtcp_packet.size(),
                                                out_buf.data(),
                                                &out_len);
    if (rc != srtp_err_status_ok) {
        return core::Result<core::ByteSpan>::fail(
            core::ErrorCode::ProtocolError, "SRTP: unprotect_rtcp failed");
    }
    ++impl_->stats_.rtcp_packets_decrypted;
    static thread_local std::vector<std::uint8_t> tls_buf;
    tls_buf.assign(out_buf.begin(), out_buf.begin() + out_len);
    return core::Result<core::ByteSpan>::ok(
        core::ByteSpan(tls_buf.data(), tls_buf.size()));
}

std::uint32_t SrtpSession::current_roc() const {
    if (!impl_->session) return 0;
    std::uint32_t roc = 0;
    srtp_stream_get_roc(impl_->session, impl_->bound_ssrc, &roc);
    return roc;
}
SrtpSession::Stats SrtpSession::stats() const { return impl_->stats_; }
void SrtpSession::reset() { impl_->stats_ = Stats{}; }

// ---------------------------------------------------------------------------
// SrtpContext::Impl
// ---------------------------------------------------------------------------

struct SrtpContext::Impl {
    CryptoSuite                              default_suite = CryptoSuite::Aes128CmSha1_80;
    std::unordered_map<std::uint32_t, std::unique_ptr<SrtpSession>> sessions_;

    // DTLS-derived keying material (master key + salt) — used to install
    // a new session per SSRC.
    std::vector<std::uint8_t>                outbound_key;   // master key for THIS side
    std::vector<std::uint8_t>                outbound_salt;
    std::vector<std::uint8_t>                inbound_key;    // master key for the OTHER side
    std::vector<std::uint8_t>                inbound_salt;

    SrtpSession* install(std::uint32_t ssrc, bool outgoing) {
        ensure_srtp_init();

        auto& slot = sessions_[ssrc];
        if (slot) return slot.get();

        auto sess = std::make_unique<SrtpSession>();
        srtp::Config cfg;
        cfg.suite = default_suite;
        cfg.enable_auth = true;
        cfg.enable_encryption = true;

        const auto& key = outgoing ? outbound_key : inbound_key;
        const auto& salt = outgoing ? outbound_salt : inbound_salt;
        if (key.empty() || salt.empty()) {
            core::log::Logger::instance().warn(
                "SRTP context: no key set, session creation will fail");
            return nullptr;
        }
        auto rc = sess->init_from_master_key(cfg, key, salt);
        if (!rc) {
            core::log::Logger::instance().error("SRTP install failed for SSRC");
            sessions_.erase(ssrc);
            return nullptr;
        }
        slot = std::move(sess);
        return slot.get();
    }

    void derive_keys_for_remote(std::span<const std::uint8_t> master_key,
                               std::span<const std::uint8_t> master_salt,
                               CryptoSuite suite) {
        default_suite = suite;
        // The remote peer's keys are used for INBOUND (decryption of peer's SRTP).
        inbound_key.assign(master_key.begin(), master_key.end());
        inbound_salt.assign(master_salt.begin(), master_salt.end());
        core::log::Logger::instance().info(
            "SRTP context: derived INBOUND keys (suite=" +
            std::to_string(static_cast<int>(suite)) + ")");
    }

    void derive_keys_for_local(std::span<const std::uint8_t> master_key,
                               std::span<const std::uint8_t> master_salt,
                               CryptoSuite suite) {
        default_suite = suite;
        // NimRTC's own server keys are used for OUTBOUND (encryption to peer).
        outbound_key.assign(master_key.begin(), master_key.end());
        outbound_salt.assign(master_salt.begin(), master_salt.end());
        core::log::Logger::instance().info(
            "SRTP context: derived OUTBOUND keys (suite=" +
            std::to_string(static_cast<int>(suite)) + ")");
    }
};

SrtpContext::SrtpContext() : impl_(std::make_unique<Impl>()) {}
SrtpContext::~SrtpContext() = default;

void SrtpContext::derive_keys_for_remote(
        std::span<const std::uint8_t> srtp_master_key,
        std::span<const std::uint8_t> srtp_master_salt,
        CryptoSuite suite) {
    impl_->derive_keys_for_remote(srtp_master_key, srtp_master_salt, suite);
}

void SrtpContext::derive_keys_for_local(
        std::span<const std::uint8_t> srtp_master_key,
        std::span<const std::uint8_t> srtp_master_salt,
        CryptoSuite suite) {
    impl_->derive_keys_for_local(srtp_master_key, srtp_master_salt, suite);
}

SrtpSession* SrtpContext::get_session(std::uint32_t ssrc, bool outgoing) {
    if (auto* sess = impl_->install(ssrc, outgoing)) {
        return sess;
    }
    return nullptr;
}

void SrtpContext::remove_session(std::uint32_t ssrc) {
    impl_->sessions_.erase(ssrc);
}

SrtpSession::Stats SrtpContext::total_stats() const {
    SrtpSession::Stats total{};
    for (const auto& [ssrc, session] : impl_->sessions_) {
        auto s = session->stats();
        total.rtp_packets_encrypted += s.rtp_packets_encrypted;
        total.rtp_packets_decrypted += s.rtp_packets_decrypted;
        total.rtcp_packets_encrypted += s.rtcp_packets_encrypted;
        total.rtcp_packets_decrypted += s.rtcp_packets_decrypted;
        total.decryption_failures    += s.decryption_failures;
        total.replay_attacks_dropped += s.replay_attacks_dropped;
    }
    return total;
}

} // namespace nimrtc::srtp
