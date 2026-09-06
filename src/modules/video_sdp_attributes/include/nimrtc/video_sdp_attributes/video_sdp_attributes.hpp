/**
 * @file nimrtc/video_sdp_attributes/video_sdp_attributes.hpp
 * @brief Codec-specific SDP attribute parsers for video (H.264/VP8/VP9).
 *
 * Per ARCHITECTURE.md §5 (Codec payload fmtp) and §6 (Codec):
 *   - Video codecs carry per-stream parameters in `a=fmtp:<pt> <key>=<val>;...`
 *     and codec identification in `a=rtpmap:<pt> <name>/<clock>`.
 *   - For Chrome / Firefox P2P video interop (Tier 0, §4.2) the engine
 *     MUST be able to read & emit these parameters correctly.
 *
 * This module is the codec-specific helper that sits on top of the
 * generic SDP parser (`modules/sdp`) and the video payload module. It
 * does NOT touch the wire — it only translates between SDP strings and
 * strongly-typed codec config structs.
 *
 * ## Scope
 *
 *   - **H.264** (RFC 6184 §8): `packetization-mode`, `profile-level-id`,
 *     `sprop-parameter-sets`, `max-mbps`, `max-fs`, `max-smbps`,
 *     `max-br`, `max-dpb`, `level-asymmetry-allowed`.
 *
 *   - **VP8** (RFC 7741): `max-fr`, `max-fs`.
 *
 *   - **VP9** (draft-ietf-payload-vp9, RFC 9559): `profile-id`,
 *     `profile-mapping`, `max-fr`, `max-fs`, `max-br`, etc.
 *
 *   - **AV1**: future (no SDP parameters yet standardised widely).
 *
 * The module is **codec-only** — the generic SDP parser remains in
 * `modules/sdp`. We provide a thin façade that:
 *   1. Parses `a=fmtp:<pt> <params>` for a single codec kind, returning
 *      a typed struct.
 *   2. Builds the matching `a=fmtp:<pt> <params>` string for SDP emission.
 *   3. Helps with `a=rtpmap:<pt> <name>/<clock>` matching the codec kind.
 *
 * @note P1. Standalone module; not yet wired into the engine.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/error.hpp>
#include <nimrtc/video_frame/frame.hpp>

namespace nimrtc::video_sdp_attributes {

// ---------------------------------------------------------------------------
// Codec kind re-export (avoid dragging nimrtc::video_frame::CodecKind into
// every consumer of this header — but the values match).
// ---------------------------------------------------------------------------

using CodecKind = nimrtc::video_frame::CodecKind;

// ---------------------------------------------------------------------------
// H.264 fmtp parameters (RFC 6184 §8.2 + §8.3)
// ---------------------------------------------------------------------------

/** H.264 `packetization-mode` (RFC 6184 §6 / §8.2). */
enum class PacketizationMode : std::uint8_t {
    kSingleNal   = 0,
    kNonInterleaved = 1,   // Single NAL + STAP-A + FU-A
    kInterleaved   = 2,   // reserved by RFC 6184; rarely used
};

struct H264Fmtp {
    std::optional<PacketizationMode> packetization_mode;
    /** Hex-encoded profile-level-id, e.g. "42e01f" (Baseline 3.1). */
    std::optional<std::string>       profile_level_id;
    /** Base64-encoded SPS, PPS pair from sprop-parameter-sets. */
    std::optional<std::string>       sprop_sps;
    std::optional<std::string>       sprop_pps;

    std::optional<std::uint32_t>     max_mbps;     // macroblocks/s
    std::optional<std::uint32_t>     max_fs;       // frame size in macroblocks
    std::optional<std::uint32_t>     max_smbps;    // macroblocks/s over all slices
    std::optional<std::uint32_t>     max_br;       // kbps
    std::optional<std::uint32_t>     max_dpb;      // kbits
    std::optional<bool>              level_asymmetry_allowed;

    /** Whether at least one field was populated. */
    [[nodiscard]] bool has_any() const noexcept;
};

// ---------------------------------------------------------------------------
// VP8 fmtp parameters (RFC 7741 §6.1 + WebRTC extension)
// ---------------------------------------------------------------------------

