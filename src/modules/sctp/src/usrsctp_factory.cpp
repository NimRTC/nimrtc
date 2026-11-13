/**
 * @file src/sctp/src/usrsctp_factory.cpp
 * @brief UsrsctpSocketFactory — registration id "usrsctp" (v0.11.0
 *        production backend).
 *
 * All behaviour lives in the header; this .cpp exists so the static
 * lib carries the symbol (same pattern as `SctpStubFactory`'s
 * `sctp_stub_factory.cpp` and `nimrtc::audio3a::NullPluginFactory`).
 */

#include <nimrtc/sctp/usrsctp_factory.hpp>
#include <nimrtc/sctp/usrsctp_socket.hpp>

namespace nimrtc::sctp {

std::unique_ptr<ISctpSocket>
UsrsctpSocketFactory::create(const SctpConfig& cfg) const {
    // std::make_unique would force a public ctor on UsrsctpSocket;
    // we own the ctor (it's only meant to be invoked via the factory)
    // so we use new + unique_ptr ctor.
    return std::unique_ptr<ISctpSocket>(new UsrsctpSocket(cfg));
}

} // namespace nimrtc::sctp
