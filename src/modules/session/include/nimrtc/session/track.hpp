/**
 * @file track.hpp
 * @brief Track — single media track (audio / video / data).
 *
 * P1 milestone (NimRTC-V2 §5 scope list: Session BUNDLE/rtcp-mux).
 *
 * A Track is a unidirectional media source or sink.  It maps to a single
 * SDP m-line and carries SSRC, direction, and BUNDLE state.
 */

#ifndef NIMRTC_SESSION_TRACK_HPP
#define NIMRTC_SESSION_TRACK_HPP

#include <cstdint>
#include <memory>
#include <string>

namespace nimrtc::session {

// ---------------------------------------------------------------------------
// Media type
// ---------------------------------------------------------------------------
enum class MediaType {
    kAudio,
    kVideo,
    kData,
};

// ---------------------------------------------------------------------------
// Track direction (RFC 8829 / WebRTC semantics)
// ---------------------------------------------------------------------------
enum class TrackDirection {
    kSendOnly,   // offer=sendonly / answer=recvonly
    kRecvOnly,   // offer=recvonly / answer=sendonly
    kSendRecv,   // offer=sendrecv / answer=sendrecv
    kInactive,   // offer=inactive / answer=inactive
};

// ---------------------------------------------------------------------------
// Track — single media track (PIMPL)
// ---------------------------------------------------------------------------
class Track {
public:
    /**
     * Construct a track with the given identity and type.
     * Direction defaults to kSendRecv.
     */
    Track(std::string id, MediaType type, TrackDirection dir = TrackDirection::kSendRecv);
    ~Track();

    Track(const Track&)            = delete;
    Track& operator=(const Track&) = delete;
    Track(Track&&)                 = default;
    Track& operator=(Track&&)      = default;

    // ---- Identity ----
    [[nodiscard]] std::string id()   const noexcept { return id_; }
    [[nodiscard]] MediaType   type()  const noexcept { return type_; }
    [[nodiscard]] TrackDirection direction() const noexcept { return direction_; }

    // ---- Direction ----
    void set_direction(TrackDirection dir) noexcept { direction_ = dir; }

    // ---- SSRC (maps to SDP a=ssrc attribute) ----
    void          set_ssrc(std::uint32_t ssrc)       noexcept { ssrc_ = ssrc; has_ssrc_ = true; }
    [[nodiscard]] std::uint32_t ssrc()        const noexcept { return ssrc_; }
    [[nodiscard]] bool          has_ssrc()     const noexcept { return has_ssrc_; }

    // ---- BUNDLE: multiple Tracks share a single ICE component ----
    // When bundled=true, this track's RTP/RTCP multiplex over the transport
    // component used by the BUNDLE tag mid (RFC 8829 §5.2.1).
    void          set_bundled(bool bundled) noexcept { bundled_ = bundled; }
    [[nodiscard]] bool          bundled()     const noexcept { return bundled_; }

private:
    std::string      id_;
    MediaType        type_;
    TrackDirection   direction_;

    std::uint32_t    ssrc_     = 0;
    bool             has_ssrc_ = false;
    bool             bundled_  = false;
};

} // namespace nimrtc::session

#endif // NIMRTC_SESSION_TRACK_HPP
