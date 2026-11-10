/**
 * @file nimrtc/plugins/session.hpp
 * @brief ISession — pluggable session / track / transceiver model.
 *
 * v1.2.0 — `docs/plan/session-capability-plan.md` N1 / T1.
 *
 * ## Why this seam exists
 *
 * `src/modules/session/` already ships a Track / Stream / Session data
 * model, but until v1.2.0 nothing in the engine could reach it: the
 * engine built a *fixed* SDP media list (one audio m-line, one video
 * m-line) and exposed no way for a host to add a track, remove a track
 * or change a track's direction.  A host that wanted renegotiation had
 * to drive `session::Session` by hand and re-implement the SDP mapping
 * on the side.
 *
 * This seam is what makes renegotiation reachable:
 *
 *   - the engine resolves an `ISession` implementation through
 *     `core::PluginRegistry::get_session(id)` (ADR-001, same shape as
 *     every other typed slot);
 *   - the engine drives it through the interface declared here and
 *     never includes `src/modules/session/`'s headers — Layout
 *     Invariant 4 says the engine is a composition layer and must not
 *     depend on concrete module headers;
 *   - the built-in implementation lives in `src/modules/session/` and
 *     registers under the id `"session"`.
 *
 * A host that wants a different session model (a different BUNDLE
 * policy, a different mid-allocation strategy) implements `ISession`
 * and registers it; the engine picks it up by id without a rebuild.
 *
 * ## Relationship to the existing `session::TrackDirection`
 *
 * `nimrtc::session::TrackDirection` (the concrete module's enum) and
 * `plugins::Direction` (the enum here) carry the same four RFC 4566
 * states.  They are deliberately *separate* types: the seam must not
 * name a concrete module's types, so the module converts at its
 * boundary.  The mapping is one-to-one and lossless.
 *
 * @note The m-line ↔ transceiver mapping rules that the engine
 *       implements on top of this interface (mid stability, BUNDLE
 *       membership, the `inactive` semantics of a removed track) are
 *       specified in `docs/plan/session-capability-plan.md` §2 and
 *       `docs/api/engine_api.md`.
 */

// base.hpp must be before include guard — see transport.hpp for rationale.
#include <nimrtc/core/api_version.hpp>
#include "nimrtc/plugins/base.hpp"

#ifndef NIMRTC_PLUGINS_SESSION_HPP
#define NIMRTC_PLUGINS_SESSION_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// Media kinds and directions
// ---------------------------------------------------------------------------

/** The kind of media a track or transceiver carries. */
enum class NIMRTC_API_STABLE MediaKind : std::uint8_t {
    kAudio,
    kVideo,
    kData,
};

/** A track's or transceiver's direction (the four RFC 4566 states).
 *
 *  The names match `sdp::Direction` one-to-one; the engine maps them
 *  onto the `a=sendonly` / `a=recvonly` / `a=sendrecv` / `a=inactive`
 *  media-level attributes. */
enum class NIMRTC_API_STABLE Direction : std::uint8_t {
    kSendOnly,
    kRecvOnly,
    kSendRecv,
    kInactive,
};

// ---------------------------------------------------------------------------
// Value types
// ---------------------------------------------------------------------------

/** A snapshot of one track. */
struct NIMRTC_API_STABLE TrackInfo {
    /** Stable identity of the track (host-chosen). */
    std::string id;

    /** What the track carries. */
    MediaKind kind = MediaKind::kAudio;

    /** Current direction. */
    Direction direction = Direction::kSendRecv;

    /** SSRC, when one has been assigned. */
    std::uint32_t ssrc = 0;

    /** True when `ssrc` is meaningful. */
    bool has_ssrc = false;

    /** True when the track multiplexes over the BUNDLE transport. */
    bool bundled = false;
};

/** A snapshot of one transceiver (one m-line's worth of session state). */
struct NIMRTC_API_STABLE TransceiverInfo {
    /** The `a=mid:` value this transceiver owns. */
    std::string mid;

    /** What the transceiver carries. */
    MediaKind kind = MediaKind::kAudio;

    /** Current direction of the transceiver as a whole. */
    Direction direction = Direction::kSendRecv;

    /** Id of the track this transceiver *sends*, or empty when it sends
     *  nothing.  A transceiver may send and receive on different
     *  tracks — that is the Unified Plan's send / receive split. */
    std::string send_track_id;

    /** Id of the track this transceiver *receives*, or empty when it
     *  receives nothing. */
    std::string recv_track_id;

    /** True when the transceiver has been stopped.  A stopped
     *  transceiver stays in the SDP (its m-line is marked `inactive`)
     *  — see OD-A in `docs/plan/session-capability-plan.md`. */
    bool stopped = false;
};

