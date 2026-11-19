/**
 * @file src/modules/nvcodec/src/vt_common.hpp
 * @brief Shared helpers for the Apple VideoToolbox backends.
 *
 * ## Why this file exists
 *
 * The VideoToolbox encoder and the VideoToolbox decoder both have to
 * speak Annex-B — H.264 and HEVC parameter sets, the access-unit
 * framing the RTP packetizer expects — and both have to map between
 * the tree's `VideoCodecKind` and Apple's `CMVideoCodecType`.  Those
 * two concerns are shared, so they live here rather than being
 * written twice (and drifting apart).
 *
 * The header is intentionally *implementation-only* (under `src/`, not
 * `include/`): the public plugin surface is `IVideoCodec` in
 * `<nimrtc/plugins/video_codec.hpp>`, and the module's public surface
 * is `<nimrtc/nvcodec/vt_backends.hpp>`.
 *
 * ## The Apple SDK is not available off-Apple
 *
 * Every declaration here is guarded so that the file compiles on every
 * platform NimRTC builds.  Off-Apple the bitstream helpers work
 * everywhere (they are pure byte manipulation), and only the
 * `CMVideoCodecType` mapping is Apple-only.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <nimrtc/nvcodec/vt_backends.hpp>
#include <nimrtc/plugins/video_codec.hpp>

#if defined(__APPLE__)
  #include <CoreMedia/CoreMedia.h>
  #include <CoreVideo/CoreVideo.h>
  #include <VideoToolbox/VideoToolbox.h>
  #include <sys/sysctl.h>
#endif

namespace nimrtc::nvcodec::vt_detail {

// ---------------------------------------------------------------------------
// Codec mapping
// ---------------------------------------------------------------------------

/** The codec family a VideoToolbox session serves. */
enum class Codec {
    kH264 = 0,
    kH265 = 1,
    kAv1  = 2,
};

/** The tree's codec kind for @p codec. */
inline plugins::VideoCodecKind codec_kind(Codec codec) noexcept {
    switch (codec) {
        case Codec::kH265: return plugins::VideoCodecKind::kH265;
        case Codec::kAv1:  return plugins::VideoCodecKind::kAV1;
        case Codec::kH264: break;
    }
    return plugins::VideoCodecKind::kH264;
}

/** The RTP payload type the tree uses for @p codec.
 *
 *  H.264 has the statically-assigned payload type 102 (RFC 3551).
 *  H.265 has none; 116 is the value RFC 7798 §4.1 recommends and the
 *  one Chrome's WebRTC stack uses for HEVC.  AV1 likewise has none;
 *  45 is the value draft-ietf-payload-av1 §10 (and Chrome's Unified
 *  Plan) uses. */
inline std::uint8_t default_payload_type(Codec codec) noexcept {
    switch (codec) {
        case Codec::kH265: return std::uint8_t{116};
        case Codec::kAv1:  return std::uint8_t{45};
        case Codec::kH264: break;
    }
    return std::uint8_t{102};
}

/** The tree's short codec name for @p codec ("h264" / "h265" / "av1"). */
inline const char* codec_short_name(Codec codec) noexcept {
    switch (codec) {
        case Codec::kH265: return "h265";
        case Codec::kAv1:  return "av1";
        case Codec::kH264: break;
    }
    return "h264";
}

/** Map the *public* `nvcodec::VtCodec` onto the internal codec enum.
 *
 *  The public enum lives in `vt_backends.hpp`, which includes this
 *  header — so the mapping is defined here to avoid a circular
 *  include, and it is available on every platform (the reason strings
 *  need it off-Apple too). */
inline Codec to_detail(VtCodec codec) noexcept {
    switch (codec) {
        case VtCodec::kH265: return Codec::kH265;
        case VtCodec::kAv1:  return Codec::kAv1;
        case VtCodec::kH264: break;
    }
    return Codec::kH264;
}

/** The `CMVideoCodecType` for @p codec.
 *
 *  Apple-only: the constant lives in CoreMedia's `CMFormatDescription.h`.
 *  Off-Apple the type does not exist either, so the function is
 *  compiled out and the caller never gets that far (the session
 *  creation is guarded too). */
#if defined(__APPLE__)
inline CMVideoCodecType cm_codec_type(Codec codec) noexcept {
    switch (codec) {
        case Codec::kH265: return kCMVideoCodecType_HEVC;
        case Codec::kAv1:  return kCMVideoCodecType_AV1;
        case Codec::kH264: break;
    }
    return kCMVideoCodecType_H264;
}
#endif

