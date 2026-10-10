/**
 * @file src/modules/dtls/src/dtls_session.cpp
 * @brief DtlsSession — thin pImpl wrapper that delegates to DtlsSessionWolfSSL.
 *
 * As of the Linux-adaptation refactor the original hand-written DTLS state
 * machine (dtls.cpp) is gone and wolfSSL is the only DTLS provider.
 * `DtlsSession` is retained as the public name that engine.cpp / tests refer
 * to; this file provides the out-of-line definitions and forwards every
 * call to a heap-allocated `DtlsSessionWolfSSL`.
 *
 * Note: `dtls_wolfssl_session.cpp` defines `DtlsSessionWolfSSL::*` and the
 * `state_name(DtlsState)` symbol.  This file must NOT redefine `state_name`
 * independently — the static call resolves here to
 * `DtlsSession::state_name` (this file), which itself forwards to
 * `DtlsSessionWolfSSL::state_name` (the .cpp above).  The duplication of the
 * dispatch table is intentional: callers of `DtlsSession::state_name()`
 * stay decoupled from `DtlsSessionWolfSSL`.
 */

#include "nimrtc/dtls/dtls.hpp"
#include "nimrtc/dtls/dtls_wolfssl_session.hpp"

namespace nimrtc::dtls {

// ---------------------------------------------------------------------------
// DtlsSession::Impl — pImpl hiding DtlsSessionWolfSSL behind unique_ptr.
// ---------------------------------------------------------------------------
struct DtlsSession::Impl {
    DtlsSessionWolfSSL inner;
    explicit Impl(Config cfg) : inner(std::move(cfg)) {}
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
DtlsSession::DtlsSession(Config config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

DtlsSession::~DtlsSession() = default;
DtlsSession::DtlsSession(DtlsSession&&) noexcept = default;
DtlsSession& DtlsSession::operator=(DtlsSession&&) noexcept = default;

core::Result<void> DtlsSession::open() noexcept {
    return impl_->inner.open();
}

void DtlsSession::close() noexcept {
    impl_->inner.close();
}

// ---------------------------------------------------------------------------
// Wire I/O
// ---------------------------------------------------------------------------
std::size_t DtlsSession::feed_inbound(std::span<const std::uint8_t> bytes,
                                        const DtlsAddr& from) noexcept {
    return impl_->inner.feed_inbound(bytes, from);
}

std::vector<DtlsRecord> DtlsSession::take_outbound() noexcept {
    return impl_->inner.take_outbound();
}

void DtlsSession::tick() noexcept {
    impl_->inner.tick();
}

// ---------------------------------------------------------------------------
// Status / fingerprint / role / SRTP
// ---------------------------------------------------------------------------
DtlsState DtlsSession::state() const noexcept {
    return impl_->inner.state();
}

bool DtlsSession::is_connected() const noexcept {
    return impl_->inner.is_connected();
}

const char* DtlsSession::state_name(DtlsState s) noexcept {
    // Forward to the wolfSSL-backed implementation so there's only one
    // canonical state-name table.  If we ever support non-wolfSSL backends
    // again this can dispatch on a backend tag.
    return DtlsSessionWolfSSL::state_name(s);
}

const Fingerprint& DtlsSession::local_fingerprint() const noexcept {
    return impl_->inner.local_fingerprint();
}

void DtlsSession::set_peer_fingerprint(std::string algo,
                                        std::vector<std::uint8_t> value) noexcept {
    impl_->inner.set_peer_fingerprint(std::move(algo), std::move(value));
}

void DtlsSession::set_role(DtlsRole r) noexcept {
    impl_->inner.set_role(r);
}

std::optional<SrtpKeyingMaterial>
DtlsSession::srtp_keying_material() const noexcept {
    return impl_->inner.srtp_keying_material();
}

// ---------------------------------------------------------------------------
// PAL Slice 4 / TPAL-4 seam surface — thin adapters that delegate to the
// underlying DtlsSessionWolfSSL (which itself inherits IDtlsSession).
// ---------------------------------------------------------------------------
void DtlsSession::set_role(Role role) noexcept {
    impl_->inner.set_role(role);
}

void DtlsSession::set_peer_fingerprint(
    std::span<const std::uint8_t> raw_sha256) noexcept {
    std::vector<std::uint8_t> value(raw_sha256.begin(), raw_sha256.end());
    impl_->inner.set_peer_fingerprint("sha-256", std::move(value));
}

void DtlsSession::start() noexcept {
    (void)impl_->inner.open();
}

void DtlsSession::pump() noexcept {
    impl_->inner.tick();
}

void DtlsSession::on_handshake_complete(OnCompleteCb cb) noexcept {
    impl_->inner.on_handshake_complete(std::move(cb));
}

plugins::Status DtlsSession::export_srtp_key_material(
    std::span<std::uint8_t, 60> out) noexcept {
    auto km = impl_->inner.srtp_keying_material();
    if (!km.has_value()) return plugins::kErrNotReady;
    std::memcpy(out.data() +  0, km->client_master_key.data(),  16);
    std::memcpy(out.data() + 16, km->server_master_key.data(),  16);
    std::memcpy(out.data() + 32, km->client_master_salt.data(), 14);
    std::memcpy(out.data() + 46, km->server_master_salt.data(), 14);
    return plugins::kOk;
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------
DtlsSession::Stats DtlsSession::stats() const noexcept {
    const auto s = impl_->inner.stats();
    Stats out;
    out.records_in    = s.records_in;
    out.records_out   = s.records_out;
    out.handshake_ms  = s.handshake_ms;
    out.retransmits   = s.retransmits;
    out.alerts_in     = s.alerts_in;
    out.alerts_out    = s.alerts_out;
    out.errors        = s.errors;
    return out;
}

} // namespace nimrtc::dtls