// ---------------------------------------------------------------------------
// ISession
// ---------------------------------------------------------------------------

/** The session model the engine drives.
 *
 *  Thread-safety: the engine drives this from its own thread only (the
 *  same thread that calls `tick()`); the interface does not promise
 *  internal locking. */
class NIMRTC_API_STABLE ISession : public IPlugin {
public:
    // ---- Track management ------------------------------------------------

    /** Add a track.
     *
     *  @param id       Host-chosen identity; must be unique within the
     *                  session.
     *  @param kind     What the track carries.
     *  @param dir      Initial direction.
     *  @return true on success; false when the id is already taken. */
    virtual bool add_track(std::string id, MediaKind kind, Direction dir) = 0;

    /** Remove a track.
     *
     *  Removing a track does **not** remove its m-line — the engine
     *  keeps the transceiver and marks it `inactive` (OD-A).  The
     *  removal still clears the track's slots in the transceiver.
     *
     *  @return true when a track with that id existed. */
    virtual bool remove_track(std::string_view id) = 0;

    /** Change a track's direction. */
    virtual bool set_track_direction(std::string_view id, Direction dir) = 0;

    /** Snapshot of every track in the session. */
    virtual std::vector<TrackInfo> tracks() const = 0;

    // ---- Transceiver management -------------------------------------------

    /** Create a transceiver and allocate a mid for it.
     *
     *  The returned mid is unique within the session and never reused
     *  once handed out — mid stability is what lets the engine keep a
     *  removed track's m-line around as `inactive` without confusing
     *  the peer.
     *
     *  @return The allocated mid, or an empty string on failure. */
    virtual std::string add_transceiver(MediaKind kind, Direction dir) = 0;

    /** Create a transceiver *at a specific mid*.
     *
     *  The engine uses this for the m-lines it always emits -- the
     *  audio m-line is `mid="0"`, the video m-line `mid="1"`, the
     *  data m-line `mid="2"` -- so the numbering stays stable no
     *  matter in which order the m-lines become known (a peer may
     *  offer video before the local video plugin is resolved, and a
     *  host may add a track before the engine is even open).
     *
     *  The default implementation ignores the hint and falls back to
     *  `add_transceiver()`, which is always correct but lets the mid
     *  depend on the allocation order.  An implementation that can
     *  honour the hint should override this.
     *
     *  @param mid The mid to use.  When it is already taken, the
     *             implementation MUST fall back to a fresh mid -- a
     *             mid is never reused.
     *  @return The allocated mid, or an empty string on failure. */
    virtual std::string add_transceiver_at(std::string_view mid,
                                           MediaKind kind,
                                           Direction dir) {
        (void)mid;
        return add_transceiver(kind, dir);
    }

    /** Attach a track to a transceiver's send or receive slot.
     *
     *  @param track_id Id of the track to attach; empty detaches.
     *  @param send     true to attach as the sending track, false as the
     *                  receiving track.
     *  @return true when the transceiver exists. */
    virtual bool set_transceiver_track(std::string_view mid,
                                       std::string_view track_id,
                                       bool send) = 0;

    /** Change a transceiver's direction. */
    virtual bool set_transceiver_direction(std::string_view mid,
                                           Direction dir) = 0;

    /** Stop a transceiver.  The transceiver stays in the session and its
     *  m-line is marked `inactive` (OD-A). */
    virtual bool stop_transceiver(std::string_view mid) = 0;

    /** Restart a stopped transceiver. */
    virtual bool restart_transceiver(std::string_view mid) = 0;

    /** Snapshot of every transceiver in the session. */
    virtual std::vector<TransceiverInfo> transceivers() const = 0;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

class NIMRTC_API_STABLE ISessionFactory {
public:
    virtual ~ISessionFactory() = default;
    virtual std::string_view id()           const noexcept = 0;
    virtual std::string_view display_name() const noexcept = 0;
    virtual ISession* create()              const = 0;
};

template<class T>
class NIMRTC_API_STABLE SimpleSessionFactory : public ISessionFactory {
    std::string_view id_;
    std::string_view name_;
public:
    explicit SimpleSessionFactory(std::string_view id,
                                  std::string_view name) noexcept
        : id_(id), name_(name) {}
    virtual std::string_view id()           const noexcept override { return id_; }
    virtual std::string_view display_name() const noexcept override { return name_; }
    virtual ISession* create()              const override { return new T(); }
};

} // namespace nimrtc::plugins
#endif // NIMRTC_PLUGINS_SESSION_HPP
