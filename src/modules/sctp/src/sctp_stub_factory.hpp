/**
 * @file src/sctp/src/sctp_stub_factory.hpp
 * @brief SctpStubFactory — registration id "stub" (NOT "usrsctp").
 *
 * Stub impl pending v0.11.0 usrsctp integration per
 * `docs/plan/transport-selection.md` §7.
 *
 * ## Why the id is "stub", not "usrsctp"
 *
 * Per the Slice 5 spec, the registration id MUST be "stub" (and NOT
 * "usrsctp") until the production backend lands. This is a deliberate
 * signal to consumers and tests:
 *
 *   - Anyone doing `PluginRegistry::get_sctp_socket("usrsctp")` will get
 *     `nullptr` until v0.11.0 — making it obvious that the production
 *     backend has not yet been wired in, rather than silently resolving
 *     a non-functional stub.
 *   - Anyone doing `get_sctp_socket("stub")` will get the honest
 *     `SctpStubSocket` (sends return kErrNotReady).
 *
 * This avoids the false impression that the v0.11.0 usrsctp default is
 * already integrated in v0.10.x — see the Slice 5 DoD gate "Registration
 * id is 'stub' — explicit, not 'usrsctp'".
 *
 * ## v0.11.0 migration note
 *
 * When `UsrsctpSocketFactory` lands, it MUST register under id "usrsctp"
 * (not overwrite "stub"). Both factories coexist; the engine / Profile
 * resolver picks "usrsctp" explicitly when ready. The stub stays around
 * for tests and downgrade scenarios — see transport-selection §8 #7.
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
        // Explicit "stub", NOT "usrsctp" — see file header rationale.
        return "stub";
    }

    std::string_view display_name() const noexcept override {
        return "SCTP (stub; pending v0.11.0 usrsctp integration)";
    }

    std::unique_ptr<ISctpSocket> create(const SctpConfig& /*cfg*/) const override {
        // Config fields are ignored in Slice 5 — the stub has no backend
        // state to configure. v0.11.0 will route cfg.max_num_streams,
        // cfg.local_port, etc. into the usrsctp socket setup.
        return std::make_unique<SctpStubSocket>();
    }
};

} // namespace nimrtc::sctp

#endif // NIMRTC_SCTP_SRC_SCTP_STUB_FACTORY_HPP