/** True iff this host has a **hardware** decoder for @p codec.
 *
 *  ## Why this is a capability query and not a session probe
 *
 *  The obvious way to ask "does this machine decode @p codec in
 *  hardware?" is to create a decompression session and see whether it
 *  works.  That is what the *encoder* half of this module does, and it
 *  is the honest answer there: the compression session is created from
 *  a format description that needs no stream context.
 *
 *  The decoder half cannot do that.  A hardware VideoToolbox decoder
 *  rejects a format description that carries no **codec extension
 *  atom** (`avcC` for H.264, `hvcC` for HEVC) with
 *  `-8971` (`codecExtensionNotFoundErr`) — and those atoms can only be
 *  built from the *stream's own* parameter sets, which a probe has no
 *  way to know.  Probing by session creation therefore reports "this
 *  host has no hardware decoder" on every Mac that in fact has one.
 *
 *  `VTIsHardwareDecodeSupported()` answers the same question without a
 *  format description, which is exactly what the probe needs.
 *
 *  Off-Apple the answer is `false`: there is no VideoToolbox there.
 *
 *  @see the session's deferred-open path in `videotoolbox_decoder.cpp`,
 *       which uses this to decide whether to wait for the stream's
 *       parameter sets or to give up. */
inline bool hardware_decode_supported(Codec codec) noexcept {
#if defined(__APPLE__)
    return VTIsHardwareDecodeSupported(cm_codec_type(codec)) != 0;
#else
    (void)codec;
    return false;
#endif
}

// ---------------------------------------------------------------------------
// OS version
// ---------------------------------------------------------------------------

/** The running macOS version, as a (major, minor) pair.
 *
 *  Off-Apple the function returns `{0, 0}`, which the "is the OS new
 *  enough" test below reads as "no" — the conservative answer. */
struct OsVersion {
    int major = 0;
    int minor = 0;
};

/** The running macOS version.
 *
 *  The version is read from `kern.osproductversion` through `sysctl`,
 *  so the probe reports the *runtime* OS version rather than the
 *  version the binary was compiled against. */
inline OsVersion os_version() noexcept {
#if defined(__APPLE__)
    char buf[64] = {};
    std::size_t size = sizeof(buf);
    if (::sysctlbyname("kern.osproductversion", buf, &size, nullptr, 0) != 0) {
        return OsVersion{};
    }
    int major = 0;
    int minor = 0;
    if (std::sscanf(buf, "%d.%d", &major, &minor) != 2) {
        return OsVersion{};
    }
    return OsVersion{major, minor};
#else
    return OsVersion{};
#endif
}

/** The oldest macOS release that can run a VideoToolbox session for
 *  @p codec with the properties this backend sets.
 *
 *  The floor is driven by the *property* the session needs, and by the
 *  codec itself:
 *
 *   - H.264 : macOS 10.9 — `kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder`
 *             and `kVTCompressionPropertyKey_RealTime` are both
 *             `API_AVAILABLE(macosx(10.9))`; every other property this
 *             backend sets (`ExpectedFrameRate`, `AverageBitRate`,
 *             `MaxKeyFrameInterval`, `AllowFrameReordering`) is
 *             `API_AVAILABLE(macosx(10.8))`.
 *   - HEVC  : macOS 10.13 — VideoToolbox gained the HEVC profile / level
 *             constants and the HEVC encoder in High Sierra.
 *   - AV1   : macOS 14.0 — `kCMVideoCodecType_AV1` and the AV1 hardware
 *             decode support landed in Sonoma (and only on M3+ silicon).
 *             Note that no Apple chip has an AV1 hardware *encoder*, so
 *             the AV1 *encoder* probe fails on every current Mac.
 *
 *  A host older than this cannot run the session at all, so the probe
 *  reports "not available" — and says so — instead of letting the
 *  session creation fail with an opaque `OSStatus`. */
inline OsVersion min_macos_version(Codec codec) noexcept {
    switch (codec) {
        case Codec::kAv1:  return OsVersion{14, 0};
        case Codec::kH265: return OsVersion{10, 13};
        case Codec::kH264: break;
    }
    return OsVersion{10, 9};
}

/** True iff the running OS is at least @p required.
 *
 *  An *unknown* version (which is what a non-Apple host reports) is
 *  treated as "too old" — the conservative answer. */
inline bool os_at_least(const OsVersion& required) noexcept {
    const OsVersion v = os_version();
    if (v.major == 0) return false;
    if (v.major != required.major) return v.major > required.major;
    return v.minor >= required.minor;
}

