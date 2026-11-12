/**
 * @file video_codec_config.hpp
 * @brief The video codec the engine negotiated, and how it maps onto the
 *        SDP / plugin seams.
 *
 * ## Why this file exists
 *
 * The engine holds *one* video codec for a session, but that codec shows
 * up in four different places, each of which spells the codec differently:
 *
 *   1. `EngineConfig::video_codec_name` — a plugin id ("h264" / "h265" /
 *      "av1"), the way the user selects it.
 *   2. `plugins::VideoCodecConfig::codec` — a `VideoCodecKind` enum, the
 *      way the plugin layer names it.
 *   3. `a=rtpmap:<pt> <encoding>/90000` — the SDP encoding name
 *      ("H264" / "H265" / "AV1"), the way the *peer* names it.
 *   4. `a=fmtp:<pt> ...` — the codec's format parameters, whose *keys*
 *      differ per codec family.
 *
 * Keeping the mapping in one place is what stops the four from drifting
 * apart: the offer says "H265", the receiver config says "kH265" and the
 * plugin id says "h265", and all three come from the same table here.
 *
 * The table lives in the *engine* (not in a codec module) because the
 * engine is the only place that sees all four representations at once —
 * and per Layout Invariant 4 the engine may not include a concrete codec
 * module's header, so the mapping cannot live in `modules/h265` either.
 */

#pragma once

#include <cstdint>
#include <string_view>

#include <nimrtc/plugins/video_codec.hpp>

