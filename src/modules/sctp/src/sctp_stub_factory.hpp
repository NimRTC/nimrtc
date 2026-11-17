/**
 * @file src/sctp/src/sctp_stub_factory.hpp
 * @brief SctpStubFactory — registration id "stub".
 *
 * The stub is the only registered SCTP backend since v1.2.0 (plan E7):
 * the usrsctp backend and its vendored tree were deleted, and the
 * production data plane is WebTransport over QUIC, which does not go
 * through the `ISctpSocket` seam at all.
 *
 * ## Why the id is "stub"
 *
 * The registration id is deliberately explicit: a consumer that resolves
 * id "stub" gets the honest `SctpStubSocket` (every send returns
 * `kErrNotReady`), so the failure is loud rather than silent.
 *
 * ## v0.11.0 note (historical)
 *
 * Per the Slice 5 spec the id had to be "stub" (and NOT "usrsctp") until
 * a production backend landed. That backend never became reachable — it
 * was removed in v1.2.0 — so "stub" stayed.
 */

#ifndef NIMRTC_SCTP_SRC_SCTP_STUB_FACTORY_HPP
#define NIMRTC_SCTP_SRC_SCTP_STUB_FACTORY_HPP

#include <memory>
#include <string_view>

#include <nimrtc/sctp/sctp_socket_factory.hpp>
#include "sctp_stub_socket.hpp"   // SctpStubSocket — make_unique target

namespace nimrtc::sctp {

class SctpStubFactory final : public ISctpSocketFactory {
public:
    SctpStubFactory() = default;
    ~SctpStubFactory() override = default;

    std::string_view id() const noexcept override {
        // Explicit "stub" — see file header rationale.
        return "stub";
    }

    std::string_view display_name() const noexcept override {
        return "SCTP (stub; no data plane)";
    }

    std::unique_ptr<ISctpSocket> create(const SctpConfig& /*cfg*/) const override {
        // Config fields are ignored — the stub has no backend
        // state to configure.
        return std::make_unique<SctpStubSocket>();
    }
};

} // namespace nimrtc::sctp

#endif // NIMRTC_SCTP_SRC_SCTP_STUB_FACTORY_HPP
