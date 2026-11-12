/**
 * @file src/modules/srtp/src/srtp.cpp
 * @brief SRTP — the engine-facing session/context, over the pluggable
 *        crypto-backend seam.
 *
 * ## Suite mapping (libsrtp enums)
 *
 *   SrtpSuite::Aes128CmSha1_80  ↔ srtp_profile_aes128_cm_sha1_80
 *   SrtpSuite::Aes128CmSha1_32  ↔ srtp_profile_aes128_cm_sha1_32
 *   SrtpSuite::Aes128Gcm_128    ↔ srtp_profile_aead_aes_128_gcm
 *   SrtpSuite::Aes256CmSha1_80  ↔ srtp_profile_aes256_cm_sha1_80
 *
 * ## Where the crypto lives
 *
 * Since v1.2.0 (architecture remediation plan B7 / Q3) this file does
 * **not** call libsrtp directly.  Every protect / unprotect goes
 * through a `plugins::ISrtpSessionBackend` instance, and the built-in
 * implementation of that seam is `LibsrtpSessionBackend`
 * (`libsrtp_backend.cpp`).  A third-party backend registers under its
 * own id and the engine selects it through
 * `EngineConfig::srtp_backend_name`.
 *
 * `to_libsrtp_profile()` stays here because it is a public API that
 * names a profile id; the mapping itself is the only libsrtp-specific
 * knowledge this file keeps.
 *
 * ## Threading
 *
 * SrtpSession is NOT thread-safe — same as the rest of the engine.
 */

#include <nimrtc/srtp/srtp.hpp>

#include <cstring>
#include <unordered_map>

#include <srtp.h>

#include <nimrtc/core/error.hpp>
#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/srtp/libsrtp_backend.hpp>