/** The codec's short name, for the reason strings. */
inline const char* codec_name(Codec codec) noexcept {
    switch (codec) {
        case Codec::kH265: return "H.265 / HEVC";
        case Codec::kAv1:  return "AV1";
        case Codec::kH264: break;
    }
    return "H.264";
}

/** The reason the VideoToolbox *encoder* is unavailable on this host.
 *
 *  The string is always non-empty — the plan's B5 / B6 acceptance is
 *  "unsupported combinations are explicitly reported, not silently
 *  ignored". */
inline std::string encoder_unavailable_reason(Codec codec) noexcept {
#if defined(__APPLE__)
    const OsVersion required = min_macos_version(codec);
    const OsVersion running  = os_version();
    if (!os_at_least(required)) {
        return std::string("videotoolbox: the ") + codec_name(codec) +
               " hardware encoder needs macOS " + std::to_string(required.major) +
               "." + std::to_string(required.minor) +
               " or newer for the required compression-session properties; this "
               "host runs macOS " +
               std::to_string(running.major) + "." + std::to_string(running.minor);
    }
    return std::string("videotoolbox: VTCompressionSessionCreate failed for ") +
           codec_name(codec) +
           " — this host's VideoToolbox hardware does not provide that encoder";
#else
    return std::string("videotoolbox: the Apple VideoToolbox framework does "
                       "not exist on this platform, so the ") +
           codec_name(codec) + " hardware encoder is unavailable";
#endif
}

/** The reason the VideoToolbox *decoder* is unavailable on this host. */
inline std::string decoder_unavailable_reason(Codec codec) noexcept {
#if defined(__APPLE__)
    const OsVersion required = min_macos_version(codec);
    const OsVersion running  = os_version();
    if (!os_at_least(required)) {
        return std::string("videotoolbox: the ") + codec_name(codec) +
               " hardware decoder needs macOS " + std::to_string(required.major) +
               "." + std::to_string(required.minor) +
               " or newer for the required VTDecompressionSession settings; "
               "this host runs macOS " +
               std::to_string(running.major) + "." + std::to_string(running.minor);
    }
    if (!hardware_decode_supported(codec)) {
        return std::string("videotoolbox: this host's VideoToolbox hardware "
                           "does not provide a ") +
               codec_name(codec) +
               " decoder (VTIsHardwareDecodeSupported reports no hardware "
               "support for the codec)";
    }
    return std::string("videotoolbox: VTDecompressionSessionCreate failed for ") +
           codec_name(codec) +
           " — the host has a hardware decoder for the codec, but the "
           "session could not be created from the stream's parameter sets";
#else
    return std::string("videotoolbox: the Apple VideoToolbox framework does "
                       "not exist on this platform, so the ") +
           codec_name(codec) + " hardware decoder is unavailable";
#endif
}

// ---------------------------------------------------------------------------
// AV1 OBU framing
// ---------------------------------------------------------------------------
//
// VideoToolbox's AV1 encoder emits the AV1 spec's **low-overhead**
// bitstream: a sequence of OBUs, each carrying `obu_has_size_field = 1`
// (a LEB128 length after the header).  The RTP packetizer in this tree
// carries AV1 in **Annex-B** framing (`00 00 00 01` before each OBU) —
// the same framing the NVENC AV1 path produces via
// `outputAnnexBFormat = 1`.
//
// The two conversions below are pure byte manipulation: they are
// platform-independent, so they compile (and are unit-tested) on every
// host, not only on macOS.
// ---------------------------------------------------------------------------

/** The size of the OBU header (before the `obu_size` field).
 *
 *  The header is one byte, plus one more when `obu_extension_flag` is
 *  set (AV1 spec §5.3.1). */
inline std::size_t av1_obu_header_size(std::uint8_t header_byte) noexcept {
    return (header_byte & 0x04u) ? 2u : 1u;
}

/** Read a LEB128 value from @p data at @p pos.
 *
 *  @param data  The buffer.
 *  @param size  The buffer's size.
 *  @param pos   In/out cursor — advanced past the value on success.
 *  @param value Receives the decoded value.
 *  @return true on success; false when the buffer is truncated or the
 *          encoding is longer than 8 bytes (which cannot happen in a
 *          legal OBU). */
inline bool av1_read_leb128(const std::uint8_t* data,
                            std::size_t size,
                            std::size_t& pos,
                            std::size_t& value) noexcept {
    value = 0;
    for (int i = 0; i < 8; ++i) {
        if (pos >= size) return false;
        const std::uint8_t byte = data[pos++];
        value |= static_cast<std::size_t>(byte & 0x7Fu) << (i * 7);
        if ((byte & 0x80u) == 0) return true;
    }
    return false;
}
/** Write a LEB128 encoding of @p value into @p out (which must hold at
 *  least 10 bytes).  @return the number of bytes written. */
