/**
 * @file session_model.hpp
 * @brief SessionModel — the `plugins::ISession` implementation.
 *
 * v1.2.0 — `docs/plan/session-capability-plan.md` N1 / T1.
 *
 * ## What this is
 *
 * `src/modules/session/` already shipped a Track / Stream / Session data
 * model, but the engine could not reach it: the engine built a *fixed*
 * SDP media list (one audio m-line, one video m-line) and exposed no
 * way for a host to add a track, remove a track or change a track's
 * direction.  This class is the concrete session model behind the
 * `plugins::ISession` seam, so the engine can drive renegotiation
 * without naming a concrete module (Layout Invariant 4).
 *
 * ## Model
 *
 * The model holds two collections:
 *
 *   - **tracks** — the media the host wants to exchange.  A track has
 *     an id, a kind (audio / video / data) and a direction.
 *   - **transceivers** — one per SDP m-line.  A transceiver owns a
 *     `mid`, a kind, a direction, an optional *send* track and an
 *     optional *receive* track, and a `stopped` flag.
 *
 * A transceiver with a stopped flag keeps its mid: the m-line stays in
 * the SDP and is marked `inactive` (decision OD-A of the plan).  The
 * same holds for a removed track — the transceiver survives, empty.
 *
 * ## Mid stability
 *
 * Mids are allocated monotonically and are **never reused**, not even
 * after the transceiver that owned them was stopped.  `add_transceiver_at()`
 * lets the caller ask for a specific mid (the engine uses "0" for
 * audio, "1" for video, "2" for data); when the requested mid is
 * already taken the model falls back to a fresh allocation.
 *
 * @note Thread-safety: none.  The engine drives the session from the
 *       thread that owns the engine; the seam makes no promise about
 *       concurrent access.
 */

#ifndef NIMRTC_SESSION_SESSION_MODEL_HPP
#define NIMRTC_SESSION_SESSION_MODEL_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).

#include <nimrtc/core/api_version.hpp>
#include <nimrtc/plugins/session.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nimrtc::session {

/** The concrete session model behind `plugins::ISession`.
 *
 *  Registered by `nimrtc::session::register_default_plugins()` under
 *  the id "session"; the engine resolves it through
 *  `core::PluginRegistry::get_session(EngineConfig::session_name)`. */
class NIMRTC_API_STABLE SessionModel final : public plugins::ISession {
public:
    SessionModel();
    ~SessionModel() override;

    SessionModel(const SessionModel&)            = delete;
    SessionModel& operator=(const SessionModel&) = delete;
    SessionModel(SessionModel&&)                 = default;
    SessionModel& operator=(SessionModel&&)      = default;

    // ---- plugins::IPlugin ------------------------------------------------

    const char* name() const noexcept override;
    plugins::Status open() noexcept override;
    void close() noexcept override;

    // ---- Track management ------------------------------------------------

    bool add_track(std::string id,
                   plugins::MediaKind kind,
                   plugins::Direction dir) override;

    bool remove_track(std::string_view id) override;

    bool set_track_direction(std::string_view id, plugins::Direction dir) override;

    std::vector<plugins::TrackInfo> tracks() const override;

    // ---- Transceiver management -------------------------------------------

    std::string add_transceiver(plugins::MediaKind kind,
                                plugins::Direction dir) override;

    std::string add_transceiver_at(std::string_view mid,
                                   plugins::MediaKind kind,
                                   plugins::Direction dir) override;

    bool set_transceiver_track(std::string_view mid,
                               std::string_view track_id,
                               bool send) override;

    bool set_transceiver_direction(std::string_view mid,
                                   plugins::Direction dir) override;

    bool stop_transceiver(std::string_view mid) override;

    bool restart_transceiver(std::string_view mid) override;

    std::vector<plugins::TransceiverInfo> transceivers() const override;

private:
    struct TrackEntry {
        std::string          id;
        plugins::MediaKind   kind    = plugins::MediaKind::kAudio;
        plugins::Direction   dir     = plugins::Direction::kSendRecv;
        std::uint32_t        ssrc    = 0;
        bool                 has_ssrc = false;
        bool                 bundled = false;
    };

    struct TransceiverEntry {
        std::string          mid;
        plugins::MediaKind   kind      = plugins::MediaKind::kAudio;
        plugins::Direction   dir       = plugins::Direction::kSendRecv;
        plugins::Direction   dir_before_stop = plugins::Direction::kSendRecv;
        std::string          send_track;
        std::string          recv_track;
        bool                 stopped   = false;
    };

    /** Allocate a fresh mid.  Monotonic, never reused. */
    std::string allocate_mid();

    /** Index of the transceiver with that mid, or npos. */
    static constexpr std::size_t kNpos = static_cast<std::size_t>(-1);
    std::size_t find_transceiver(std::string_view mid) const;
    std::size_t find_track(std::string_view id) const;

    std::vector<TrackEntry>       tracks_;
    std::unordered_map<std::string, std::size_t> track_index_;

    std::vector<TransceiverEntry> transceivers_;
    std::unordered_map<std::string, std::size_t> transceiver_index_;

    /** Next mid counter — monotonic, never restarted. */
    std::uint32_t next_mid_ = 0;
};

} // namespace nimrtc::session

#endif // NIMRTC_SESSION_SESSION_MODEL_HPP