namespace nimrtc::srtp {

namespace {

// libsrtp profile mapping.
//
// The crypto itself now lives in `libsrtp_backend.cpp` (the
// `ISrtpSessionBackend` adapter); this file only speaks the seam.  The
// profile mapping is kept here because `to_libsrtp_profile()` is a
// public API that callers use to name a profile id.
srtp_profile_t to_lib_profile(CryptoSuite suite) noexcept {
    switch (suite) {
        case CryptoSuite::Aes128CmSha1_80: return srtp_profile_aes128_cm_sha1_80;
        case CryptoSuite::Aes128CmSha1_32: return srtp_profile_aes128_cm_sha1_32;
        case CryptoSuite::Aes128Gcm_128:   return srtp_profile_aead_aes_128_gcm;
        case CryptoSuite::Aes256CmSha1_80: return srtp_profile_aes256_cm_sha1_80;
    }
    return srtp_profile_aes128_cm_sha1_80;
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

    // The pluggable crypto backend (v1.2.0, remediation plan B7 / Q3).
    // Every protect / unprotect goes through this seam; the libsrtp
    // specifics live inside `LibsrtpSessionBackend`.
    std::unique_ptr<plugins::ISrtpSessionBackend> backend;
    std::string                               backend_id;

    std::uint32_t                           bound_ssrc = 0;
    bool                                    is_outbound = true;
    Stats                                   stats_{};

    // Working buffer for protect (must hold plaintext + auth tag).
    std::vector<std::uint8_t>               work_buf_;
};

// ---------------------------------------------------------------------------
// Backend resolution
// ---------------------------------------------------------------------------
//
// The backend id is resolved through `core::PluginRegistry` — the same
// registry every other plugin family uses.  A third-party backend
// registers with `NIMRTC_REGISTER_SRTP_BACKEND(id, &factory)` (or
// `PluginRegistry::register_srtp_backend(id, &factory)`) and is picked
// up by the engine through `EngineConfig::srtp_backend_name`.
//
// ## The built-in backend is always reachable
//
// The *default* id ("libsrtp") names the backend that ships with the
// module, so it must work even when nobody has called
// `core::register_all_default_plugins()` — a module-level consumer
// that constructs an `SrtpContext` directly (the SRTP unit tests, the
// DTLS-over-ARQ integration tests) never goes through the engine and
// therefore never runs the registrar table.
//
// So the resolution has two tiers:
//
//   1. The registry, if it has an entry for the id.  A third party
//      that registers "libsrtp" wins over the built-in — the registry
//      is the override point.
//   2. Otherwise, the built-in `LibsrtpSessionBackend`, but ONLY for
//      the id "libsrtp".  Any other unregistered id is an error.
//
// There is NO fallback for an *unknown* id: substituting a different
// crypto backend behind the user's back would be a security bug.  The
// engine turns that into a hard `open()` failure.
namespace {

/** The id of the backend that ships with the module. */
constexpr const char* kBuiltinBackendId = "libsrtp";

std::unique_ptr<plugins::ISrtpSessionBackend> make_backend(
        const std::string& backend_id) {
    if (backend_id.empty()) return nullptr;

    auto& reg = nimrtc::core::PluginRegistry::instance();
    const plugins::ISrtpBackendFactory* factory =
        reg.get_srtp_backend(backend_id);
    if (factory) {
        auto backend = factory->create();
        if (!backend) {
            core::log::Logger::instance().error(
                "SRTP: crypto backend '" + backend_id + "' failed to create a session");
        }
        return backend;
    }

    // No registry entry.  The built-in id still resolves — the module
    // owns that implementation, and the caller may never have run the
    // registrar table.
    if (backend_id == kBuiltinBackendId) {
        static const LibsrtpBackendFactory builtin{};
        return builtin.create();
    }

    core::log::Logger::instance().error(
        "SRTP: no crypto backend registered under id '" + backend_id + "'");
    return nullptr;
}

/** Map the module's `CryptoSuite` onto the seam's suite id.  The two
 *  enums share their ordering (see `plugins::SrtpCryptoSuite`). */
std::uint32_t seam_suite_id(CryptoSuite suite) noexcept {
    return static_cast<std::uint32_t>(suite);
}

} // anonymous namespace

SrtpSession::SrtpSession() : impl_(std::make_unique<Impl>()) {
    impl_->backend_id = "libsrtp";
    impl_->backend = make_backend(impl_->backend_id);
}

SrtpSession::SrtpSession(std::string backend_id)
    : impl_(std::make_unique<Impl>()) {
    impl_->backend_id = std::move(backend_id);
    impl_->backend = make_backend(impl_->backend_id);
}

SrtpSession::SrtpSession(std::unique_ptr<plugins::ISrtpSessionBackend> backend)
    : impl_(std::make_unique<Impl>()) {
    impl_->backend = std::move(backend);
    impl_->backend_id = impl_->backend ? "custom" : "";
}

SrtpSession::~SrtpSession() = default;
SrtpSession::SrtpSession(SrtpSession&&) noexcept = default;
SrtpSession& SrtpSession::operator=(SrtpSession&&) noexcept = default;

std::string_view SrtpSession::backend_id() const noexcept {
    return impl_->backend_id;
}

core::Result<void> SrtpSession::init_from_master_key(
        const Config& config,
        std::span<const std::uint8_t> master_key,
        std::span<const std::uint8_t> master_salt) {
    impl_->config = config;
    if (master_key.empty() || master_salt.empty()) {
        return core::Result<void>::fail(
            core::ErrorCode::InvalidArgument, "SRTP: empty key/salt");
    }
    if (!impl_->backend) {
        return core::Result<void>::fail(
            core::ErrorCode::InvalidState,
            "SRTP: no crypto backend available for session");
    }
    auto rc = impl_->backend->init_outbound(
        master_key, master_salt, seam_suite_id(config.suite));
    if (!rc) {
        return core::Result<void>::fail(
            core::ErrorCode::CryptoError,
            "SRTP: crypto backend failed to install the outbound key (" +
                rc.error().message() + ")");
    }
    impl_->is_outbound = true;
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
    if (!impl_->backend) {
        return core::Result<void>::fail(
            core::ErrorCode::InvalidState,
            "SRTP: no crypto backend available for session");
    }
    auto rc = impl_->backend->init_inbound(
        master_key, master_salt, seam_suite_id(config.suite));
    if (!rc) {
        return core::Result<void>::fail(
            core::ErrorCode::CryptoError,
            "SRTP: crypto backend failed to install the inbound key (" +
                rc.error().message() + ")");
    }
    impl_->is_outbound = false;
    return core::Result<void>::make_ok();
}

core::Result<void> SrtpSession::init_from_session_keys(const Config& config) {
    impl_->config = config;
    if (config.session_key_rtp.empty() || config.session_salt_rtp.empty()) {
        return core::Result<void>::fail(
            core::ErrorCode::InvalidArgument, "SRTP: empty session keys");
    }
    if (!impl_->backend) {
        return core::Result<void>::fail(
            core::ErrorCode::InvalidState,
            "SRTP: no crypto backend available for session");
    }
    auto rc = impl_->backend->init_outbound_session_keys(
        config.session_key_rtp, config.session_salt_rtp,
        seam_suite_id(config.suite));
    if (!rc) {
        // A backend that cannot install session keys (the default seam
        // implementation returns NotImplemented) is not an error — the
        // caller falls back to the master-key path.
        return core::Result<void>::fail(
            rc.error().code(), rc.error().message());
    }
    return core::Result<void>::make_ok();
}

core::Result<core::ByteSpan> SrtpSession::protect_rtp(
        core::ByteSpan header_and_plaintext,
        std::uint32_t /*ssrc*/,
        std::uint32_t /*rtp_timestamp*/) {
    if (!impl_->backend) {
        return core::Result<core::ByteSpan>::fail(
            core::ErrorCode::InvalidState, "SRTP: session not initialised");
    }
    auto rc = impl_->backend->protect_rtp(header_and_plaintext);
    if (!rc) {
        ++impl_->stats_.decryption_failures;
        return core::Result<core::ByteSpan>::fail(
            core::ErrorCode::ProtocolError,
            "SRTP: protect_rtp failed (" + rc.error().message() + ")");
    }
    ++impl_->stats_.rtp_packets_encrypted;
    return rc;
}

core::Result<core::ByteSpan> SrtpSession::unprotect_rtp(
        core::ByteSpan srtp_packet,
        std::uint32_t* out_ssrc,
        std::uint32_t* out_timestamp) {
    if (!impl_->backend) {
        return core::Result<core::ByteSpan>::fail(
            core::ErrorCode::InvalidState, "SRTP: session not initialised");
    }
    if (srtp_packet.size() < 12) {
        ++impl_->stats_.decryption_failures;
        return core::Result<core::ByteSpan>::fail(
            core::ErrorCode::ProtocolError, "SRTP: packet too short");
    }
    // The SSRC / timestamp read-out happens BEFORE the unprotect so the
    // caller still sees them when the auth tag check fails (the caller
    // uses them to demux the packet to the right session).
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
    auto rc = impl_->backend->unprotect_rtp(srtp_packet);
    if (!rc) {
        ++impl_->stats_.decryption_failures;
        return core::Result<core::ByteSpan>::fail(
            core::ErrorCode::ProtocolError,
            "SRTP: unprotect_rtp failed (" + rc.error().message() + ")");
    }
    ++impl_->stats_.rtp_packets_decrypted;
    return rc;
}

core::Result<core::ByteSpan> SrtpSession::protect_rtcp(core::ByteSpan rtcp_packet) {
    if (!impl_->backend) {
        return core::Result<core::ByteSpan>::ok(rtcp_packet);
    }
    auto rc = impl_->backend->protect_rtcp(rtcp_packet);
    if (!rc) {
        return core::Result<core::ByteSpan>::fail(
            core::ErrorCode::ProtocolError,
            "SRTP: protect_rtcp failed (" + rc.error().message() + ")");
    }
    ++impl_->stats_.rtcp_packets_encrypted;
    return rc;
}

core::Result<core::ByteSpan> SrtpSession::unprotect_rtcp(core::ByteSpan srtcp_packet) {
    if (!impl_->backend) {
        return core::Result<core::ByteSpan>::ok(srtcp_packet);
    }
    auto rc = impl_->backend->unprotect_rtcp(srtcp_packet);
    if (!rc) {
        return core::Result<core::ByteSpan>::fail(
            core::ErrorCode::ProtocolError,
            "SRTP: unprotect_rtcp failed (" + rc.error().message() + ")");
    }
    ++impl_->stats_.rtcp_packets_decrypted;
    return rc;
}

std::uint32_t SrtpSession::current_roc() const {
    if (!impl_->backend) return 0;
    return impl_->backend->current_roc();
}
SrtpSession::Stats SrtpSession::stats() const { return impl_->stats_; }
void SrtpSession::reset() {
    impl_->stats_ = Stats{};
    if (impl_->backend) impl_->backend->reset();
}

// ---------------------------------------------------------------------------
// SrtpContext::Impl
// ---------------------------------------------------------------------------

struct SrtpContext::Impl {
    CryptoSuite                              default_suite = CryptoSuite::Aes128CmSha1_80;
    std::unordered_map<std::uint32_t, std::unique_ptr<SrtpSession>> outbound_sessions_;
    std::unordered_map<std::uint32_t, std::unique_ptr<SrtpSession>> inbound_sessions_;