struct Vp8Fmtp {
    std::optional<std::uint32_t>     max_fr;        // max frame rate (per second)
    std::optional<std::uint32_t>     max_fs;        // max frame size (macroblocks / 16x16 blocks)
};

// ---------------------------------------------------------------------------
// VP9 fmtp parameters (RFC 9559 / draft-ietf-payload-vp9-09)
// ---------------------------------------------------------------------------

struct Vp9Fmtp {
    std::optional<std::uint32_t>     profile_id;
    std::optional<std::string>       profile_mapping;   // comma-separated "<pt>:<profile>"
    std::optional<std::uint32_t>     max_fr;
    std::optional<std::uint32_t>     max_fs;
    std::optional<std::uint32_t>     max_br;
};

// ---------------------------------------------------------------------------
// Variant — strongly-typed union for parse / build dispatch
// ---------------------------------------------------------------------------

struct ParsedFmtp {
    CodecKind kind = CodecKind::kUnknown;
    union {
        H264Fmtp h264;
        Vp8Fmtp  vp8;
        Vp9Fmtp  vp9;
    };

    ParsedFmtp() noexcept;
    ~ParsedFmtp();
    ParsedFmtp(const ParsedFmtp&);
    ParsedFmtp& operator=(const ParsedFmtp&);
    ParsedFmtp(ParsedFmtp&&) noexcept;
    ParsedFmtp& operator=(ParsedFmtp&&) noexcept;
};

// ---------------------------------------------------------------------------
// Parsing — given a codec kind and a raw fmtp body, return a ParsedFmtp
// ---------------------------------------------------------------------------

/** Parse a fmtp body for the given codec kind.
 *  @param kind     The codec kind (drives which parser to use).
 *  @param fmtp_raw The fmtp body, e.g. "packetization-mode=1;profile-level-id=42e01f"
 *                  (NOT including the leading "a=fmtp:<pt> ").
 *  @return Typed ParsedFmtp on success; core::Status on error. */
core::Result<ParsedFmtp> parse_fmtp(CodecKind kind,
                                    std::string_view fmtp_raw) noexcept;

/** Auto-detect the codec kind from a `a=rtpmap:<pt> <encoding>/<clock>` line.
 *  Returns kUnknown if the encoding name is not recognised. */
CodecKind codec_from_rtpmap_encoding(std::string_view encoding) noexcept;

/** Parse H.264 fmtp body directly. */
core::Result<H264Fmtp> parse_h264(std::string_view fmtp_raw) noexcept;

/** Parse VP8 fmtp body directly. */
core::Result<Vp8Fmtp> parse_vp8(std::string_view fmtp_raw) noexcept;

/** Parse VP9 fmtp body directly. */
core::Result<Vp9Fmtp> parse_vp9(std::string_view fmtp_raw) noexcept;

// ---------------------------------------------------------------------------
// Build — produce a fmtp body from a typed config (for SDP answer emission)
// ---------------------------------------------------------------------------

/** Build a H.264 fmtp body, e.g. "packetization-mode=1;profile-level-id=42e01f".
 *  Fields that are std::nullopt are omitted. */
std::string build_h264_fmtp(const H264Fmtp& cfg) noexcept;

/** Build a VP8 fmtp body. */
std::string build_vp8_fmtp(const Vp8Fmtp& cfg) noexcept;

/** Build a VP9 fmtp body. */
std::string build_vp9_fmtp(const Vp9Fmtp& cfg) noexcept;

/** Dispatch on ParsedFmtp::kind. */
std::string build_fmtp(const ParsedFmtp& cfg) noexcept;

// ---------------------------------------------------------------------------
// Generic key=value parser (used internally; also handy for unit tests)
// ---------------------------------------------------------------------------

/** Split a fmtp body into ordered (key, value) pairs.
 *  Semicolons separate pairs; the first '=' per pair separates key/value.
 *  Values are URL-decoded (replacing %xx and '+' with their literal).
 *  @return Ordered list of (key, value) pairs. */
std::vector<std::pair<std::string, std::string>>
split_fmtp_pairs(std::string_view body) noexcept;

/** Parse a single integer value from a fmtp key=value pair.
 *  @return Parsed value, or std::nullopt on parse error. */
std::optional<std::int64_t>
parse_int_value(std::string_view value) noexcept;

} // namespace nimrtc::video_sdp_attributes
