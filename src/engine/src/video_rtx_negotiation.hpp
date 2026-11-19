/**
 * @file src/engine/src/video_rtx_negotiation.hpp
 * @brief The RFC 4588 retransmission stream's SDP parameters.
 *
 * ## Why this file exists
 *
 * The retransmission stream shows up in three places, and they must all
 * agree:
 *
 *   1. the **offer** — `a=rtpmap:<pt> rtx/90000` + `a=fmtp:<pt> apt=<vpt>`;
 *   2. the **answer** — the same two lines, echoed (RFC 3264 §6.1
 *      constrains an answer to the payload types the offer contained);
 *   3. the **wire** — the retransmissions travel on that payload type,
 *      with their own SSRC, and the SDP declares the pair through
 *      `a=ssrc-group:FID <rtx_ssrc> <media_ssrc>`.
 *
 * Three places means three chances to drift apart, so the *parameters*
 * live here once and the two SDP paths read them.
 *
 * ## The normative constraints (RFC 4588)
 *
 *   - §4: the retransmission payload type MUST differ from the media
 *     one, and the retransmission stream has its **own** sequence
 *     number space.
 *   - §4: the timestamp clock rate MUST equal the original's (90 kHz
 *     for every video codec NimRTC negotiates).
 *   - §4.1: the retransmission payload header is exactly **two octets**
 *     — the OSN, in network byte order.  There is no marker byte.
 *   - §5.2: the retransmission SSRC MUST be distinct from the media
 *     SSRC, and every SSRC in an `a=ssrc-group:FID` MUST have an
 *     `a=ssrc:<ssrc> cname:<cname>` line (RFC 5576 §4.2).
 *   - §8.1: the `apt=` parameter binds the retransmission payload type
 *     to the payload type it protects.
 *
 * ## The payload-type choice
 *
 * The retransmission payload type is a *dynamic* payload type (RFC 3551
 * §4.2 reserves 96..127 for the types that need an `a=rtpmap:` line),
 * and it must not collide with anything else on the wire.
 *
 * The collision rule is the subtle part, and it is **not** per-m-line:
 * with BUNDLE (RFC 8843 §9.1) every bundled m-section shares **one**
 * RTP session, so the payload types of the audio m-line, the video
 * m-line and the retransmission stream all live in **one** number
 * space.  A retransmission payload type that collided with the audio
 * payload type would make the receiver's demux ambiguous — and the
 * demux is exactly what routes a retransmission to the unwrapper.
 *
 * So the number is chosen against *every* payload type the session
 * already uses.
 */

#pragma once

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/rtx/rtx.hpp>
#include <nimrtc/sdp/session_description.hpp>

