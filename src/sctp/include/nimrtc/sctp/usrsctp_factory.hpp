/**
 * @file nimrtc/sctp/usrsctp_factory.hpp
 * @brief UsrsctpSocketFactory — registration id "usrsctp" (v0.11.0
 *        production backend).
 *
 * Sibling to the existing `SctpStubFactory` (id="stub", v0.10.x
 * default). TPAL-5 (v0.11.0) registers this factory alongside the
 * stub — `nimrtc::sctp::register_default_plugins()` adds both slots
 * so v0.10.x callers that picked `id="stub"` continue to work.
 *
 * ## v0.11.0 selection semantics
 *
 * Per `docs/plan/transport-selection.md` §6.2 / §7, the production
 * default in v0.11.0 is `id="usrsctp"`. Callers that leave the
 * factory id unset (or rely on the registry's first-registered id)
 * get the stub under the current dual-registration scheme (because
 * `get_sctp_socket("stub")` and `get_sctp_socket("usrsctp")` both
 * resolve non-null, but a default-id resolver picks the first id
 * alphabetically — "stub"). A future v0.11.x follow-up will add a
 * typed resolver that defaults to "usrsctp" once both ids are
 * populated — see transport-selection §8 #7.
 *
 * @note P0 scaffold — interface stable; binary layout TBD P4.
 */

#ifndef NIMRTC_SCTP_USRSCTP_FACTORY_HPP
#define NIMRTC_SCTP_USRSCTP_FACTORY_HPP

#include <memory>
#include <string_view>

#include <nimrtc/sctp/sctp_socket_factory.hpp>

namespace nimrtc::sctp {

// Forward declaration — full type lives in usrsctp_socket.hpp.
class UsrsctpSocket;

class UsrsctpSocketFactory final : public ISctpSocketFactory {
public:
    UsrsctpSocketFactory() = default;
    ~UsrsctpSocketFactory() override = default;

    UsrsctpSocketFactory(const UsrsctpSocketFactory&)            = delete;
    UsrsctpSocketFactory& operator=(const UsrsctpSocketFactory&) = delete;

    // ---- ISctpSocketFactory ---------------------------------------------

    /** Registration id — MUST be the literal `"usrsctp"` so the engine /
     *  Profile loader can resolve the production backend explicitly
     *  (and tests can assert `get_sctp_socket("usrsctp") != nullptr`). */
    std::string_view id() const noexcept override { return "usrsctp"; }

    /** Short human-readable name, surfaced in logs / registry listings. */
    std::string_view display_name() const noexcept override {
        return "SCTP (usrsctp; userland SCTP stack 0.9.5.0)";
    }

    /** Build a new UsrsctpSocket configured from `cfg`. Ownership
     *  passes to the caller. Returns nullptr only on catastrophic
     *  OOM (the implementation always returns a valid socket; if
     *  usrsctp_socket() fails the socket is constructed but reports
     *  `is_ready() == false` and every send_* returns kErrInternal). */
    std::unique_ptr<ISctpSocket> create(const SctpConfig& cfg) const override;
};

} // namespace nimrtc::sctp

#endif // NIMRTC_SCTP_USRSCTP_FACTORY_HPP
