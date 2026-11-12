/**
 * @file nimrtc/core/engine_errors.hpp
 * @brief Centralised engine-level error codes (P1#9 fix).
 *
 * ## Why this header exists
 *
 * The plugin layer (`nimrtc::plugins::Status`) defines the cross-module
 * `kOk / kErr*` constants (0x0000..0x1FFF range) that the engine casts
 * to uint32_t via `static_cast<std::uint32_t>(err)`.  The engine itself
 * also emits *module-specific* status codes that don't exist as
 * `plugins::Status` constants — e.g. DTLS open failure, audio3a plugin
 * missing, video plugin load failure.
 *
 * Before this header, those module-specific codes were scattered as raw
 * magic numbers throughout `engine.cpp` (`0x2000` for DTLS,
 * `0x1A00` for audio3a, `0x1A20` for video_sink, etc.).  When the
 * engine's `on_error_` callback fired with code `0x1A26`, downstream
 * tooling had no way to know whether it meant "video sender open failed"
 * or "video sender not found" — both were raw integers.
 *
 * This header consolidates every module-specific engine-level code into a
 * single namespace with named constants.  The numeric values are unchanged
 * (existing consumers — tests, demos, debug scripts — still recognise the
 * legacy values).  New code MUST use these names; raw integer literals
 * in `engine.cpp` are reserved for ABI-stable wire format.
 *
 * ## Naming convention
 *
 *   - `kEngine*`   — engine lifecycle (open / close / state)
 *   - `kTransport*`— ICE / transport layer
 *   - `kDtls*`     — DTLS module
 *   - `kAudio3A*`  — Audio3A plugin loader
 *   - `kVideoSink*`, `kVideoSource*`, `kVideoReceiver*`,
 *     `kVideoSender*`, `kVideoCodec*` — video plugins
 *   - `kSdp*`      — SDP parse / munger
 *
 * ## Compatibility
 *
 * The numeric values MUST stay stable — see CHANGELOG.md and any user-facing
 * scripts that key off the wire codes (e.g. `e2e_chrome_interop.py`).
 */

#pragma once


// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <cstdint>
#include <nimrtc/core/api_version.hpp>

