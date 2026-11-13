/**
 * @file src/engine/src/engine_session_state.hpp
 * @brief PRIVATE engine collaborator: the session / transport-plane state.
 *
 * Architecture remediation plan §4.3 (D1 — engine split): this state used
 * to live as twenty-odd loose members inside `NimRTCEngine::Impl`, which
 * made the engine's internal header a catch-all and scattered one
 * feature's state across the file.
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * ## What lives here
 *
 *   - the **session model** (the pluggable `plugins::ISession`) and the
 *     mids it handed out;
 *   - the **DTLS role negotiation** (RFC 5763 §5) and the offer's
 *     session id (RFC 3264 §5.1);
 *   - the **ICE flap** detector that arms a DTLS rehandshake after a
 *     NAT rebinding;
 *   - the **DataChannel / WebTransport** session ownership;
 *   - the **resolved transport strategy** (the factory ids the
 *     capability selector picked).
 *
 * The transport *stack* itself (`TransportSession`, which owns the media
 * and control `ITransportStack` instances) stays in `Impl`: it is the
 * engine's own composition, not a peer-negotiated state.
 */
#pragma once

#include <memory>
#include <optional>
#include <string>

#include <nimrtc/dtls/dtls_session_iface.hpp>
#include <nimrtc/plugins/ice_transport.hpp>
#include <nimrtc/plugins/session.hpp>
#include <nimrtc/sdp/session_description.hpp>
#include <nimrtc/web_transport/web_transport_session_iface.hpp>

namespace nimrtc::engine::detail {

/** The session / transport-plane state. */
struct SessionState {
    // ---- Session model (v1.2.0, session capability plan N1 / T1) --------
    //
    // The engine drives renegotiation through a pluggable session model
    // (`plugins::ISession`), resolved from the registry's session slot
    // with `EngineConfig::session_name`.  The engine never includes
    // `src/modules/session/`'s headers (Layout Invariant 4) — it only
    // talks to the interface declared in
    // `<nimrtc/plugins/session.hpp>`.
    //
    // `session_model` is null when the session model is disabled
    // (`session_name` empty) or when the resolution failed; in that
    // case the engine keeps the pre-v1.2.0 fixed media list.
    std::unique_ptr<plugins::ISession> session_model;

    // True once the base session state (the audio / video / data
    // transceiver and the tracks that ride on them) has been created.
    // The seeding is idempotent: it runs from `create_offer()` and
    // after `open()` resolved the video / datachannel plugins, and it
    // only ever *adds* what is missing.
    bool session_seeded = false;

    // True when the session changed since the last offer / answer was
    // produced — the host's cue to renegotiate (N2).
    bool session_dirty = false;

    // The mids the engine handed out for the base m-lines.  Empty means
    // "not created yet"; the value is the mid that is actually in the
    // SDP.
    std::string base_audio_mid;
    std::string base_video_mid;
    std::string base_data_mid;

    // ---- DTLS role negotiation ------------------------------------------
    // Resolved in process_remote_sdp() from the peer's a=setup attribute.
    // Used to emit a matching a=setup on the answer SDP.  Empty until
    // process_remote_sdp() runs; defaults to "active" if unset.
    std::string dtls_local_setup;

    /** The DTLS role this engine ended up playing (RFC 5763 §5).
     *
     *  `true`  = DTLS **Client** — we send the ClientHello.
     *  `false` = DTLS **Server** — we wait for the peer's ClientHello.
     *
     *  The role decides two things:
     *
     *    1. which `a=setup` attribute we advertise in the answer, and
     *    2. which half of the DTLS-SRTP keying material is *ours*
     *       (RFC 5764 §4.2): the client uses `client_master_key` for its
     *       outbound stream, the server uses `server_master_key`.
     *
     *  It is resolved from the peer's `a=setup` attribute when we are the
     *  answerer, and from the answer's attribute when we are the offerer
     *  (see `process_remote_sdp()`). */
    bool dtls_is_client = false;

    /** The `o=` session id of the offer we produced, if any.
     *
     *  RFC 3264 §5.1: the answer carries the *offer's* session id, so the
     *  offerer can tell "this is the answer to my offer" from "this is a
     *  fresh offer" by comparing the ids.  That distinction is what lets
     *  `process_remote_sdp()` pick the right DTLS role (the answer commits
     *  the roles, an offer does not) and skip generating an answer for an
     *  answer.  Empty when we never produced an offer. */
    std::string local_session_id;

    // Local SDP (after create_offer / process_remote_sdp).
    std::optional<sdp::SessionDescription> local_sdp;

