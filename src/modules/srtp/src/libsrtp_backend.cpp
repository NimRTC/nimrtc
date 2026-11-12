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
#include <string>

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

// The seam suite id -> libsrtp profile.
//
// The input is a `plugins::SrtpCryptoSuite` id (0..3), NOT a libsrtp
// `srtp_profile_t`.  The two numbering schemes differ:
//
//     seam id 0 (AES-128-CM-SHA1-80) -> libsrtp profile id 2
//     seam id 1 (AES-128-CM-SHA1-32) -> libsrtp profile id 3
//     seam id 2 (AES-128-GCM)        -> libsrtp profile id 10
//     seam id 3 (AES-256-CM-SHA1-80) -> libsrtp profile id 6
//
// An out-of-range id is REJECTED rather than defaulted.  The old
// `default:` branch mapped every unknown id onto AES-128-CM-SHA1-80,
// which meant a caller that (wrongly) passed a libsrtp profile id got
// a *different* protection profile instead of an error -- and the
// wrong auth tag length with it.  A silently wrong crypto profile is
// worse than a refused session.
bool suite_id_is_valid(std::uint32_t suite_id) noexcept {
    return suite_id <= 3u;
}

// The vendored libsrtp is built against its "internal" crypto backend,
// which provides AES-CM + HMAC-SHA1 but NOT AES-GCM.  The suite id for
// AES-128-GCM is therefore *recognised* but not *installable*: the
// profile is a valid seam value, yet libsrtp cannot create a context
// for it.  Refusing it here turns a generic "srtp_create failed" into
// a message that says what is actually wrong.
bool suite_is_supported(std::uint32_t suite_id) noexcept {
    if (suite_id == 2u) {   // kAes128Gcm_128
#if defined(NIMRTC_LIBSRTP_HAS_GCM) && (NIMRTC_LIBSRTP_HAS_GCM == 0)
        return false;
#else
        return true;
#endif
    }
    return suite_id_is_valid(suite_id);
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

/// The size of the SRTCP index trailer (RFC 3711 §3.4): a 31-bit
/// packet index plus one "encrypted" bit, i.e. exactly 4 bytes.
constexpr std::size_t kSrtcpIndexTrailerLen = 4;

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

    if (!suite_id_is_valid(suite_id)) {
        // The caller passed something that is not a seam suite id --
        // most likely a libsrtp `srtp_profile_t` (2, 3, 6 or 10), which
        // happens to be in range for ids 2 and 3 and would otherwise be
        // silently mapped onto the WRONG protection profile.
        //
        // Refuse the session instead.  A caller that cannot name a
        // suite must not get a session with an unexpected profile.
        return core::Result<void>::fail(
            core::ErrorCode::InvalidArgument,
            "SRTP backend: invalid suite id " + std::to_string(suite_id) +
                " (expected a plugins::SrtpCryptoSuite value 0..3)");
    }

    if (!suite_is_supported(suite_id)) {
        // A valid seam value that this libsrtp build cannot provide.
        return core::Result<void>::fail(
            core::ErrorCode::NotImplemented,
            "SRTP backend: suite id " + std::to_string(suite_id) +
                " (AES-128-GCM) is not supported by this libsrtp build "
                "(the vendored build ships AES-CM only)");
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

    // Phase 3 / C5: grow the scratch to a high-water mark and never
    // shrink it.  The old code did `resize(out_len)` before the call
    // and `resize(actual_len)` after it, so the vector's size tracked
    // the *last* packet.  The next packet with a different size then
    // grew the vector again — and `std::vector::resize()` value-
    // initialises the newly added bytes, so every size change cost a
    // memset of the whole difference.  Keeping the size at the
    // high-water mark makes the steady-state path allocation-free and
    // memset-free.
    //
    // Only the vector's *storage* matters to libsrtp: `data()` plus the
    // explicit `out_len` the call writes back.
    if (work_buf_.size() < out_len) {
        work_buf_.resize(out_len);
    }

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
    return core::Result<core::ByteSpan>::ok(
        core::ByteSpan(work_buf_.data(), out_len));
}

core::Result<core::ByteSpan> LibsrtpSessionBackend::unprotect_rtp(
    core::ByteSpan srtp_packet) noexcept {
    if (!session_) {
        return core::Result<core::ByteSpan>::fail(
            core::ErrorCode::InvalidState, "SRTP backend: not initialised");
    }
    auto* ctx = static_cast<srtp_t>(session_);

    // Phase 3 / C5: same high-water-mark scratch policy as protect_rtp().
    // `out_len` is the *available* capacity handed to libsrtp; libsrtp
    // writes the actual plaintext length back into it.
    if (out_buf_.size() < srtp_packet.size()) {
        out_buf_.resize(srtp_packet.size());
    }
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
    return core::Result<core::ByteSpan>::ok(
        core::ByteSpan(out_buf_.data(), out_len));
}

core::Result<core::ByteSpan> LibsrtpSessionBackend::protect_rtcp(
    core::ByteSpan rtcp_packet) noexcept {
    if (!session_) {
        return core::Result<core::ByteSpan>::ok(rtcp_packet);
    }
    auto* ctx = static_cast<srtp_t>(session_);

    // The SRTCP output is: the RTCP packet, then a 4-byte SRTCP index
    // trailer (the 31-bit index + the E bit), then the authentication
    // tag.  libsrtp checks the buffer size against that total and
    // returns `srtp_err_status_buffer_small` if it is short -- so the
    // trailer must be accounted for here, not just the tag.
    //
    // The old sizing was `rtcp.size() + tag_len`, which was 4 bytes
    // short: every SRTCP protect failed.
    std::size_t out_len = rtcp_packet.size() + kSrtcpIndexTrailerLen +
                          auth_tag_len_for(suite_id_);
    if (work_buf_.size() < out_len) {
        work_buf_.resize(out_len);
    }
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
    return core::Result<core::ByteSpan>::ok(
        core::ByteSpan(work_buf_.data(), out_len));
}

core::Result<core::ByteSpan> LibsrtpSessionBackend::unprotect_rtcp(
    core::ByteSpan srtcp_packet) noexcept {
    if (!session_) {
        return core::Result<core::ByteSpan>::ok(srtcp_packet);
    }
    auto* ctx = static_cast<srtp_t>(session_);
    if (out_buf_.size() < srtcp_packet.size()) {
        out_buf_.resize(srtcp_packet.size());
    }
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
    return core::Result<core::ByteSpan>::ok(
        core::ByteSpan(out_buf_.data(), out_len));
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