namespace nimrtc::engine::detail {

/** The video codec families the engine can negotiate. */
enum class VideoCodecFamily : std::uint8_t {
    kUnknown = 0,
    kH264    = 1,
    kH265    = 2,
    kAV1     = 3,
};

/** The SDP spelling of a codec family: "H264" / "H265" / "AV1".
 *  This is what goes into the `a=rtpmap:` line's encoding field. */
inline constexpr std::string_view sdp_encoding_of(VideoCodecFamily f) noexcept {
    switch (f) {
        case VideoCodecFamily::kH264: return "H264";
        case VideoCodecFamily::kH265: return "H265";
        case VideoCodecFamily::kAV1:  return "AV1";
        default:                      return {};
    }
}

/** The plugin id of a codec family: "h264" / "h265" / "av1".
 *  This is the `EngineConfig::video_codec_name` spelling and the
 *  `core::PluginRegistry` video-codec slot key. */
inline constexpr std::string_view plugin_id_of(VideoCodecFamily f) noexcept {
    switch (f) {
        case VideoCodecFamily::kH264: return "h264";
        case VideoCodecFamily::kH265: return "h265";
        case VideoCodecFamily::kAV1:  return "av1";
        default:                      return {};
    }
}

/** The conventional payload type of a codec family.
 *
 *  H.264 uses the static payload type 102 (RFC 6184).  H.265 and AV1
 *  have no statically-assigned type in RFC 3551, so they live in the
 *  dynamic range 96..127 — the range RFC 3551 §4.2 reserves for
 *  "dynamic" payload types that *must* be described by an `a=rtpmap:`
 *  line.
 *
 *  H.265 uses 116 and AV1 uses 45:
 *
 *    - **H.265 = 116** is the value Chrome's WebRTC stack uses for
 *      HEVC (the "HEVC" payload type in Chrome's `kVideoCodecMimeType`
 *      mapping).  RFC 7798 §4.1 lists 116 among the *recommended*
 *      dynamic payload types for H.265.
 *    - **AV1 = 45** is the value the AV1 RTP payload specification
 *      (draft-ietf-payload-av1 §10) and Chrome's Unified Plan both
 *      use for AV1 in the video m-line.
 *
 *  Both numbers are conventions, not assignments: a peer may pick a
 *  different dynamic payload type, and the negotiation code picks
 *  whatever the peer offers.  These are only the *defaults* NimRTC
 *  advertises when the host did not pin one. */
inline constexpr std::uint8_t default_payload_type_of(VideoCodecFamily f) noexcept {
    switch (f) {
        case VideoCodecFamily::kH264: return 102;
        case VideoCodecFamily::kH265: return 116;
        case VideoCodecFamily::kAV1:  return 45;
        default:                      return 102;
    }
}

/** The plugin-layer codec kind of a codec family. */
inline constexpr plugins::VideoCodecKind codec_kind_of(VideoCodecFamily f) noexcept {
    switch (f) {
        case VideoCodecFamily::kH264: return plugins::VideoCodecKind::kH264;
        case VideoCodecFamily::kH265: return plugins::VideoCodecKind::kH265;
        case VideoCodecFamily::kAV1:  return plugins::VideoCodecKind::kAV1;
        default:                      return plugins::VideoCodecKind::kUnknown;
    }
}

/** Map a plugin id onto the codec family.
 *
 *  The id is a *fallback* source of the family: it is used to look the
 *  factory up, and — only when the instantiated codec's `kind()`
 *  returns `kUnknown` — to name the family.
 *
 *  A plugin id is accepted when it is **exactly** one of the canonical
 *  names, or when it *starts with* one of them followed by a
 *  separator.  The prefix rule is what lets a vendor register
 *  "h265-nvenc" or "av1-daemon" and still have the engine derive the
 *  right family from the id alone:
 *
 *    "h264"        → kH264
 *    "h264-openh264" → kH264
 *    "h265-nvenc"  → kH265
 *    "hevc"        → kH265
 *    "av1"         → kAV1
 *    "av1-daemon"  → kAV1
 *
 *  The separator must be one of `-`, `_` or `.` — a bare prefix match
 *  would mis-classify a hypothetical "h2650" as H.265.
 *
 *  @return kUnknown when the id names no codec the engine supports. */
inline VideoCodecFamily codec_family_from_plugin_id(std::string_view id) noexcept {
    // The comparison is case-insensitive because a user may write
    // "H265" in the config; the registry's slot key is lower-case.
    auto lower = [](char c) noexcept -> char {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };
    auto eq_ci = [&](std::string_view a, std::string_view b) noexcept {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (lower(a[i]) != lower(b[i])) return false;
        }
        return true;
    };
    auto starts_with_ci = [&](std::string_view a, std::string_view prefix) noexcept {
        if (a.size() < prefix.size()) return false;
        for (std::size_t i = 0; i < prefix.size(); ++i) {
            if (lower(a[i]) != lower(prefix[i])) return false;
        }
        return true;
    };
    // A trailing separator ("h265-") is still a match.
    auto matches = [&](std::string_view canonical) noexcept {
        if (eq_ci(id, canonical)) return true;
        if (!starts_with_ci(id, canonical)) return false;
        if (id.size() == canonical.size()) return true;
        const char sep = id[canonical.size()];
        return sep == '-' || sep == '_' || sep == '.';
    };

    if (matches("h264")) return VideoCodecFamily::kH264;
    if (matches("h265") || matches("hevc")) return VideoCodecFamily::kH265;
    if (matches("av1"))  return VideoCodecFamily::kAV1;
    return VideoCodecFamily::kUnknown;
}

/** Map an SDP encoding name ("H264" / "H265" / "AV1", case-insensitive)
   *  onto the codec family. */
inline VideoCodecFamily codec_family_from_sdp_encoding(std::string_view enc) noexcept {
    return codec_family_from_plugin_id(enc);
}

/** Map a plugin-layer codec kind onto the codec family.
 *
 *  This is the inverse of `codec_kind_of`.  The engine uses it after the
 *  codec plugin has been instantiated: `IVideoCodec::kind()` is the
 *  codec's own claim about the wire format it produces, which is the
 *  authoritative source for the SDP `a=rtpmap:` encoding name. */
inline VideoCodecFamily codec_family_from_codec_kind(
        plugins::VideoCodecKind kind) noexcept {
    switch (kind) {
        case plugins::VideoCodecKind::kH264: return VideoCodecFamily::kH264;
        case plugins::VideoCodecKind::kH265: return VideoCodecFamily::kH265;
        case plugins::VideoCodecKind::kAV1:  return VideoCodecFamily::kAV1;
        default:                             return VideoCodecFamily::kUnknown;
    }
}

