/**
 * @file nimrtc/sctp/sctp_socket_factory.hpp
 * @brief ISctpSocketFactory — seam factory for ISctpSocket (Slice 5).
 *
 * Per `docs/plan/transport-selection.md` §5.2:
 *   `create(const SctpConfig&)` produces a configured `ISctpSocket*`;
 *   `id()` returns the stable registration id (e.g. "stub" pre-v0.11.0,
 *   "usrsctp" once `UsrsctpSocket` lands).
 *
 * Slice 5 status:
 *   SEAM ONLY — only `SctpStubFactory` (id="stub") exists. v0.11.0 will add
 *   `UsrsctpSocketFactory` (id="usrsctp") per `docs/plan/transport-selection.md`
 *   §6.2 / §7 and promote it to the registered default.
 *
 * @note P0 scaffold — interface stable; binary layout TBD P4.
 */

#ifndef NIMRTC_SCTP_SCTP_SOCKET_FACTORY_HPP
#define NIMRTC_SCTP_SCTP_SOCKET_FACTORY_HPP

#include <memory>
#include <string_view>

#include <nimrtc/plugins/base.hpp>   // Status

namespace nimrtc::sctp {

// Forward declaration — full type lives in sctp_socket_iface.hpp.
class ISctpSocket;

// ---------------------------------------------------------------------------
// SctpConfig — opaque per-socket config passed to ISctpSocketFactory::create().
// ---------------------------------------------------------------------------
//
// Intentionally minimal in Slice 5 — only the fields required to honour the
// ISctpSocket contract are present. v0.11.0 will extend this with usrsctp-
// specific knobs (e.g. `max_num_streams`, `local_port`, `dtls_srtp_keys`,
// SCTP-over-DTLS plumbing). Keep the seam additive so v0.11.0 is a pure
// field addition — no breakage at existing call sites.
// ---------------------------------------------------------------------------
struct SctpConfig {
    /** Application-level label, surfaced in logs. May be empty. */
    std::string_view label;

    /** Maximum number of outbound streams we will use (default 16 — matches
     *  WebRTC DataChannel default; sufficient for control/telemetry/file). */
    std::uint16_t max_num_streams = 16;

    /** Local SCTP port hint. 0 = backend default (usrsctp picks a free one
     *  in 0.0.0.0:0; libjuice/DTLS feed the actual transport later). */
    std::uint16_t local_port = 0;
};

// ---------------------------------------------------------------------------
// ISctpSocketFactory
// ---------------------------------------------------------------------------

/** Factory for SCTP backend instances. */
class ISctpSocketFactory {
public:
    virtual ~ISctpSocketFactory() = default;

    /** Unique registration id, e.g. "stub" (Slice 5 default) or "usrsctp"
     *  (v0.11.0 production). */
    virtual std::string_view id() const noexcept = 0;

    /** Short human-readable name, e.g. "SCTP (stub)" or "SCTP (usrsctp)". */
    virtual std::string_view display_name() const noexcept = 0;

    /** Create a new SCTP socket instance configured from `cfg`.
     *  Ownership passes to the caller. */
    virtual std::unique_ptr<ISctpSocket> create(const SctpConfig& cfg) const = 0;
};

} // namespace nimrtc::sctp

#endif // NIMRTC_SCTP_SCTP_SOCKET_FACTORY_HPP
