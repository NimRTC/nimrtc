/**
 * @file src/sctp/src/sctp_stub_socket.hpp
 * @brief SctpStubSocket — honest "not ready" implementation of ISctpSocket.
 *
 * Stub impl pending v0.11.0 usrsctp integration per
 * `docs/plan/transport-selection.md` §7.
 *
 * The stub does NOT silently drop outbound messages. Every send method
 * returns `plugins::kErrNotReady` so the engine / tests can detect that
 * the seam compiles but no real backend is wired in — failing loud beats
 * silent loss for control-channel traffic.
 *
 * The on-recv callback is stored (so tests can verify registration works)
 * but is never invoked — there is no transport behind the seam yet.
 */

#ifndef NIMRTC_SCTP_SRC_SCTP_STUB_SOCKET_HPP
#define NIMRTC_SCTP_SRC_SCTP_STUB_SOCKET_HPP

#include <nimrtc/sctp/sctp_socket_iface.hpp>

namespace nimrtc::sctp {

class SctpStubSocket final : public ISctpSocket {
public:
    SctpStubSocket() = default;
    ~SctpStubSocket() override = default;

    SctpStubSocket(const SctpStubSocket&)            = delete;
    SctpStubSocket& operator=(const SctpStubSocket&) = delete;

    // ---- ISctpSocket -----------------------------------------------------

    plugins::Status send_datagram(std::uint16_t stream,
                                  plugins::BufferView data) noexcept override;

    plugins::Status send_stream(std::uint16_t stream,
                                plugins::BufferView data) noexcept override;

    plugins::Status send_partial_reliable(
        std::uint16_t stream,
        plugins::BufferView data,
        std::chrono::milliseconds ttl) noexcept override;

    void set_on_recv(plugins::OnSctpRecvCb cb) noexcept override;

    // ---- Test helpers ----------------------------------------------------

    /** Returns true if a non-empty recv callback has been registered.
     *  Used by tests/test_sctp_factory.cpp to verify registration works. */
    bool has_recv_callback() const noexcept { return static_cast<bool>(on_recv_); }

private:
    plugins::OnSctpRecvCb on_recv_;
};

} // namespace nimrtc::sctp

#endif // NIMRTC_SCTP_SRC_SCTP_STUB_SOCKET_HPP
