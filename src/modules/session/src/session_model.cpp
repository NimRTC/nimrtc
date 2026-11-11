/**
 * @file session_model.cpp
 * @brief SessionModel implementation — the `plugins::ISession` backend.
 *
 * v1.2.0 — `docs/plan/session-capability-plan.md` N1 / T1.
 */

#include <nimrtc/session/session_model.hpp>

#include <algorithm>
#include <utility>

namespace nimrtc::session {

SessionModel::SessionModel()  = default;
SessionModel::~SessionModel() = default;

// ---------------------------------------------------------------------------
// plugins::IPlugin
// ---------------------------------------------------------------------------

const char* SessionModel::name() const noexcept {
    return "NimRTC session model";
}

plugins::Status SessionModel::open() noexcept {
    // The model has no resources: it is a pure in-memory structure, so
    // opening it is a no-op.  The method exists because the seam is a
    // plugin like every other one, and the engine calls open() on the
    // model it resolved (the same lifecycle every plugin goes through).
    return plugins::kOk;
}

void SessionModel::close() noexcept {
    // Nothing to release.  The model stays usable after close() — the
    // engine may re-open it, and a host that re-opens it must not find
    // its tracks gone.
}

// ---------------------------------------------------------------------------
// Tracks
// ---------------------------------------------------------------------------

bool SessionModel::add_track(std::string id,
                              plugins::MediaKind kind,
                              plugins::Direction dir) {
    if (id.empty()) return false;
    if (track_index_.count(id) != 0) return false;

    TrackEntry entry;
    entry.id   = std::move(id);
    entry.kind = kind;
    entry.dir  = dir;

    track_index_.emplace(entry.id, tracks_.size());
    tracks_.push_back(std::move(entry));
    return true;
}

bool SessionModel::remove_track(std::string_view id) {
    const auto it = track_index_.find(std::string(id));
    if (it == track_index_.end()) return false;

    const std::size_t index = it->second;
    const std::string key  = tracks_[index].id;

    // The track leaves the transceiver slots too, but the transceiver
    // itself survives — its m-line stays in the SDP, marked `inactive`
    // (decision OD-A of the plan).
    for (auto& t : transceivers_) {
        if (t.send_track == key) t.send_track.clear();
        if (t.recv_track == key) t.recv_track.clear();
    }

    tracks_.erase(tracks_.begin() + static_cast<std::ptrdiff_t>(index));
    track_index_.erase(key);
    // The index map is dense-order sensitive: rebuild it so the indices
    // keep pointing at the right entries.
    track_index_.clear();
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        track_index_.emplace(tracks_[i].id, i);
    }
    return true;
}

bool SessionModel::set_track_direction(std::string_view id,
                                        plugins::Direction dir) {
    const auto it = track_index_.find(std::string(id));
    if (it == track_index_.end()) return false;
    tracks_[it->second].dir = dir;
    return true;
}

std::vector<plugins::TrackInfo> SessionModel::tracks() const {
    std::vector<plugins::TrackInfo> out;
    out.reserve(tracks_.size());
    for (const auto& t : tracks_) {
        plugins::TrackInfo info;
        info.id       = t.id;
        info.kind     = t.kind;
        info.direction = t.dir;
        info.ssrc     = t.ssrc;
        info.has_ssrc = t.has_ssrc;
        info.bundled  = t.bundled;
        out.push_back(std::move(info));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Transceivers
// ---------------------------------------------------------------------------

std::size_t SessionModel::find_transceiver(std::string_view mid) const {
    const auto it = transceiver_index_.find(std::string(mid));
    return it == transceiver_index_.end() ? kNpos : it->second;
}

std::size_t SessionModel::find_track(std::string_view id) const {
    const auto it = track_index_.find(std::string(id));
    return it == track_index_.end() ? kNpos : it->second;
}

std::string SessionModel::allocate_mid() {
    // Monotonic and never reused: a mid that has been handed out stays
    // dead even when its transceiver is stopped.
    while (true) {
        const std::string candidate = std::to_string(next_mid_++);
        if (transceiver_index_.count(candidate) == 0) return candidate;
    }
}

std::string SessionModel::add_transceiver(plugins::MediaKind kind,
                                          plugins::Direction dir) {
    const std::string mid = allocate_mid();

    TransceiverEntry entry;
    entry.mid  = mid;
    entry.kind = kind;
    entry.dir  = dir;

    transceiver_index_.emplace(mid, transceivers_.size());
    transceivers_.push_back(std::move(entry));
    return mid;
}

std::string SessionModel::add_transceiver_at(std::string_view mid,
                                             plugins::MediaKind kind,
                                             plugins::Direction dir) {
    const std::string requested(mid);
    if (!requested.empty() && transceiver_index_.count(requested) == 0) {
        // Honour the hint — this is what keeps the engine's base
        // m-lines at "0" / "1" / "2" regardless of the order in which
        // they become known.
        TransceiverEntry entry;
        entry.mid  = requested;
        entry.kind = kind;
        entry.dir  = dir;

        transceiver_index_.emplace(requested, transceivers_.size());
        transceivers_.push_back(std::move(entry));

        // Keep the monotonic counter ahead of every hand-edited mid so a
        // later allocation cannot collide with it.
        std::uint32_t numeric = 0;
        try {
            numeric = static_cast<std::uint32_t>(std::stoul(requested));
        } catch (...) {
            numeric = 0;
        }
        next_mid_ = std::max(next_mid_, numeric + 1);
        return requested;
    }
    // The requested mid is taken (or malformed) — fall back to a fresh
    // allocation.  A mid is never reused.
    return add_transceiver(kind, dir);
}

bool SessionModel::set_transceiver_track(std::string_view mid,
                                         std::string_view track_id,
                                         bool send) {
    const std::size_t index = find_transceiver(mid);
    if (index == kNpos) return false;

    const std::string key(track_id);
    if (!key.empty() && find_track(key) == kNpos) {
        // Attaching a track that does not exist must not touch the slots.
        return false;
    }

    TransceiverEntry& t = transceivers_[index];
    if (send) {
        t.send_track = key;
    } else {
        t.recv_track = key;
    }
    return true;
}

bool SessionModel::set_transceiver_direction(std::string_view mid,
                                             plugins::Direction dir) {
    const std::size_t index = find_transceiver(mid);
    if (index == kNpos) return false;
    transceivers_[index].dir = dir;
    return true;
}

bool SessionModel::stop_transceiver(std::string_view mid) {
    const std::size_t index = find_transceiver(mid);
    if (index == kNpos) return false;

    TransceiverEntry& t = transceivers_[index];
    if (!t.stopped) {
        t.dir_before_stop = t.dir;
        t.dir  = plugins::Direction::kInactive;
        t.stopped = true;
    }
    return true;
}

bool SessionModel::restart_transceiver(std::string_view mid) {
    const std::size_t index = find_transceiver(mid);
    if (index == kNpos) return false;

    TransceiverEntry& t = transceivers_[index];
    if (t.stopped) {
        t.dir = t.dir_before_stop;
        t.stopped = false;
    }
    return true;
}

std::vector<plugins::TransceiverInfo> SessionModel::transceivers() const {
    std::vector<plugins::TransceiverInfo> out;
    out.reserve(transceivers_.size());
    for (const auto& t : transceivers_) {
        plugins::TransceiverInfo info;
        info.mid           = t.mid;
        info.kind          = t.kind;
        info.direction     = t.dir;
        info.send_track_id = t.send_track;
        info.recv_track_id = t.recv_track;
        info.stopped       = t.stopped;
        out.push_back(std::move(info));
    }
    return out;
}

} // namespace nimrtc::session
