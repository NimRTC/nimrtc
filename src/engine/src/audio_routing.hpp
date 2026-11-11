/**
 * @file src/engine/src/audio_routing.hpp
 * @brief WP-6 (spec B-4 / B-14) — the inbound payload-type routing table
 *        and the Opus `a=fmtp` parameter parser / builder.
 *
 * PRIVATE engine internals.  This header is not installed, and no public
 * header includes it — the public surface of the audio pipeline is the
 * two plugin seams (`plugins::IAudioDecoder`, `plugins::IAudioReceiver`),
 * never an engine-internal routing type (constraint L4).
 *
 * ## Why this exists
 *
 * Before WP-6 the engine demultiplexed inbound RTP with a single rule:
 * "if the payload type is the video one, it is video; otherwise it is
 * audio".  That is defect F-8 from the specification: a packet on any
 * other payload type — a telephone-event, a RED stream, a codec we do
 * not implement — was pushed into the audio pipeline and handed to the
 * decoder as if it were audio.
 *
 * The fix is a *negotiated routing table*: the peer's own `a=rtpmap:`
 * lines say which payload types it will actually send, and only those
 * are decoded.  Anything else is counted and dropped with
 * `kAudioPayloadTypeUnknown`.
 *
 * ## The two tables
 *
 *   - `audio_pt_routes` — the payload types the peer declared on its
 *     *audio* m-line, together with the encoding, the media clock rate
 *     and the channel count.
 *   - `video_pt_routes` — the same for the *video* m-line.
 *
 * They are kept apart because a payload type is only meaningful within
 * the m-section that declared it (RFC 3550 §5.14: the payload type is
 * scoped to the RTP session, and an m-line that is rejected contributes
 * no RTP session at all).
 */