inline std::size_t av1_write_leb128(std::size_t value,
                                    std::uint8_t* out) noexcept {
    std::size_t n = 0;
    while (value >= 0x80u) {
        out[n++] = static_cast<std::uint8_t>(value) | 0x80u;
        value >>= 7;
    }
    out[n++] = static_cast<std::uint8_t>(value);
    return n;
}

/** Convert VideoToolbox's low-overhead AV1 bitstream into Annex-B.
 *
 *  The low-overhead form is a sequence of OBUs, each with
 *  `obu_has_size_field = 1`; the Annex-B form replaces the size field
 *  with a `00 00 00 01` start code and clears the `obu_has_size_field`
 *  bit (the AV1 spec requires that bit to be 0 in Annex-B).
 *
 *  A buffer that already *is* Annex-B is passed through unchanged —
 *  the conversion is idempotent on its own output.
 *
 *  @param data  The bitstream.
 *  @param size  Its size in bytes.
 *  @param out   Receives the Annex-B bytes.
 *  @return true on success; false when the stream is not a well-formed
 *          low-overhead OBU stream (and therefore cannot be converted). */
inline bool av1_low_overhead_to_annex_b(const std::uint8_t* data,
                                         std::size_t size,
                                         std::vector<std::uint8_t>& out) noexcept {
    out.clear();
    if (!data || size == 0) return false;

    // Already Annex-B?  Then there is nothing to do.
    if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        out.assign(data, data + size);
        return true;
    }

    constexpr std::uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};

    std::size_t pos = 0;
    while (pos < size) {
        const std::uint8_t header = data[pos];
        const std::size_t header_size = av1_obu_header_size(header);
        if (pos + header_size > size) return false;

        // The size field must be present — without it the OBU's length
        // is not knowable, so the stream cannot be split.
        if ((header & 0x02u) == 0) return false;

        // The extension byte (AV1 spec §5.3.1) follows the header when
        // `obu_extension_flag` is set.
        std::uint8_t extension = 0;
        if (header_size == 2) extension = data[pos + 1];

        std::size_t payload_size = 0;
        std::size_t size_pos = pos + header_size;
        if (!av1_read_leb128(data, size, size_pos, payload_size)) return false;

        // `size_pos` now points at the OBU's payload.
        const std::size_t payload_pos = size_pos;
        if (payload_pos + payload_size > size) return false;

        // Annex-B: start code, then the header with the size bit cleared.
        out.insert(out.end(), kStartCode, kStartCode + 4);
        out.push_back(static_cast<std::uint8_t>(header & ~0x02u));
        if (header_size == 2) out.push_back(extension);
        out.insert(out.end(), data + payload_pos,
                   data + payload_pos + payload_size);
        pos = payload_pos + payload_size;
    }
    return !out.empty();
}

/** Convert an Annex-B AV1 bitstream into VideoToolbox's low-overhead form.
 *
 *  The inverse of `av1_low_overhead_to_annex_b()`: each OBU's payload
 *  length is recovered from the Annex-B framing, and the OBU is written
 *  with `obu_has_size_field = 1` and an explicit LEB128 size.
 *
 *  @param data  The Annex-B bitstream.
 *  @param size  Its size in bytes.
 *  @param out   Receives the low-overhead bytes.
 *  @return true on success; false when the stream is not Annex-B. */
