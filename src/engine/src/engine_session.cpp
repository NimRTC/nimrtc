/**
 * @file engine_session.cpp
 * @brief The engine's session / track / transceiver surface.
 *
 * v1.2.0 — `docs/plan/session-capability-plan.md` N1 / N2 / N3 / N4 /
 * N5 and T1 / T2 / T3 / T4.
 *
 * ## What lives here
 *
 * The engine drives renegotiation through a *pluggable* session model
 * (`plugins::ISession`), resolved from the registry's session slot with
 * `EngineConfig::session_name`.  The engine never includes
 * `src/modules/session/`'s headers (Layout Invariant 4) — it only
 * talks to the interface.
 *
 * The session model owns two collections:
 *
 *   - **tracks** — what the host wants to exchange;
 *   - **transceivers** — one per SDP m-line, each with a `mid`, a
 *     direction and an optional send / receive track.
 *
 * `create_offer()` turns the transceivers into the media list, so a
 * track that was added, removed or re-pointed shows up in the next
 * offer.
 *
 * ## The rules the SDP has to honour
 *
 *   - **mid stability** — a mid is handed out once and never reused.
 *     The base m-lines keep their numbers (audio "0", video "1", data
 *     "2") no matter in which order they become known.
 *   - **OD-A / N5 / T4** — a removed track or a stopped transceiver
 *     does *not* remove the m-line: it stays, marked `inactive`, and
 *     the mid is not recycled.
 *   - **N4** — the m-line direction follows the transceiver, and the
 *     transceiver direction follows the tracks that ride on it.
 */

#include "engine_impl.hpp"

#include <algorithm>