#pragma once

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace nimrtc::engine::detail {

// ---------------------------------------------------------------------------
// Payload-type routing (B-4)
// ---------------------------------------------------------------------------

/** One negotiated inbound payload type. */
struct AudioPayloadRoute {
    /** The RTP encoding name, lower-cased ("opus", "pcmu", ...). */
    std::string   encoding;
    /** The media clock rate the `a=rtpmap:` line declared, in Hz. */
    std::uint32_t clock_rate = 0;
    /** The channel count the `a=rtpmap:` line declared. */
    std::uint8_t  channels = 0;
};

/** The static payload types of RFC 3551 §4.5 that we know by heart.
 *
 *  A peer may omit the `a=rtpmap:` line for a static payload type
 *  (RFC 3551 §4.5 makes it optional), so the routing table has to be
 *  able to fill those in on its own.  Only the two mandatory WebRTC
 *  codecs are listed; the rest of the static range is either a
 *  telephone-event or a codec we cannot decode, and an unlisted static
 *  payload type simply produces no route.
 *
 *  @param pt    The payload type.
 *  @param out   Filled with the static assignment when one exists.
 *  @return      true when @p pt has a static assignment. */
inline bool static_audio_payload_type(std::uint8_t pt,
                                      AudioPayloadRoute& out) noexcept {
    switch (pt) {
        case 0:  out = AudioPayloadRoute{"pcmu", 8000, 1}; return true;  // RFC 3551 §4.5.14
        case 8:  out = AudioPayloadRoute{"pcma", 8000, 1}; return true;  // RFC 3551 §4.5.14
        default: return false;
    }
}

/** The id under which a decoder plugin for this encoding is registered.
 *
 *  The registry's ids are per category (DEC-6), and the Opus decoder
 *  plugin registers under the id "opus" — the same word as the RTP
 *  encoding name.  The mapping is therefore the identity on the
 *  lower-cased encoding name, which also lets a host register its own
 *  decoder under the encoding it implements ("pcmu", "g722", ...).
 *
 *  @param encoding  The RTP encoding name, in any case.
 *  @return          The lower-cased name. */
inline std::string decoder_id_for_encoding(std::string_view encoding) {
    std::string out;
    out.reserve(encoding.size());
    for (char c : encoding) {
        out.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Opus format parameters (RFC 7587 §7.1)
// ---------------------------------------------------------------------------

/** The Opus `a=fmtp` parameters (RFC 7587 §7.1).
 *
 *  The ones NimRTC acts on are:
 *
 *   - `minptime` / `maxptime` — the packet durations the peer will use
 *     (RFC 6716 §4 allows 10 / 20 / 40 / 60 ms).
 *   - `useinbandfec` — the peer embeds the redundancy of the previous
 *     frame in the next packet, which is what the decoder uses to
 *     reconstruct a lost frame (B-8).  NimRTC enables it, so it must be
 *     signalled.
 *   - `stereo` / `sprop-stereo` — the channel count of the *stream*.
 *     The `a=rtpmap:` channel field is the number of channels the
 *     encoding *supports* (always 2 for Opus), so the actual count of
 *     the stream lives in the format parameters (A-5).
 *
 *  `usedtx` and `sprop-maxcapturesize` are parsed but not acted on:
 *  discontinuous transmission and the capture-size limit belong to the
 *  P2 line (WP-12).
 *
 *  Every field is optional, because "the peer did not say" and "the
 *  peer said zero" are different facts — the answer must echo only
 *  what was actually negotiated (B-4). */
struct OpusFmtpParams {
    std::optional<std::uint32_t> minptime_ms;
    std::optional<std::uint32_t> maxptime_ms;
    std::optional<bool>         useinbandfec;
    std::optional<bool>         usedtx;
    std::optional<bool>         stereo;
    std::optional<bool>         sprop_stereo;
    std::optional<std::uint32_t> sprop_maxcapturesize;
};

/** Parse a decimal unsigned integer.  Returns nothing on a non-digit. */
inline std::optional<std::uint32_t> parse_uint_token(std::string_view tok) noexcept {
    if (tok.empty() || tok.size() > 10) return std::nullopt;
    std::uint32_t v = 0;
    for (char c : tok) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + static_cast<std::uint32_t>(c - '0');
    }
    return v;
}

/** Parse the boolean form SDP uses: "1" / "0". */
inline std::optional<bool> parse_bool_token(std::string_view tok) noexcept {
    if (tok == "1") return true;
    if (tok == "0") return false;
    return std::nullopt;
}

/** Trim ASCII whitespace off both ends of a view. */
inline std::string_view trim_ascii(std::string_view sv) noexcept {
    while (!sv.empty() &&
           std::isspace(static_cast<unsigned char>(sv.front()))) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() &&
           std::isspace(static_cast<unsigned char>(sv.back()))) {
        sv.remove_suffix(1);
    }
    return sv;
}

/** Parse the Opus `a=fmtp:` parameter string.
 *
 *  The syntax is a `;`-separated list of `key=value` pairs (RFC 7587
 *  §7.1, RFC 8866 §5).  The keys are matched case-insensitively, and
 *  an unknown key is ignored rather than rejected — a peer that sends
 *  a parameter we do not know must still be negotiable.
 *
 *  @param raw  The parameter string, without the `a=fmtp:<pt> ` prefix.
 *  @return     The recognised parameters; unset fields mean "not said". */
inline OpusFmtpParams parse_opus_fmtp(std::string_view raw) noexcept {
    OpusFmtpParams p;
    std::size_t pos = 0;
    while (pos <= raw.size()) {
        const std::size_t semi = raw.find(';', pos);
        const std::string_view tok = trim_ascii(
            raw.substr(pos, semi == std::string_view::npos
                              ? std::string_view::npos
                              : semi - pos));
        if (semi == std::string_view::npos) {
            pos = raw.size() + 1;
        } else {
            pos = semi + 1;
        }
        if (tok.empty()) continue;

        const std::size_t eq = tok.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string key = decoder_id_for_encoding(tok.substr(0, eq));
        const std::string_view val = trim_ascii(tok.substr(eq + 1));

        if (key == "minptime") {
            if (auto v = parse_uint_token(val)) p.minptime_ms = *v;
        } else if (key == "maxptime") {
            if (auto v = parse_uint_token(val)) p.maxptime_ms = *v;
        } else if (key == "useinbandfec") {
            if (auto v = parse_bool_token(val)) p.useinbandfec = *v;
        } else if (key == "usedtx") {
            if (auto v = parse_bool_token(val)) p.usedtx = *v;
        } else if (key == "stereo") {
            if (auto v = parse_bool_token(val)) p.stereo = *v;
        } else if (key == "sprop-stereo") {
            if (auto v = parse_bool_token(val)) p.sprop_stereo = *v;
        } else if (key == "sprop-maxcapturesize") {
            if (auto v = parse_uint_token(val)) p.sprop_maxcapturesize = *v;
        }
    }
    return p;
}

/** Render the parameters back into an `a=fmtp:` parameter string.
 *
 *  Only the fields that are set are written — an unset field means the
 *  peer never said anything about it, and inventing a value here would
 *  claim a negotiation that did not happen.
 *
 *  @param p  The parameters.
 *  @return   The parameter string, without the `a=fmtp:<pt> ` prefix. */
inline std::string build_opus_fmtp(const OpusFmtpParams& p) {
    std::string out;
    auto add = [&out](const char* key, const std::string& value) {
        if (!out.empty()) out += ';';
        out += key;
        out += '=';
        out += value;
    };
    if (p.minptime_ms) add("minptime", std::to_string(*p.minptime_ms));
    if (p.maxptime_ms) add("maxptime", std::to_string(*p.maxptime_ms));
    if (p.useinbandfec) add("useinbandfec", *p.useinbandfec ? "1" : "0");
    if (p.stereo)       add("stereo", *p.stereo ? "1" : "0");
    if (p.sprop_stereo) add("sprop-stereo", *p.sprop_stereo ? "1" : "0");
    return out;
}

/** The `a=fmtp` line the engine advertises for its own outbound Opus
 *  stream (B-14).
 *
 *  The parameters describe what NimRTC actually does:
 *
 *   - `minptime=10` — the smallest Opus frame (RFC 6716 §4), which is
 *     also the smallest packet duration the engine accepts.
 *   - `useinbandfec=1` — the decoder reconstructs a lost frame from
 *     the redundancy the peer embeds in the next packet (B-8), so the
 *     sender has to be told to embed it.
 *   - `stereo` / `sprop-stereo` — the channel count of the *stream*.
 *     Opus' `a=rtpmap:` channel field is the maximum the encoding
 *     supports (always 2), so the real count lives here.
 *
 *  @param stream_channels  The channel count of the stream we send.
 *  @return                 The parameter string. */
inline std::string build_local_opus_fmtp(std::uint8_t stream_channels) {
    OpusFmtpParams p;
    p.minptime_ms  = 10;
    p.useinbandfec = true;
    const bool stereo = stream_channels >= 2;
    p.stereo       = stereo;
    p.sprop_stereo = stereo;
    return build_opus_fmtp(p);
}

/** The `a=fmtp` line the answer echoes for the negotiated payload type.
 *
 *  B-4: "The answer SDP echoes the negotiated values back, but only the
 *  subset NimRTC actually supports."  The parameters the peer sent are
 *  echoed, minus the ones NimRTC does not act on (`usedtx`,
 *  `sprop-maxcapturesize`).  The packet durations are clamped to the
 *  Opus frame sizes, because a peer that names a duration Opus cannot
 *  produce (RFC 6716 §4) would otherwise negotiate a frame size the
 *  decoder has no way to honour.
 *
 *  @param peer  The parameters the peer sent.
 *  @return      The parameter string; empty when there is nothing to
 *               echo. */
inline std::string build_negotiated_opus_fmtp(const OpusFmtpParams& peer) {
    OpusFmtpParams out;
    out.minptime_ms  = peer.minptime_ms;
    out.maxptime_ms  = peer.maxptime_ms;
    out.useinbandfec = peer.useinbandfec;
    out.stereo       = peer.stereo;
    out.sprop_stereo = peer.sprop_stereo;

    // RFC 6716 §4: Opus frames are 10 / 20 / 40 / 60 ms.  A peer that
    // names anything else gets the nearest legal value, and the frame
    // duration the receiver plans with follows `maxptime`.
    auto clamp_frame_ms = [](std::optional<std::uint32_t>& v) {
        if (!v) return;
        const std::uint32_t x = *v;
        if (x <= 10)      { *v = 10; return; }
        if (x <= 20)      { *v = 20; return; }
        if (x <= 40)      { *v = 40; return; }
        *v = 60;
    };
    clamp_frame_ms(out.minptime_ms);
    clamp_frame_ms(out.maxptime_ms);
    if (out.maxptime_ms && out.minptime_ms &&
        *out.maxptime_ms < *out.minptime_ms) {
        out.maxptime_ms = out.minptime_ms;
    }
    return build_opus_fmtp(out);
}

// ---------------------------------------------------------------------------
// The ssrc-audio-level header extension (B-14)
// ---------------------------------------------------------------------------
/** The RTP header extension URI for the audio level (RFC 6464). */
inline constexpr std::string_view kAudioLevelExtUri =
    "urn:ietf:params:rtp-hdrext:ssrc-audio-level";

/** The extension id we advertise for the audio level.
 *
 *  RFC 5285 §6 negotiates the id per direction, so the value is only
 *  "the id we use"; the peer's id is whatever its own description
 *  says, and the answer echoes that one.  Id 1 is the id Chrome has
 *  used for this extension for years, and it does not collide with
 *  the abs-send-time id (4) that shares the same m-line. */
inline constexpr int kAudioLevelDefaultLocalId = 1;

// ---------------------------------------------------------------------------
// Error reporting policy (spec Part D)
// ---------------------------------------------------------------------------

/** How many unknown-payload-type packets may pass before the next one is
 *  reported through `on_error`.
 *
 *  Spec Part D: per-packet errors are "counted but not reported
 *  individually — the first occurrence in a window and every N-th
 *  occurrence are reported (default: first + every 50th)".  Reporting
 *  every drop would flood the host when a peer sends a stream we never
 *  negotiated, so the counter always advances and only the first and
 *  every N-th occurrence reach the callback. */
inline constexpr std::uint64_t kAudioPayloadTypeReportInterval = 50;

} // namespace nimrtc::engine::detail