inline bool av1_annex_b_to_low_overhead(const std::uint8_t* data,
                                         std::size_t size,
                                         std::vector<std::uint8_t>& out) noexcept {
    out.clear();
    if (!data || size < 4) return false;
    if (!(data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1)) return false;

    std::size_t pos = 0;
    while (pos < size) {
        // Locate the start code.
        std::size_t start = 0;
        std::size_t sc_len = 0;
        if (pos + 3 < size && data[pos] == 0 && data[pos + 1] == 0 &&
            data[pos + 2] == 0 && data[pos + 3] == 1) {
            sc_len = 4;
            start = pos + 4;
        } else if (pos + 2 < size && data[pos] == 0 && data[pos + 1] == 0 &&
                   data[pos + 2] == 1) {
            sc_len = 3;
            start = pos + 3;
        } else {
            return false;
        }

        // Find the next start code (or the end of the buffer).
        std::size_t end = start;
        bool found = false;
        while (end < size) {
            if (end + 2 < size && data[end] == 0 && data[end + 1] == 0 &&
                (data[end + 2] == 1 ||
                 (end + 3 < size && data[end + 2] == 0 && data[end + 3] == 1))) {
                found = true;
                break;
            }
            ++end;
        }
        if (!found) end = size;

        const std::size_t obu_size = end - start;
        if (obu_size == 0) return false;

        const std::uint8_t header = data[start];
        const std::size_t header_size = av1_obu_header_size(header);
        if (obu_size < header_size) return false;

        // Low-overhead form: the size bit is set and the LEB128 size
        // follows the header.
        out.push_back(static_cast<std::uint8_t>(header | 0x02u));
        if (header_size == 2) out.push_back(data[start + 1]);

        std::uint8_t leb[10] = {};
        const std::size_t payload_size = obu_size - header_size;
        const std::size_t leb_len = av1_write_leb128(payload_size, leb);
        out.insert(out.end(), leb, leb + leb_len);
        out.insert(out.end(), data + start + header_size, data + start + obu_size);

        pos = end;
        (void)sc_len;
    }
    return !out.empty();
}

// ---------------------------------------------------------------------------
// NAL unit types
// ---------------------------------------------------------------------------

/** The NAL unit type of the NAL unit starting at @p p.
 *
 *  H.264 puts the type in the low 5 bits of the first byte; HEVC uses
 *  6 bits shifted left by one. */
inline std::uint8_t nalu_type(Codec codec, const std::uint8_t* p) noexcept {
    if (!p) return 0xFF;
    return codec == Codec::kH265 ? static_cast<std::uint8_t>((p[0] >> 1) & 0x3F)
                                 : static_cast<std::uint8_t>(p[0] & 0x1F);
}

/** The NAL unit type of the *first* parameter set in @p codec's stream:
 *  the H.264 SPS (7) or the HEVC VPS (32). */
inline std::uint8_t primary_param_set_type(Codec codec) noexcept {
    return codec == Codec::kH265 ? std::uint8_t{32} : std::uint8_t{7};
}

// ---------------------------------------------------------------------------
// Annex-B scanning
// ---------------------------------------------------------------------------

/** One NAL unit found in an Annex-B stream. */
struct Nalu {
    std::size_t offset = 0;   ///< offset of the NAL header byte
    std::size_t size   = 0;   ///< length of the NAL unit (header + payload)
    std::uint8_t type = 0;    ///< the NAL unit type
};

/** Collect every NAL unit in the Annex-B stream @p data.
 *
 *  Both the 4-byte (`00 00 00 01`) and the 3-byte (`00 00 01`) start
 *  code forms are recognised. */
inline std::vector<Nalu> scan_annex_b(Codec codec,
                                      const std::uint8_t* data,
                                      std::size_t size) noexcept {
    std::vector<Nalu> out;
    if (!data || size < 4) return out;

    std::size_t i = 0;
    while (i + 3 < size) {
        std::size_t start = 0;
        std::size_t sc_len = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
            sc_len = 4;
            start = i + 4;
        } else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            sc_len = 3;
            start = i + 3;
        } else {
            ++i;
            continue;
        }

        // Find the next start code (or the end of the buffer).
        std::size_t j = start;
        bool found = false;
        while (j < size) {
            if (j + 2 < size && data[j] == 0 && data[j + 1] == 0 &&
                (data[j + 2] == 1 ||
                 (j + 3 < size && data[j + 2] == 0 && data[j + 3] == 1))) {
                found = true;
                break;
            }
            ++j;
        }
        if (!found) j = size;

        if (j > start) {
            out.push_back(Nalu{start, j - start, nalu_type(codec, data + start)});
        }
        i = (j > i) ? j : (i + sc_len);
    }
    return out;
}

/** True iff @p data looks like an Annex-B stream (it starts with a
 *  start code). */
inline bool looks_like_annex_b(const std::uint8_t* data, std::size_t size) noexcept {
    if (!data || size < 3) return false;
    if (data[0] == 0 && data[1] == 0 && data[2] == 1) return true;
    return size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1;
}

// ---------------------------------------------------------------------------
// Parameter-set extraction
//
// VideoToolbox's **decompression** session is created from a format
// description, and for H.264 / HEVC that description must carry the
// stream's own parameter sets (the SPS / PPS for H.264, the VPS / SPS /
// PPS for HEVC) — a description built from a bare codec type carries no
// parameter sets, and the decoder then rejects the first frame.
//
// The extraction below is pure byte manipulation, so it compiles (and is
// unit-tested) on every host, not only on macOS.
// ---------------------------------------------------------------------------