namespace nimrtc::engine {

namespace {

// The ids the engine gives the tracks it owns.  They are prefixed so a
// host cannot collide with them by accident.
constexpr std::string_view kBaseAudioTrackId = "nimrtc-audio";
constexpr std::string_view kBaseVideoTrackId = "nimrtc-video";

// The mids of the base m-lines.  They are fixed so the media list does
// not shift when a plugin becomes available later (T3).
constexpr std::string_view kBaseAudioMid = "0";
constexpr std::string_view kBaseVideoMid = "1";
constexpr std::string_view kBaseDataMid  = "2";

/** Does the track direction let us *send* on it? */
constexpr bool dir_sends(plugins::Direction dir) noexcept {
    return dir == plugins::Direction::kSendOnly ||
           dir == plugins::Direction::kSendRecv;
}

/** Does the track direction let us *receive* on it? */
constexpr bool dir_receives(plugins::Direction dir) noexcept {
    return dir == plugins::Direction::kRecvOnly ||
           dir == plugins::Direction::kSendRecv;
}

} // namespace

// ---------------------------------------------------------------------------
// Resolution + seeding
// ---------------------------------------------------------------------------

bool NimRTCEngine::resolve_session_model() noexcept {
    if (impl_->session_state.session_model) return true;
    if (config_.session_name.empty()) return false;

    const auto* factory =
        core::PluginRegistry::instance().get_session(config_.session_name);
    if (factory == nullptr) {
        // Not fatal: the engine falls back to the pre-v1.2.0 fixed
        // media list.  A host that asked for a session model it does
        // not have gets a loud log line rather than a broken open().
        core::log::Logger::instance().error(
            std::string("session model '") + std::string(config_.session_name) +
            "' is not registered — the engine keeps its fixed media list");
        return false;
    }

    std::unique_ptr<plugins::ISession> model(factory->create());
    if (!model) {
        core::log::Logger::instance().error(
            std::string("session model '") + std::string(config_.session_name) +
            "' failed to create an instance");
        return false;
    }
    impl_->session_state.session_model = std::move(model);
    return true;
}

void NimRTCEngine::ensure_base_session_state() noexcept {
    if (!impl_->session_state.session_model) return;
    plugins::ISession& s = *impl_->session_state.session_model;

    const auto transceivers = s.transceivers();

    // Does a transceiver with this mid still exist?
    const auto mid_alive = [&](const std::string& mid) {
        if (mid.empty()) return false;
        for (const auto& t : transceivers) {
            if (t.mid == mid) return true;
        }
        return false;
    };

    // ---- Audio base ----------------------------------------------------
    //
    // The audio m-line is always there (the engine always offers one).
    // Its transceiver is remembered from the previous seeding round, so
    // a second `ensure_base_session_state()` call finds the very same
    // transceiver instead of allocating a second one — the seeding has
    // to be idempotent, because it runs from `create_offer()` *and*
    // again after `open()` resolved the video plugins.
    std::string audio_mid = impl_->session_state.base_audio_mid;
    if (!mid_alive(audio_mid)) {
        audio_mid.clear();
        for (const auto& t : transceivers) {
            if (t.kind == plugins::MediaKind::kAudio && t.send_track_id.empty()) {
                audio_mid = t.mid;
                break;
            }
        }
        if (audio_mid.empty()) {
            audio_mid = s.add_transceiver_at(kBaseAudioMid, plugins::MediaKind::kAudio,
                                             plugins::Direction::kSendRecv);
        }
    }
    impl_->session_state.base_audio_mid = audio_mid;
    if (audio_mid.empty()) return;

    // The engine's own audio track rides on it.
    bool has_base_audio = false;
    for (const auto& t : s.tracks()) {
        if (t.id == kBaseAudioTrackId) { has_base_audio = true; break; }
    }
    if (!has_base_audio) {
        s.add_track(std::string(kBaseAudioTrackId), plugins::MediaKind::kAudio,
                    plugins::Direction::kSendRecv);
    }
    s.set_transceiver_track(audio_mid, kBaseAudioTrackId, true);

    // ---- Video base ----------------------------------------------------
    //
    // Only when the video pipeline is actually there: an m-line we
    // cannot serve would be a lie.
    if (video_receiver_ || video_sender_) {
        std::string video_mid = impl_->session_state.base_video_mid;
        if (!mid_alive(video_mid)) {
            video_mid.clear();
            for (const auto& t : transceivers) {
                if (t.kind == plugins::MediaKind::kVideo && t.send_track_id.empty()) {
                    video_mid = t.mid;
                    break;
                }
            }
            if (video_mid.empty()) {
                video_mid = s.add_transceiver_at(kBaseVideoMid, plugins::MediaKind::kVideo,
                                                 plugins::Direction::kSendRecv);
            }
        }
        impl_->session_state.base_video_mid = video_mid;

        if (video_mid.empty()) return;

        bool has_base_video = false;
        for (const auto& t : s.tracks()) {
            if (t.id == kBaseVideoTrackId) { has_base_video = true; break; }
        }
        if (!has_base_video) {
            s.add_track(std::string(kBaseVideoTrackId), plugins::MediaKind::kVideo,
                        plugins::Direction::kSendRecv);
        }
        s.set_transceiver_track(video_mid, kBaseVideoTrackId, true);
    }

    // ---- Data base -----------------------------------------------------
    //
    // The SCTP data m-line only exists when the SCTP backend is the
    // effective one (the WebTransport backend rides its own QUIC
    // connection and emits no m=application line).
    const bool wt_backend =
        (!resolved_dc_factory_id_.empty()
             ? (resolved_dc_factory_id_ == "web_transport")
             : (config_.datachannel_backend == "web_transport"));
    if (!wt_backend && !config_.datachannel_name.empty()) {
        std::string data_mid = impl_->session_state.base_data_mid;
        if (!mid_alive(data_mid)) {
            for (const auto& t : transceivers) {
                if (t.kind == plugins::MediaKind::kData) {
                    data_mid = t.mid;
                    break;
                }
            }
            if (data_mid.empty()) {
                data_mid = s.add_transceiver_at(kBaseDataMid, plugins::MediaKind::kData,
                                                plugins::Direction::kSendRecv);
            }
        }
        impl_->session_state.base_data_mid = data_mid;
    }
}

void NimRTCEngine::sync_transceiver_direction(std::string_view mid) noexcept {
    if (!impl_->session_state.session_model) return;
    plugins::ISession& s = *impl_->session_state.session_model;

    bool can_send = false;
    bool can_recv = false;
    for (const auto& t : s.transceivers()) {
        if (t.mid != mid) continue;
        // The send slot carries what we send, the recv slot what we
        // receive; a `sendrecv` track contributes to both.
        for (const auto& track : s.tracks()) {
            if (t.send_track_id == track.id) {
                can_send = can_send || dir_sends(track.direction);
                can_recv = can_recv || dir_receives(track.direction);
            }
            if (t.recv_track_id == track.id) {
                can_send = can_send || dir_sends(track.direction);
                can_recv = can_recv || dir_receives(track.direction);
            }
        }
        break;
    }

    plugins::Direction dir = plugins::Direction::kInactive;
    if (can_send && can_recv)      dir = plugins::Direction::kSendRecv;
    else if (can_send)             dir = plugins::Direction::kSendOnly;
    else if (can_recv)             dir = plugins::Direction::kRecvOnly;

    s.set_transceiver_direction(mid, dir);
}

std::string NimRTCEngine::bind_track_to_transceiver(std::string_view track_id,
                                                    plugins::MediaKind kind) {
    if (!impl_->session_state.session_model) return {};
    plugins::ISession& s = *impl_->session_state.session_model;

    // The track's own direction decides which slot(s) it fills: a
    // sendonly track fills the send slot, a recvonly track the receive
    // slot, and a sendrecv track both — that is the Unified Plan's
    // send / receive split (T3).
    plugins::Direction dir = plugins::Direction::kSendRecv;
    for (const auto& t : s.tracks()) {
        if (t.id == track_id) { dir = t.direction; break; }
    }
    const bool want_send = dir_sends(dir);
    const bool want_recv = dir_receives(dir);

    // Prefer an existing transceiver of the same kind with the slot we
    // need still free; otherwise create one (that is what grows the
    // offer by an m-line).
    for (const auto& t : s.transceivers()) {
        if (t.kind != kind) continue;
        if (t.stopped) continue;
        const bool send_free = t.send_track_id.empty();
        const bool recv_free = t.recv_track_id.empty();
        if (want_send && !send_free) continue;
        if (want_recv && !recv_free) continue;
        if (want_send) s.set_transceiver_track(t.mid, track_id, true);
        if (want_recv)  s.set_transceiver_track(t.mid, track_id, false);
        return t.mid;
    }

    const std::string mid = s.add_transceiver(kind, plugins::Direction::kSendRecv);
    if (mid.empty()) return {};
    if (want_send) s.set_transceiver_track(mid, track_id, true);
    if (want_recv)  s.set_transceiver_track(mid, track_id, false);
    return mid;
}

// ---------------------------------------------------------------------------
// Mirroring the peer's m-lines (T2 / T3)
// ---------------------------------------------------------------------------

void NimRTCEngine::mirror_remote_media(const sdp::SessionDescription& remote) noexcept {
    if (!impl_->session_state.session_model) return;
    plugins::ISession& s = *impl_->session_state.session_model;

    for (std::size_t index = 0; index < remote.media.size(); ++index) {
        const auto& rm = remote.media[index];
        if (rm.type != sdp::MediaType::Audio &&
            rm.type != sdp::MediaType::Video &&
            rm.type != sdp::MediaType::Application) {
            continue;
        }

        plugins::MediaKind kind = plugins::MediaKind::kAudio;
        if (rm.type == sdp::MediaType::Video) {
            kind = plugins::MediaKind::kVideo;
        } else if (rm.type == sdp::MediaType::Application) {
            kind = plugins::MediaKind::kData;
        }

        // The peer's mid is authoritative: it is the key both ends use
        // to pair the m-lines (RFC 8829 §5.2.1).  A description without
        // a mid falls back to the positional assignment the answer path
        // uses as well.
        std::string mid = rm.mid;
        if (mid.empty()) mid = std::to_string(index);

        bool exists = false;
        for (const auto& t : s.transceivers()) {
            if (t.mid == mid) { exists = true; break; }
        }

        // Our direction is the inverse of the peer's (RFC 3264 §6.1):
        // a peer that only sends leaves us a receiver.
        const plugins::Direction ours =
            detail::from_sdp_direction(detail::invert_sdp_direction(rm.direction));

        if (!exists) {
            s.add_transceiver_at(mid, kind, ours);
        } else {
            s.set_transceiver_direction(mid, ours);
        }
    }
}

// ---------------------------------------------------------------------------
// Public API — tracks
// ---------------------------------------------------------------------------
bool NimRTCEngine::add_track(std::string_view id,
                              plugins::MediaKind kind,
                              plugins::Direction dir) noexcept {
    if (!resolve_session_model()) return false;
    if (id.empty()) return false;

    plugins::ISession& s = *impl_->session_state.session_model;
    if (!s.add_track(std::string(id), kind, dir)) return false;

    const std::string mid = bind_track_to_transceiver(id, kind);
    if (mid.empty()) {
        // No transceiver to carry the track — undo the insertion so the
        // session stays consistent.
        s.remove_track(id);
        return false;
    }
    sync_transceiver_direction(mid);
    impl_->session_state.session_dirty = true;
    return true;
}

bool NimRTCEngine::remove_track(std::string_view id) noexcept {
    if (!impl_->session_state.session_model) return false;
    plugins::ISession& s = *impl_->session_state.session_model;

    // Remember which transceiver carried the track so its direction can
    // be recomputed (OD-A: the m-line stays, it turns `inactive`).
    std::string mid;
    for (const auto& t : s.transceivers()) {
        if (t.send_track_id == id || t.recv_track_id == id) {
            mid = t.mid;
            break;
        }
    }

    if (!s.remove_track(id)) return false;
    if (!mid.empty()) sync_transceiver_direction(mid);
    impl_->session_state.session_dirty = true;
    return true;
}

bool NimRTCEngine::set_track_direction(std::string_view id,
                                        plugins::Direction dir) noexcept {
    if (!impl_->session_state.session_model) return false;
    plugins::ISession& s = *impl_->session_state.session_model;

    if (!s.set_track_direction(id, dir)) return false;

    for (const auto& t : s.transceivers()) {
        if (t.send_track_id == id || t.recv_track_id == id) {
            sync_transceiver_direction(t.mid);
            break;
        }
    }
    impl_->session_state.session_dirty = true;
    return true;
}

std::vector<plugins::TrackInfo> NimRTCEngine::tracks() const noexcept {
    if (!impl_->session_state.session_model) return {};
    return impl_->session_state.session_model->tracks();
}

// ---------------------------------------------------------------------------
// Public API — transceivers
// ---------------------------------------------------------------------------

std::string NimRTCEngine::add_transceiver(plugins::MediaKind kind,
                                           plugins::Direction dir) noexcept {
    if (!resolve_session_model()) return {};
    const std::string mid = impl_->session_state.session_model->add_transceiver(kind, dir);
    if (mid.empty()) return {};
    impl_->session_state.session_dirty = true;
    return mid;
}

bool NimRTCEngine::set_transceiver_direction(std::string_view mid,
                                              plugins::Direction dir) noexcept {
    if (!impl_->session_state.session_model) return false;
    if (!impl_->session_state.session_model->set_transceiver_direction(mid, dir)) return false;
    impl_->session_state.session_dirty = true;
    return true;
}

bool NimRTCEngine::stop_transceiver(std::string_view mid) noexcept {
    if (!impl_->session_state.session_model) return false;
    if (!impl_->session_state.session_model->stop_transceiver(mid)) return false;
    impl_->session_state.session_dirty = true;
    return true;
}

bool NimRTCEngine::restart_transceiver(std::string_view mid) noexcept {
    if (!impl_->session_state.session_model) return false;
    if (!impl_->session_state.session_model->restart_transceiver(mid)) return false;
    impl_->session_state.session_dirty = true;
    return true;
}

std::vector<plugins::TransceiverInfo> NimRTCEngine::transceivers() const noexcept {
    if (!impl_->session_state.session_model) return {};
    return impl_->session_state.session_model->transceivers();
}

bool NimRTCEngine::needs_negotiation() const noexcept {
    return impl_->session_state.session_dirty;
}

} // namespace nimrtc::engine