namespace nimrtc::core {

// ---------------------------------------------------------------------------
// Engine lifecycle / state
// ---------------------------------------------------------------------------
inline constexpr NIMRTC_API_STABLE std::uint32_t kEngineInvalidParam = 0x1001;  // == plugins::kErrInvalidParam
inline constexpr NIMRTC_API_STABLE std::uint32_t kEngineNotReady     = 0x1002;  // == plugins::kErrNotReady
inline constexpr NIMRTC_API_STABLE std::uint32_t kEngineSdpCorrupt   = 0x1004;  // == plugins::kErrCorrupt
inline constexpr NIMRTC_API_STABLE std::uint32_t kEngineInternal     = 0x1FFF;  // == plugins::kErrInternal

// ---------------------------------------------------------------------------
// Config validation (v1.1 M2, additive-only)
// ---------------------------------------------------------------------------
// v1.1 M2 StrategyValidator opt-in budget failure code. New symbol, so it
// carries NIMRTC_API_UNSTABLE (empty expansion in default builds, see
// api_version.hpp). Numeric value 0x1010 is unused by all existing
// kEngine*/kDtls*/kAudio3A*/kVideo* codes.
inline constexpr NIMRTC_API_UNSTABLE std::uint32_t kEngineConfig = 0x1010;

// ---------------------------------------------------------------------------
// Transport (ICE) — 0x1F00..0x1FFF
// ---------------------------------------------------------------------------
// 0x1FFF is the catch-all "internal" used by the plugin layer; transport-
// specific failures reuse that value but with different human-readable
// strings.  No transport-only codes are defined yet — add them here when
// the transport layer grows a richer error vocabulary.

// ---------------------------------------------------------------------------
// DTLS — 0x2000..0x20FF
// ---------------------------------------------------------------------------
inline constexpr NIMRTC_API_STABLE std::uint32_t kDtlsOpenFailed     = 0x2000;  // dtls_->open() returned false
inline constexpr NIMRTC_API_STABLE std::uint32_t kDtlsHandshakeFail  = 0x2001;  // reserved (peer rejected)
inline constexpr NIMRTC_API_STABLE std::uint32_t kDtlsKeyDeriveFail  = 0x2002;  // reserved

// ---------------------------------------------------------------------------
// Audio3A plugin loader — 0x1A00..0x1A1F
// ---------------------------------------------------------------------------
inline constexpr NIMRTC_API_STABLE std::uint32_t kAudio3APluginOpenFailed = 0x1A00;
inline constexpr NIMRTC_API_STABLE std::uint32_t kAudio3APluginMissing    = 0x1A01;  // reserved

// ---------------------------------------------------------------------------
// Video plugin loader — 0x1A20..0x1A3F
// ---------------------------------------------------------------------------
// Each subsystem reserves 4 codes: open failure, factory null, not-found,
// reserved.
inline constexpr NIMRTC_API_STABLE std::uint32_t kVideoSinkOpenFailed     = 0x1A20;
inline constexpr NIMRTC_API_STABLE std::uint32_t kVideoSinkPluginMissing  = 0x1A22;

inline constexpr NIMRTC_API_STABLE std::uint32_t kVideoSourceOpenFailed    = 0x1A21;
inline constexpr NIMRTC_API_STABLE std::uint32_t kVideoSourcePluginMissing = 0x1A23;

inline constexpr NIMRTC_API_STABLE std::uint32_t kVideoReceiverOpenFailed    = 0x1A24;
inline constexpr NIMRTC_API_STABLE std::uint32_t kVideoReceiverPluginMissing = 0x1A25;

inline constexpr NIMRTC_API_STABLE std::uint32_t kVideoSenderOpenFailed    = 0x1A26;
inline constexpr NIMRTC_API_STABLE std::uint32_t kVideoSenderPluginMissing = 0x1A27;

inline constexpr NIMRTC_API_STABLE std::uint32_t kVideoCodecOpenFailed    = 0x1A30;
inline constexpr NIMRTC_API_STABLE std::uint32_t kVideoCodecPluginMissing = 0x1A31;

/** The capture-device plugin could not be opened (remote-desktop plan A1).
 *
 *  Reported when `EngineConfig::video_capture_name` names a registered
 *  factory whose `open()` fails — the device could not be acquired, the
 *  requested format was refused, or the platform denied access. */
inline constexpr NIMRTC_API_STABLE std::uint32_t kVideoCaptureOpenFailed = 0x1A40;

/** The capture-device plugin id in `EngineConfig::video_capture_name` is
 *  not registered in the registry's video-capture slot.
 *
 *  A hard failure: the user asked for a capture device by name and the
 *  name does not resolve.  Silently falling back to "no capture device"
 *  would turn a typo into a session that quietly never produces a
 *  frame. */
inline constexpr NIMRTC_API_STABLE std::uint32_t kVideoCapturePluginMissing = 0x1A41;

/** The peer offered a video codec NimRTC cannot decode (C9).
 *
 *  Reported once per negotiation, not once per packet — it is a
 *  configuration condition, the video counterpart of
 *  `kAudioCodecUnsupported`.  The video track is disabled and the
 *  answer rejects the video m-line (RFC 3264 §6.1). */
inline constexpr NIMRTC_API_STABLE std::uint32_t kVideoCodecUnsupported = 0x1A32;

/** The outbound video RTP packet could not be built.
 *
 *  The video path assembles its RTP header through
 *  `rtp::PacketBuilder`; an empty result means the builder rejected
 *  the header/extension combination (e.g. an extension that does not
 *  fit).  The packet is dropped and the error is reported so the
 *  condition is visible instead of silently truncating the stream. */
inline constexpr NIMRTC_API_STABLE std::uint32_t kVideoEncodeFailed = 0x1A33;

// ---------------------------------------------------------------------------
// Audio pipeline — 0x1A40..0x1A4F (v1.2.0)
// ---------------------------------------------------------------------------
// The audio pipeline's own range, next to the audio3A block (0x1A00..0x1A1F)
// and the video block (0x1A20..0x1A3F).  The values are stable per the
// engine error-code compatibility rule.
//
// `kAudioCodecUnsupported` (negotiation-time: the peer offered a codec we
// do not implement) and `kAudioPayloadTypeUnknown` (packet-time: this
// packet's payload type is not in the negotiated routing table) are
// deliberately different codes -- the first is a configuration condition
// that disables the track, the second is a per-packet anomaly that is
// counted and dropped.
inline constexpr NIMRTC_API_STABLE std::uint32_t kAudioCodecUnsupported   = 0x1A40;
inline constexpr NIMRTC_API_STABLE std::uint32_t kAudioDecodeFailed       = 0x1A41;
inline constexpr NIMRTC_API_STABLE std::uint32_t kAudioEncodeFailed       = 0x1A42;
inline constexpr NIMRTC_API_STABLE std::uint32_t kAudioReceiverMissing    = 0x1A43;
inline constexpr NIMRTC_API_STABLE std::uint32_t kAudioClockMismatch      = 0x1A44;
inline constexpr NIMRTC_API_STABLE std::uint32_t kAudioChannelMismatch    = 0x1A45;
inline constexpr NIMRTC_API_STABLE std::uint32_t kAudioDecoderMissing     = 0x1A46;
inline constexpr NIMRTC_API_STABLE std::uint32_t kAudioDecoderUnsupported = 0x1A47;
inline constexpr NIMRTC_API_STABLE std::uint32_t kAudioPayloadTypeUnknown = 0x1A48;

} // namespace nimrtc::core