/** Resolve the *effective* RTP payload type of a codec family.
 *
 *  The host may pin a payload type explicitly (`EngineConfig::
 *  video_sender_tuning.payload_type`), in which case that number wins.
 *  When the host left the H.264 default (102) in place while asking for
 *  a different codec family, the number is meaningless for that family —
 *  RFC 6184's 102 is the *suggested* payload type for H.264 and no other
 *  codec may reuse it — so the family's own conventional default is used
 *  instead.
 *
 *  @param family      The negotiated codec family.
 *  @param configured  The payload type the host configured (0 = unset).
 *  @return The payload type to advertise and to stamp on the wire. */
inline std::uint8_t resolve_payload_type(VideoCodecFamily family,
                                          std::uint8_t configured) noexcept {
    if (family == VideoCodecFamily::kUnknown) {
        return configured != 0 ? configured : default_payload_type_of(family);
    }
    const std::uint8_t dflt = default_payload_type_of(family);
    if (configured == 0) return dflt;
    // 102 is H.264's conventional payload type.  Leaving it in place
    // while selecting another codec family means "I did not choose a
    // payload type" — not "I want 102 for H.265".
    if (configured == default_payload_type_of(VideoCodecFamily::kH264) &&
        family != VideoCodecFamily::kH264) {
        return dflt;
    }
    return configured;
}

/** The `a=fmtp:` body the engine advertises for a codec family.
 *
 *  H.264's body mirrors the synthetic SPS/PPS the H.264 stub encoder
 *  emits (Baseline profile, level 3.1).  H.265's body names the HEVC
 *  profile / level / tier and the temporal-dependency mode, and
 *  deliberately omits the parameter sets: the WebRTC HEVC profile
 *  (draft-ietf-avtcore-hevc-webrtc §4) forbids `sprop-vps` /
 *  `sprop-sps` / `sprop-pps` in the SDP — the parameter sets travel
 *  in-band, inside the stream.  AV1's body names the profile and the
 *  level index, which is what Chrome's Unified Plan emits.
 *
 *  @param family  The codec family.
 *  @param with_sprop  Whether to include the H.264 `sprop-parameter-sets`
 *                    (the stub encoder advertises them; a hardware
 *                    encoder would override them).
 */
inline std::string_view default_fmtp_of(VideoCodecFamily family,
                                        bool with_sprop) noexcept {
    switch (family) {
        case VideoCodecFamily::kH264:
            return with_sprop
                ? "profile-level-id=42E01F;packetization-mode=1;"
                  "level-asymmetry-allowed=1;"
                  "sprop-parameter-sets=Z0LAHtkA,aM4G4g=="
                : "profile-level-id=42E01F;packetization-mode=1";
        case VideoCodecFamily::kH265:
            // RFC 7798 §4.2: `level-id` carries HEVC's `general_level_idc`
            // verbatim — 93 is level 3.1 (the RFC's own default when the
            // parameter is absent).  profile-id=1 is Main, tier-flag=0 is
            // main tier, and tx-mode is the *string* token "SRST"
            // (sub-layer transmission, sub-layer switching only) — RFC
            // 7798 §4.4 defines it as one of "SRST" / "MRST" / "MRMT",
            // and "SRST" is what a single-temporal-layer WebRTC stream
            // uses.  NimRTC advertises level 3.1 to match the 640x480@30
            // default geometry.
            return "profile-id=1;level-id=93;tier-flag=0;tx-mode=SRST";
        case VideoCodecFamily::kAV1:
            // draft-ietf-payload-av1 §7 / Chrome Unified Plan:
            // profile=0 (Main), level-idx=5 (level 3.1), tier=0.
            return "profile=0;level-idx=5;tier=0";
        default:
            return {};
    }
}

} // namespace nimrtc::engine::detail
