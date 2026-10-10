/**
 * @file nimrtc/sctp/usrsctp_factory.hpp
 * @brief UsrsctpSocketFactory — registration id "usrsctp".
 *
 * @deprecated v0.11.0 — DataChannel interop is **not** part of the
 * v0.11.0 release surface. The usrsctp backend is built into
 * `nimrtc_sctp.lib` and the factory class still compiles for opt-in
 * callers, but the production data plane will be redesigned on top
 * of **WebTransport over QUIC** in the v1.x series. See
 * `docs/plan/v0.11-plan.md` §2.2 (DC-1 / DC-2 / TPAL-5 deferred) and
 * the v1.x plan placeholder section. Expected removal: v1.2.0.
 *
 * Sibling to the existing `SctpStubFactory` (id="stub", v0.10.x
 * default). v0.11.0 ships with stub-only registration; the
 * `register_default_plugins()` body in `sctp_plugin.cpp` no longer
 * calls `register_sctp_socket("usrsctp", ...)` so the global
 * `core::PluginRegistry` resolves `get_sctp_socket("usrsctp")` to
 * nullptr. Production engines that want a real SCTP socket today
 * must construct the factory directly:
 *
 * ```cpp
 * #include <nimrtc/sctp/usrsctp_factory.hpp>
 * nimrtc::sctp::UsrsctpSocketFactory factory;
 * auto sock = factory.create(cfg);
 * // sock is non-null only if the opt-in build is linked.
 * ```
 *
 * The class is retained (not deleted) so:
 *   1. Existing test code that constructs `UsrsctpSocket` directly
 *      continues to compile and link.
 *   2. Out-of-tree integrators who already wired the production
 *      backend do not see a hard ABI break at v0.11.0 tag-cut.
 *   3. The v1.x WebTransport / QUIC work can compare the two
 *      approaches side-by-side without having to revive the
 *      upstream fork.
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