/** The parameter sets of one codec family, as raw NAL unit payloads.
 *
 *  The vectors hold the *NAL unit bytes without the start code* — which
 *  is exactly what the `CMVideoFormatDescriptionCreateFrom*ParameterSets`
 *  functions want. */
struct ParameterSets {
    std::vector<std::uint8_t> vps;   ///< HEVC video parameter set (type 32)
    std::vector<std::uint8_t> sps;   ///< H.264 SPS (7) / HEVC SPS (33)
    std::vector<std::uint8_t> pps;   ///< H.264 PPS (8) / HEVC PPS (34)

    [[nodiscard]] bool has_sps() const noexcept { return !sps.empty(); }
    [[nodiscard]] bool has_pps() const noexcept { return !pps.empty(); }
};

/** True iff @p sets carry everything the format description for @p codec
 *  needs.
 *
 *  H.264 needs the SPS and the PPS; HEVC needs the VPS as well (without
 *  it the `hvcC` atom is incomplete, and VideoToolbox refuses the
 *  session).  AV1 carries its sequence header inside the OBU stream
 *  itself, so it has no out-of-band parameter sets at all — the answer
 *  for AV1 is therefore `false`, and the AV1 path never asks.
 *
 *  This is a *pure* predicate over the extracted sets, so the rule is
 *  unit-tested on every host — the macOS-only session code is not the
 *  only place that knows what a format description needs. */
inline bool parameter_sets_complete(Codec codec,
                                     const ParameterSets& sets) noexcept {
    if (codec == Codec::kAv1) return false;   // AV1 has no out-of-band sets
    if (!sets.has_sps() || !sets.has_pps()) return false;
    if (codec == Codec::kH265 && sets.vps.empty()) return false;
    return true;
}

/** True iff @p a and @p b carry the same parameter sets, byte for byte.
 *
 *  The comparison is what tells "the stream still speaks the sequence
 *  the session was created for" from "the stream switched sequence".
 *  A mid-stream switch (a new geometry, a new profile) has to re-create
 *  the session, because the format description the session holds
 *  describes the *old* sequence.
 *
 *  The vectors hold the raw NAL units (header byte included), so the
 *  comparison is exact. */
inline bool parameter_sets_equal(const ParameterSets& a,
                                 const ParameterSets& b) noexcept {
    return a.sps == b.sps && a.pps == b.pps && a.vps == b.vps;
}

/** Extract the parameter sets from an Annex-B bitstream.
 *
 *  @param codec  The codec family (H.264 / HEVC).
 *  @param data   The Annex-B bitstream.
 *  @param size   Its size in bytes.
 *  @param out    Receives the parameter sets.
 *  @return true when at least the SPS was found. */
inline bool extract_parameter_sets(Codec codec,
                                    const std::uint8_t* data,
                                    std::size_t size,
                                    ParameterSets& out) noexcept {
    out = ParameterSets{};
    if (!data || size < 4) return false;

    const auto nalus = scan_annex_b(codec, data, size);
    for (const auto& n : nalus) {
        if (n.type == 7 || n.type == 33) {          // H.264 / HEVC SPS
            if (out.sps.empty()) {
                out.sps.assign(data + n.offset, data + n.offset + n.size);
            }
        } else if (n.type == 8 || n.type == 34) {   // H.264 / HEVC PPS
            if (out.pps.empty()) {
                out.pps.assign(data + n.offset, data + n.offset + n.size);
            }
        } else if (n.type == 32) {                  // HEVC VPS
            if (out.vps.empty()) {
                out.vps.assign(data + n.offset, data + n.offset + n.size);
            }
        }
    }
    return out.has_sps();
}

/** Convert a length-prefixed (AVCC) buffer into an Annex-B buffer.
 *
 *  VideoToolbox's compression session emits length-prefixed NAL units
 *  (the "AVCC" framing), while the RTP packetizer in this tree expects
 *  Annex-B start codes.  The conversion is therefore mandatory on the
 *  encode path.
 *
 *  A buffer that does not parse as length-prefixed yields an empty
 *  vector — the caller then treats it as a corrupt bitstream rather
 *  than emitting garbage on the wire. */
