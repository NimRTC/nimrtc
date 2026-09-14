/**
 * @file session.hpp
 * @brief Session orchestration — Stream and Session management.
 *
 * P1 milestone (NimRTC-V2 §5 scope list: Session BUNDLE/rtcp-mux).
 *
 * ## Design notes
 *
 * - **PIMPL**: all classes hold a `unique_ptr<Impl>` to keep the public header
 *   API stable and minimise compile-time coupling.
 *
 * - **BUNDLE**: `Session::bundle_tracks()` marks the given tracks as bundled.
 *   The caller (e.g. engine.cpp) is responsible for emitting the SDP
 *   `a=group:BUNDLE mid1 mid2 …` line.
 *
 * - **rtcp-mux**: `Session::set_rtcp_mux()` records the preference.  The
 *   actual socket multiplexing is handled by the ICE transport.
 */

#ifndef NIMRTC_SESSION_SESSION_HPP
#define NIMRTC_SESSION_SESSION_HPP

#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nimrtc/session/track.hpp>

namespace nimrtc::session {

// Forward declaration already provided by track.hpp (Track is defined there).

// ---------------------------------------------------------------------------
// Stream — a group of Tracks (corresponds to WebRTC MediaStream)
// ---------------------------------------------------------------------------
class Stream {
public:
    /** Construct a stream with the given identity. */
    explicit Stream(std::string id);
    ~Stream();

    Stream(const Stream&)            = delete;
    Stream& operator=(const Stream&) = delete;
    Stream(Stream&&)                 = default;
    Stream& operator=(Stream&&)      = default;

    // ---- Identity ----
    [[nodiscard]] std::string id() const noexcept { return impl_->id; }

    // ---- Track management ----
    void add_track(std::shared_ptr<Track> track);
    void remove_track(const std::string& track_id);
    [[nodiscard]] std::vector<std::shared_ptr<Track>> tracks() const;
    [[nodiscard]] std::shared_ptr<Track> track_by_id(const std::string& id) const;

private:
    struct Impl {
        explicit Impl(std::string id_) : id(std::move(id_)) {}
        std::string id;
        // Track storage: shared_ptr so Stream::tracks() can return copies.
        // Ordered map for stable iteration.
        std::unordered_map<std::string, std::shared_ptr<Track>> tracks_;
        std::shared_mutex mu;  // protects tracks_
    };
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Session — top-level session manager
// ---------------------------------------------------------------------------
class Session {
public:
    Session();
    ~Session();

    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&)                 = default;
    Session& operator=(Session&&)      = default;

    // -------------------------------------------------------------------------
    // BUNDLE — multiple tracks share one ICE component
    // -------------------------------------------------------------------------

    /** Mark the given track IDs as belonging to the same BUNDLE group.
     *  All bundled tracks multiplex over a single ICE transport component.
     *  Calling this multiple times replaces the previous BUNDLE set. */
    void bundle_tracks(const std::vector<std::string>& track_ids);

    /** Query the set of bundled track IDs. */
    [[nodiscard]] std::vector<std::string> bundled_track_ids() const;

    // -------------------------------------------------------------------------
    // rtcp-mux — RTCP multiplexed over the RTP port (RFC 5761)
    // -------------------------------------------------------------------------

    void set_rtcp_mux(bool enabled) noexcept { rtcp_mux_ = enabled; }
    [[nodiscard]] bool rtcp_mux() const noexcept { return rtcp_mux_; }

    // -------------------------------------------------------------------------
    // Stream management
    // -------------------------------------------------------------------------

    void add_stream(std::shared_ptr<Stream> stream);
    void remove_stream(const std::string& stream_id);
    [[nodiscard]] std::vector<std::shared_ptr<Stream>> streams() const;

private:
    struct Impl {
        std::unordered_map<std::string, std::shared_ptr<Stream>> streams_;
        std::unordered_set<std::string> bundled_track_ids_;
        std::shared_mutex mu;
    };
    std::unique_ptr<Impl> impl_;
    bool rtcp_mux_ = false;
};

} // namespace nimrtc::session

#endif // NIMRTC_SESSION_SESSION_HPP
