/**
 * @file nimrtc/web_transport/datagram_facade.hpp
 * @brief Datagram facade — convenience surface for WebTransport
 *        connection-level datagrams (v0.14.0 alpha.2, ADR-014).
 *
 * The datagram surface on `IWebTransportSession` is sufficient
 * for most use cases (`send_datagram` + `set_on_datagram`); the
 * facade here is a thin wrapper that adapts the session-level
 * surface to a `std::function`-based sink + a default
 * `max_datagram_frame_size` policy.
 *
 * ## Why a facade?
 *
 * The v0.14.0-beta.1 `WebTransportDataChannel` impl will use
 * datagrams as the data-plane wire-up (per
 * `docs/plan/v0.11-plan.md` §10). A consumer that needs to
 * listen to datagrams at the session level can either:
 *   (a) call `IWebTransportSession::set_on_datagram(cb)`
 *       directly, or
 *   (b) construct a `DatagramFacade` and have it own the
 *       callback registration.
 *
 * The facade pattern keeps the session surface minimal (one
 * callback, one send) while still allowing the data-channel
 * adapter to factor out the callback-management boilerplate.
 *
 * @note P0 — added as part of v0.14.0 alpha.2 (the
 *       v1.0.0-alpha.2 pre-release).  The facade is a
 *       non-virtual utility class; v1.x callers may use it
 *       as a stable convenience.
 */

#ifndef NIMRTC_WEB_TRANSPORT_DATAGRAM_FACADE_HPP
#define NIMRTC_WEB_TRANSPORT_DATAGRAM_FACADE_HPP

// Stability classification: NIMRTC_API_UNSTABLE — see
// web_transport_session_iface.hpp for the rationale.
#include <cstdint>
#include <functional>
#include <span>

#include <nimrtc/core/api_version.hpp>  // NIMRTC_API_UNSTABLE
#include <nimrtc/plugins/base.hpp>       // plugins::Status

// Forward declaration of the session type — the facade only
// needs to call into the seam surface; including the session
// header here would force every consumer of the facade to
// also pull in the (still-unimplemented) session surface.
namespace nimrtc::web_transport {
class IWebTransportSession;
} // namespace nimrtc::web_transport

namespace nimrtc::web_transport {

/**
 * @brief Datagram facade — utility wrapper around
 *        `IWebTransportSession`'s datagram surface.
 *
 * Constructed with a non-owning pointer to the session; the
 * facade does NOT extend the session's lifetime. Consumers
 * must guarantee the session outlives the facade.
 *
 * The facade is a non-virtual utility class — no inheritance
 * hierarchy, no factory. Callers construct it on the stack:
 *
 * @code
 *   DatagramFacade dg(&session);
 *   dg.set_sink([](std::span<const std::uint8_t> bytes) {
 *       // handle received datagram
 *   });
 *   dg.send(my_bytes);
 * @endcode
 */
class NIMRTC_API_UNSTABLE DatagramFacade {
public:
    /**
     * @brief Construct a facade bound to `session`.
     *
     * `session` must be non-null and outlive the facade.
     */
    explicit DatagramFacade(IWebTransportSession* session) noexcept
        : session_(session) {}

    /**
     * @brief Install the inbound datagram sink.
     *
     * Internally registers the sink on the bound session via
     * `IWebTransportSession::set_on_datagram()`. Pass an empty
     * std::function to unregister. Not thread-safe; call only
     * from the engine thread.
     */
    void set_sink(
        std::function<void(std::span<const std::uint8_t>)> sink) noexcept {
        if (session_ == nullptr) return;
        session_->set_on_datagram(
            [s = std::move(sink)](std::span<const std::uint8_t> bytes) {
                if (s) s(bytes);
            });
    }

    /**
     * @brief Send a single datagram.
     *
     * Forwards to `IWebTransportSession::send_datagram()`.
     *
     * @return Number of bytes queued on success;
     *         `kErrNotReady` if the session is not in
     *         kConnected state;
     *         `kErrInternal` if `session_` is null (the facade
     *         was constructed with a null pointer).
     */
    plugins::Status send(
        std::span<const std::uint8_t> bytes) noexcept {
        if (session_ == nullptr) return plugins::Status::kErrInternal;
        return session_->send_datagram(bytes);
    }

private:
    IWebTransportSession* session_;   // non-owning
};

} // namespace nimrtc::web_transport

#endif // NIMRTC_WEB_TRANSPORT_DATAGRAM_FACADE_HPP