inline std::vector<std::uint8_t> avcc_to_annex_b(const std::uint8_t* data,
                                                  std::size_t size) noexcept {
    std::vector<std::uint8_t> out;
    if (!data || size == 0) return out;

    constexpr std::uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};
    std::size_t offset = 0;
    while (offset + 4 <= size) {
        const std::uint32_t len = (static_cast<std::uint32_t>(data[offset]) << 24) |
                                  (static_cast<std::uint32_t>(data[offset + 1]) << 16) |
                                  (static_cast<std::uint32_t>(data[offset + 2]) << 8) |
                                   static_cast<std::uint32_t>(data[offset + 3]);
        offset += 4;
        if (len == 0 || offset + len > size) {
            // Not a length-prefixed stream after all — bail out rather
            // than emit garbage.
            out.clear();
            return out;
        }
        out.insert(out.end(), kStartCode, kStartCode + 4);
        out.insert(out.end(), data + offset, data + offset + len);
        offset += len;
    }
    return out;
}

/** Convert an Annex-B stream into the length-prefixed (AVCC) form.
 *
 * The inverse of `avcc_to_annex_b()`: each NAL unit's payload is
 * prefixed with its 4-byte big-endian length.
 *
 * This is the form VideoToolbox's **decompression** session expects when
 * the format description was built from the codec type (rather than
 * from the bitstream's own parameter sets): the sample buffer's block
 * buffer carries length-prefixed NAL units.
 *
 * @param data  The Annex-B bitstream.
 * @param size  Its size in bytes.
 * @param out   Receives the length-prefixed bytes.
 * @return true on success; false when the data is not Annex-B. */
inline bool annex_b_to_avcc(const std::uint8_t* data,
                            std::size_t size,
                            std::vector<std::uint8_t>& out) noexcept {
    out.clear();
    if (!data || size < 4) return false;
    if (!(data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1)) return false;

    std::size_t pos = 0;
    while (pos < size) {
        // Locate the start code.
        std::size_t start = 0;
        std::size_t sc_len = 0;
        if (pos + 3 < size && data[pos] == 0 && data[pos + 1] == 0 &&
            data[pos + 2] == 0 && data[pos + 3] == 1) {
            sc_len = 4;
            start = pos + 4;
        } else if (pos + 2 < size && data[pos] == 0 && data[pos + 1] == 0 &&
                   data[pos + 2] == 1) {
            sc_len = 3;
            start = pos + 3;
        } else {
            return false;
        }

        // Find the next start code (or the end of the buffer).
        std::size_t end = start;
        bool found = false;
        while (end < size) {
            if (end + 2 < size && data[end] == 0 && data[end + 1] == 0 &&
                (data[end + 2] == 1 ||
                 (end + 3 < size && data[end + 2] == 0 && data[end + 3] == 1))) {
                found = true;
                break;
            }
            ++end;
        }
        if (!found) end = size;

        const std::size_t nalu_size = end - start;
        if (nalu_size == 0) return false;

        // The 4-byte big-endian length prefix.
        const std::uint32_t len = static_cast<std::uint32_t>(nalu_size);
        out.push_back(static_cast<std::uint8_t>((len >> 24) & 0xFFu));
        out.push_back(static_cast<std::uint8_t>((len >> 16) & 0xFFu));
        out.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFFu));
        out.push_back(static_cast<std::uint8_t>(len & 0xFFu));
        out.insert(out.end(), data + start, data + start + nalu_size);

        pos = end;
        (void)sc_len;
    }
    return !out.empty();
}

/** Ensure @p data is in Annex-B form.
 *
 *  @param out  Receives the Annex-B bytes (either a copy of @p data, or
 *              the converted length-prefixed stream).
 *  @return true on success; false when @p data is neither Annex-B nor a
 *          well-formed length-prefixed stream. */
inline bool ensure_annex_b(const std::uint8_t* data,
                           std::size_t size,
                           std::vector<std::uint8_t>& out) noexcept {
    if (!data || size == 0) return false;
    if (looks_like_annex_b(data, size)) {
        out.assign(data, data + size);
        return true;
    }
    out = avcc_to_annex_b(data, size);
    return !out.empty();
}

// ---------------------------------------------------------------------------
// The parameter sets a compression session produced
//
// VideoToolbox's compression session stores the H.264 / HEVC parameter
// sets (the SPS / PPS, and the VPS for HEVC) **out of band**, in the
// sample buffer's `CMFormatDescriptionRef`.  The compressed NAL units
// themselves carry only the slice data.
//
// That framing is legal, but it is not what the rest of this tree
// expects: a software decoder (OpenH264, for instance) fed a bitstream
// without the parameter sets has nothing to decode against and answers
// `kErrCorrupt` (4100).  The parameter sets therefore have to be
// prepended to the key frame as in-band Annex-B NAL units.
//
// The CoreMedia C API is the portable spelling of "give me the
// parameter sets":
//
//   CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, index,
//       &bytes, &size, &count, &nal_len)
//   CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, index, ...)
// ---------------------------------------------------------------------------