    // The crypto-backend id every session of this context uses
    // (v1.2.0, remediation plan B7 / Q3).  Resolved through the
    // registry when the first session is created.
    std::string                               backend_id = "libsrtp";

    // DTLS-derived keying material (master key + salt) — used to install
    // a new session per SSRC.
    std::vector<std::uint8_t>                outbound_key;   // master key for THIS side
    std::vector<std::uint8_t>                outbound_salt;
    std::vector<std::uint8_t>                inbound_key;    // master key for the OTHER side
    std::vector<std::uint8_t>                inbound_salt;

    SrtpSession* install(std::uint32_t ssrc, bool outgoing) {
        // Separate the inbound and outbound slot maps.  Sharing one map
        // keyed only by SSRC is wrong: a side that has *received* an
        // RTP packet from the peer (creating an inbound session for
        // ssrc=X) and then needs to *send* its own RTP traffic on the
        // same ssrc=X would otherwise get back the inbound session,
        // and the protect call on an inbound-only session fails because
        // the crypto context was installed with the inbound polarity.
        // In a WebRTC PeerConnection this happens on both sides every
        // time the same SSRC is used for send AND receive — e.g.
        // when audio RTCP reports arrive on the same SSRC as audio
        // data, or in tests where the test thread and the recv thread
        // race to install the session for the same SSRC.
        auto& slot = outgoing ? outbound_sessions_[ssrc]
                              : inbound_sessions_[ssrc];
        if (slot) return slot.get();

        auto sess = std::make_unique<SrtpSession>(backend_id);
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
        // CRITICAL: the OUTBOUND and INBOUND installs use *different*
        // polarities.  Calling the outbound variant when the caller
        // asked for an INBOUND session produces a session whose
        // decryption direction is wrong, and the peer's
        // outbound-encrypted packet is then rejected — the failure
        // surfaces as an auth/replay failure and the test prints
        // "SRTP unprotect failed".  This was masked for a long time
        // because the SRTP master key was all zeros (the wolfSSL
        // KeepArrays bug we fixed earlier), which made every direction
        // symmetric and the wrong polarity invisible.
        core::Result<void> rc = outgoing
            ? sess->init_from_master_key(cfg, key, salt)
            : sess->init_from_master_key_inbound(cfg, key, salt);
        if (!rc) {
            core::log::Logger::instance().error(
                "SRTP install failed for SSRC: " + rc.error().message());
            (outgoing ? outbound_sessions_ : inbound_sessions_).erase(ssrc);
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

SrtpContext::SrtpContext(std::string backend_id)
    : impl_(std::make_unique<Impl>()) {
    impl_->backend_id = std::move(backend_id);
}

std::string_view SrtpContext::backend_id() const noexcept {
    return impl_->backend_id;
}

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
    impl_->outbound_sessions_.erase(ssrc);
    impl_->inbound_sessions_.erase(ssrc);
}

SrtpSession::Stats SrtpContext::total_stats() const {
    SrtpSession::Stats total{};
    auto accumulate = [&](const std::unordered_map<std::uint32_t,
                              std::unique_ptr<SrtpSession>>& map) {
        for (const auto& [ssrc, session] : map) {
            auto s = session->stats();
            total.rtp_packets_encrypted += s.rtp_packets_encrypted;
            total.rtp_packets_decrypted += s.rtp_packets_decrypted;
            total.rtcp_packets_encrypted += s.rtcp_packets_encrypted;
            total.rtcp_packets_decrypted += s.rtcp_packets_decrypted;
            total.decryption_failures    += s.decryption_failures;
            total.replay_attacks_dropped += s.replay_attacks_dropped;
        }
    };
    accumulate(impl_->outbound_sessions_);
    accumulate(impl_->inbound_sessions_);
    return total;
}

} // namespace nimrtc::srtp