namespace nimrtc::engine::detail {

/** The first dynamic payload type (RFC 3551 §4.2) that is not already
 *  taken.
 *
 *  RFC 3551 §4.2 reserves 96..127 for the dynamic range, and every
 *  type in it needs an `a=rtpmap:` line — which the retransmission
 *  stream has.
 *
 *  @param used The payload types the session already uses (the audio
 *              m-line's, the video m-line's, the DTMF's).
 *  @return The retransmission payload type, or 0 when the dynamic
 *          range is exhausted (which cannot happen with the two m-lines
 *          NimRTC negotiates, but the caller must handle it). */
inline std::uint8_t resolve_rtx_payload_type(
    std::span<const std::uint8_t> used) noexcept {
    // The dynamic range: RFC 3551 §4.2.
    for (std::uint16_t candidate = 96; candidate <= 127; ++candidate) {
        const auto pt = static_cast<std::uint8_t>(candidate);
        bool taken = false;
        for (const std::uint8_t u : used) {
            if (u == pt) { taken = true; break; }
        }
        if (!taken) return pt;
    }
    return 0;   // the dynamic range is exhausted — no RTX for this session
}

/** The retransmission SSRC derived from the media one.
 *
 *  RFC 4588 §5.2 requires the two SSRCs to be *distinct*.  The
 *  derivation is a simple increment — the value only has to differ, not
 *  to be unpredictable, and a derived value keeps the SDP and the wire
 *  from drifting apart (the SDP line and the RTP header carry the same
 *  number by construction).
 */
inline std::uint32_t derive_rtx_ssrc(std::uint32_t media_ssrc) noexcept {
    return media_ssrc + 1u;
}

/** The `a=rtpmap:` body for the retransmission payload type.
 *
 *  RFC 4588 §8: the encoding name is "rtx" and the clock rate MUST
 *  equal the associated original's (90 kHz for every video codec).
 *
 *  @note Only the **encoding name** belongs here.  The `a=rtpmap:` line
 *        is written by the SDP munger as
 *        `<encoding>/<clock_rate>[/<channels>]`, so the body must be
 *        the bare name — a body that already carried the clock rate
 *        would serialise as `rtx/90000/90000`, which no peer parses.
 */
inline std::string rtx_rtpmap_body() {
    return std::string(rtx::kEncodingName);
}

/** The `a=fmtp:` body for the retransmission payload type.
 *
 *  RFC 4588 §8.1: the `apt=` parameter is the *association* — it says
 *  which payload type the retransmission protects.  The `rtx-time`
 *  parameter is OPTIONAL and says how long the sender keeps a packet
 *  available; it is emitted only when the host configured one.
 *
 *  @param apt         The payload type the retransmission protects.
 *  @param rtx_time_ms The `rtx-time` value in milliseconds, or 0 to omit
 *                     the parameter.
 */
inline std::string rtx_fmtp_body(std::uint8_t apt, std::uint32_t rtx_time_ms) {
    std::optional<std::uint32_t> rtx_time;
    if (rtx_time_ms != 0) rtx_time = rtx_time_ms;
    return rtx::build_fmtp(apt, rtx_time);
}

/** The `a=ssrc-group:FID` body binding the retransmission SSRC to the
 *  media one.
 *
 *  RFC 5576 §4.2: the FID semantics relate a *primary* source to its
 *  *dependent* ones, and the primary comes **first**.  The WebRTC
 *  convention (Chrome, pion, libwebrtc) follows it: the line reads
 *
 *  ```
 *  a=ssrc-group:FID <media_ssrc> <rtx_ssrc>
 *  ```
 *
 *  so the *second* SSRC is the retransmission ("repair") flow of the
 *  first.  Getting the order backwards is a real interop hazard: a
 *  receiver that follows the convention would treat the media stream as
 *  the repair flow and drop the picture.
 *
 *  RFC 5576 §4.2 also requires that every SSRC named in the group has
 *  its own `a=ssrc:<ssrc> cname:<cname>` line in the same media
 *  description — which is why the caller emits one for the
 *  retransmission SSRC as well.
 */
inline std::string rtx_fid_group_body(std::uint32_t rtx_ssrc,
                                      std::uint32_t media_ssrc) {
    return "FID " + std::to_string(media_ssrc) + " " +
           std::to_string(rtx_ssrc);
}

/** The `a=ssrc:<ssrc> cname:<cname>` body for the retransmission SSRC.
 *
 *  RFC 5576 §4.1 gives the line's shape; the CNAME identifies the
 *  *participant*, so the retransmission stream carries the **same** CNAME
 *  as the media stream (RFC 3550 §8.2: one CNAME per participant).
 */
inline std::string rtx_ssrc_cname_body(std::uint32_t rtx_ssrc,
                                       std::string_view cname) {
    return std::to_string(rtx_ssrc) + " cname:" + std::string(cname);
}

/** The peer's retransmission SSRC, read from its `a=ssrc-group:FID`
 *  line.
 *
 *  RFC 5576 §4.2: the FID group lists the *primary* source first and
 *  its *dependent* (repair) sources after it, so the **second** SSRC of
 *  a two-element group is the retransmission stream's SSRC.
 *
 *  @param media          The peer's media description.
 *  @param our_media_ssrc Our own media SSRC — never the peer's, so it
 *                        only serves as a tie-break when the peer listed
 *                        more than two SSRCs.
 *  @return The peer's retransmission SSRC, or 0 when the peer advertised
 *          no FID group (in which case the peer multiplexes its
 *          retransmissions onto the media SSRC, which RFC 4588 §5.2
 *          permits when the peer does not use SSRC multiplexing). */
inline std::uint32_t remote_rtx_ssrc_of(const sdp::MediaDescription& media,
                                        std::uint32_t our_media_ssrc) {
    for (const auto& [key, value] : media.extra_attrs) {
        if (key != "ssrc-group") continue;
        if (value.rfind("FID", 0) != 0) continue;

        // "FID <ssrc> <ssrc> ..." — the primary is first.
        std::vector<std::string> tokens;
        std::string token;
        for (const char c : value) {
            if (c == ' ') {
                if (!token.empty()) tokens.push_back(token);
                token.clear();
            } else {
                token.push_back(c);
            }
        }
        if (!token.empty()) tokens.push_back(token);
        if (tokens.size() < 3) continue;   // FID + at least two SSRCs

        for (std::size_t i = 2; i < tokens.size(); ++i) {
            const std::string& tok = tokens[i];
            if (tok.empty() || tok.size() > 10) continue;
            bool all_digits = true;
            for (const char c : tok) {
                if (c < '0' || c > '9') { all_digits = false; break; }
            }
            if (!all_digits) continue;
            const auto ssrc = static_cast<std::uint32_t>(
                std::strtoul(tok.c_str(), nullptr, 10));
            // Our own SSRC can never be the peer's retransmission SSRC.
            if (ssrc == 0 || ssrc == our_media_ssrc) continue;
            return ssrc;
        }
    }
    return 0;
}

} // namespace nimrtc::engine::detail
