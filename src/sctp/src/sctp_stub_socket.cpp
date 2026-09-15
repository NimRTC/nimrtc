/**
 * @file src/sctp/src/sctp_stub_socket.cpp
 * @brief SctpStubSocket — honest "not ready" implementation of ISctpSocket.
 *
 * Stub impl pending v0.11.0 usrsctp integration per
 * `docs/plan/transport-selection.md` §7. Every send method returns
 * `plugins::kErrNotReady` so callers can detect the missing backend
 * without silent data loss.
 */

#include "sctp_stub_socket.hpp"

#include <nimrtc/core/log.hpp>

namespace nimrtc::sctp {

plugins::Status SctpStubSocket::send_datagram(std::uint16_t stream,
                                              plugins::BufferView data) noexcept {
    (void)stream;
    (void)data;
    // Log at debug level so noisy tests don't spam stdout; the engine's
    // first call already proves the seam compiles and routes correctly.
    core::log::Logger::instance().debug(
        "nimrtc::sctp::SctpStubSocket::send_datagram: not ready (Slice 5 stub); "
        "v0.11.0 usrsctp integration pending");
    return plugins::kErrNotReady;
}

plugins::Status SctpStubSocket::send_stream(std::uint16_t stream,
                                            plugins::BufferView data) noexcept {
    (void)stream;
    (void)data;
    core::log::Logger::instance().debug(
        "nimrtc::sctp::SctpStubSocket::send_stream: not ready (Slice 5 stub); "
        "v0.11.0 usrsctp integration pending");
    return plugins::kErrNotReady;
}

plugins::Status SctpStubSocket::send_partial_reliable(
    std::uint16_t stream,
    plugins::BufferView data,
    std::chrono::milliseconds ttl) noexcept {
    (void)stream;
    (void)data;
    (void)ttl;
    core::log::Logger::instance().debug(
        "nimrtc::sctp::SctpStubSocket::send_partial_reliable: not ready "
        "(Slice 5 stub); v0.11.0 usrsctp integration pending");
    return plugins::kErrNotReady;
}

void SctpStubSocket::set_on_recv(plugins::OnSctpRecvCb cb) noexcept {
    on_recv_ = std::move(cb);
}

} // namespace nimrtc::sctp