    // ---- ICE flap -> DTLS rehandshake (v1.1 Dtls2, additive) ------------
    // Tracks ICE state across tick() to detect NAT rebinding:
    // connected once -> disconnected (arm) -> connected again (trigger
    // IDtlsSession::rehandshake()). plugins::IceState mirrors
    // ice::IceState 1:1; no conversion needed.
    plugins::IceState last_ice_state = plugins::IceState::Disconnected;
    bool ice_connected_once = false;
    bool ice_rehandshake_armed = false;

    // ---- DataChannel DTLS-transport cookie (v1.x follow-up) ------------
    //
    // Placeholder for the physical SCTP-over-DTLS wiring that lands
    // in the **v1.x WebTransport / QUIC** re-design — see
    // `docs/plan/v0.11-plan.md` §2.2 (DC-1 / DC-2 / TPAL-5 deferred)
    // and §10 (forward direction). v0.11.0 falls back to the
    // in-process `tests/test_datachannel_engine` 5/5 PASS as the §7
    // substitute, made permanent.
    //
    // Typed as `nimrtc::dtls::IDtlsSession*` (non-owning) so the field
    // is self-describing — the v1.x wiring PR that swaps to
    // WebTransport will replace this with a tagged union over
    // {IDtlsSession*, IWebTransportSession*} rather than juggling a
    // raw `void*` cast.  Lifetime (v0.13.0): weak_ptr into the
    // media_stack's DTLS session; expired automatically after
    // close() tears down the stack.
    //
    // Initialised as an empty weak_ptr in init_modules_once() — at that point
    // `impl_->dtls` is a weak_ptr into `impl_->session.media_stack`. By the
    // time this method runs, `init_modules_once()` has built the session and
    // either set `impl_->dtls` to the stack's DTLS session (browser-interop
    // strategies) or left it expired (Rule 2 / raw-udp-arq media).
    // create_data_channel() then assigns impl_->dtls into this cookie
    // so the v1.x wiring code has a single, well-known place to look
    // for the underlying transport.  See also the TODO in
    // init_modules_once().
    //
    // v0.13.0 (this change): changed from raw pointer to weak_ptr, aligned
    // with Impl::dtls.  The cookie is informational — callers must .lock()
    // before use; they get nullptr after close() automatically.
    std::weak_ptr<nimrtc::dtls::IDtlsSession> datachannel_transport_cookie;

    // ---- WebTransport session ownership (v0.14.0-rc.1 follow-up) ----
    //
    // Created lazily by create_data_channel() when
    // `resolved_dc_factory_id_ == "web_transport"`, from
    // `config_.web_transport_name` + `web_transport_peer_host/port`.
    // Shared (not unique) so every channel created by this engine sits
    // on one QUIC connection. Channels hold a non-owning pointer into
    // it — callers must destroy channels before close() (same contract
    // as the non-owning ice_t_ handle). Torn down in close().
    std::shared_ptr<nimrtc::web_transport::IWebTransportSession> wt_session;

    // Last polled WebTransport session state (v0.15.0, product route).
    // Polled in tick(); transitions to kConnected/kFailed are reported
    // via on_data_state_("open"/"error") so engine integrators get the
    // same callback shape as the reliable-stream open path.
    // `std::uint8_t` (not the enum) so the header stays free of the
    // web_transport include; the .cpp maps it back to WebTransportState.
    std::uint8_t wt_last_state = 0;  // kConnecting

    // ---- Resolved transport strategy (v0.12.0 Slice 7.6 PR3) ---------
    //
    // Filled in by `init_transport_strategy()` (called from
    // `init_modules_once()` at the very top, before any module is
    // created).  The strategy name comes from `cfg.transport_strategy_name`
    // (or "browser-call" when empty); the factory ids come from
    // `CapabilitySelector::last_trace()`.  The ids are stored as
    // `std::string` (owned) because the Selector's trace returns
    // `std::string` values that are local to the call site — string_views
    // into the trace would dangle as soon as the local goes out of scope.
    std::string      resolved_strategy_name;       // owned
    std::string      resolved_media_factory_id;     // owned
    std::string      resolved_control_factory_id;   // owned (may be empty)

    // The media-plane p99 budget the strategy resolved to, after the
    // per-instance override in `cfg.transport_media_max_p99_ms` was
    // applied (architecture remediation plan §4.2 / D2).  Empty = the
    // strategy declared no budget.
    std::optional<int> resolved_media_max_p99_ms;

    /** The return code of the last `open()` attempt.  0 = success.
     *
     *  `open()` reports its failure through the return value; the field
     *  keeps the *last* one so a caller that polls the engine can tell
     *  "still closed" from "closed because the handshake failed". */
    std::uint32_t last_open_rc = 0;
};

}  // namespace nimrtc::engine::detail