/** Prepend the parameter sets to an Annex-B access unit.
 *
 *  The parameter sets belong *in front of* the slices, as Annex-B NAL
 *  units with their own start codes — that is where the H.264 / HEVC
 *  specifications put them, and that is what a decoder that expects
 *  in-band parameter sets needs in order to decode at all.
 *
 *  This helper is *portable* (it is pure byte manipulation), so the
 *  unit tests exercise it on every host — including the hosts where
 *  VideoToolbox does not exist.
 *
 *  @param parameter_sets  The parameter sets, already in Annex-B form
 *                         (start code + payload, one after another).
 *  @param access_unit     The access unit (the slices), in Annex-B form.
 *  @param out             Receives the merged access unit.
 *  @return true on success; false when @p access_unit is empty. */
inline bool prepend_parameter_sets(const std::vector<std::uint8_t>& parameter_sets,
                                   const std::vector<std::uint8_t>& access_unit,
                                   std::vector<std::uint8_t>& out) noexcept {
    if (access_unit.empty()) return false;
    out.clear();
    // The capacity is grown *geometrically* rather than to the exact
    // size: the caller reuses this buffer for every key frame, and the
    // key frames' sizes differ slightly (the bitrate controller's
    // output varies), so an exact reserve would reallocate on most of
    // them.
    const std::size_t needed = parameter_sets.size() + access_unit.size();
    if (out.capacity() < needed) {
        out.reserve(std::max(needed, out.capacity() * 2));
    }
    out.insert(out.end(), parameter_sets.begin(), parameter_sets.end());
    out.insert(out.end(), access_unit.begin(), access_unit.end());
    return true;
}

#if defined(__APPLE__)

/** Collect the parameter sets a compression session produced.
 *
 *  The CoreMedia accessors report the *total* number of parameter sets
 *  through the `parameterSetCountOut` argument, so the first call
 *  establishes how many there are and the loop then asks for each one.
 *
 *  The H.264 accessor returns the SPS first and the PPS second; the
 *  HEVC accessor returns the VPS, the SPS and the PPS — in both cases
 *  already in the order the specification puts them in the stream.
 *
 *  The bytes are written in Annex-B form (a four-byte start code in
 *  front of every parameter set), because that is the framing the
 *  bitstream carries them in.
 *
 *  @param format_description  The sample buffer's format description.
 *  @param codec               The codec family it describes.
 *  @param out                 Receives the parameter sets, in Annex-B form.
 *  @return true when at least one parameter set was found. */
inline bool collect_parameter_sets(CMFormatDescriptionRef format_description,
                                    Codec codec,
                                    std::vector<std::uint8_t>& out) noexcept {
    out.clear();
    if (!format_description) return false;

    constexpr std::uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};

    // The accessor differs per codec family, but the *shape* of the call
    // is the same, so it is spelled once in a lambda and bound to the
    // family the caller asked for.
    const auto collect = [&](auto accessor) -> bool {
        // The first call only asks for the *count* — the pointer and the
        // size are optional out-parameters, and passing them is what
        // would draw an "unused variable" warning.
        std::size_t count = 0;
        if (accessor(format_description, 0, nullptr, nullptr, &count,
                     nullptr) != noErr) {
            return false;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint8_t* set_bytes = nullptr;
            std::size_t set_size          = 0;
            if (accessor(format_description, i, &set_bytes, &set_size,
                         nullptr, nullptr) != noErr) {
                continue;
            }
            if (!set_bytes || set_size == 0) continue;
            out.insert(out.end(), kStartCode, kStartCode + sizeof(kStartCode));
            out.insert(out.end(), set_bytes, set_bytes + set_size);
        }
        return !out.empty();
    };

    // ---- H.264: SPS then PPS -----------------------------------------
    if (codec == Codec::kH264) {
        return collect(CMVideoFormatDescriptionGetH264ParameterSetAtIndex);
    }

    // ---- HEVC: VPS, SPS, PPS -----------------------------------------
    //
    // The HEVC accessor walks the three sets in the order the
    // specification requires (VPS, SPS, PPS).
    if (codec == Codec::kH265) {
        return collect(CMVideoFormatDescriptionGetHEVCParameterSetAtIndex);
    }

    // AV1 carries its sequence header inside the OBU stream itself — it
    // has no out-of-band parameter sets — so there is nothing to
    // collect.
    return false;
}

#endif  // defined(__APPLE__)

} // namespace nimrtc::nvcodec::vt_detail
