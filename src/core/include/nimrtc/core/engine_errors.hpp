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

#include <cstdint>

namespace nimrtc::core {

// ---------------------------------------------------------------------------
// Engine lifecycle / state
// ---------------------------------------------------------------------------
constexpr std::uint32_t kEngineInvalidParam = 0x1001;  // == plugins::kErrInvalidParam
constexpr std::uint32_t kEngineNotReady     = 0x1002;  // == plugins::kErrNotReady
constexpr std::uint32_t kEngineSdpCorrupt   = 0x1004;  // == plugins::kErrCorrupt
constexpr std::uint32_t kEngineInternal     = 0x1FFF;  // == plugins::kErrInternal

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
constexpr std::uint32_t kDtlsOpenFailed     = 0x2000;  // dtls_->open() returned false
constexpr std::uint32_t kDtlsHandshakeFail  = 0x2001;  // reserved (peer rejected)
constexpr std::uint32_t kDtlsKeyDeriveFail  = 0x2002;  // reserved

// ---------------------------------------------------------------------------
// Audio3A plugin loader — 0x1A00..0x1A1F
// ---------------------------------------------------------------------------
constexpr std::uint32_t kAudio3APluginOpenFailed = 0x1A00;
constexpr std::uint32_t kAudio3APluginMissing    = 0x1A01;  // reserved

// ---------------------------------------------------------------------------
// Video plugin loader — 0x1A20..0x1A3F
// ---------------------------------------------------------------------------
// Each subsystem reserves 4 codes: open failure, factory null, not-found,
// reserved.
constexpr std::uint32_t kVideoSinkOpenFailed     = 0x1A20;
constexpr std::uint32_t kVideoSinkPluginMissing  = 0x1A22;

constexpr std::uint32_t kVideoSourceOpenFailed    = 0x1A21;
constexpr std::uint32_t kVideoSourcePluginMissing = 0x1A23;

constexpr std::uint32_t kVideoReceiverOpenFailed    = 0x1A24;
constexpr std::uint32_t kVideoReceiverPluginMissing = 0x1A25;

constexpr std::uint32_t kVideoSenderOpenFailed    = 0x1A26;
constexpr std::uint32_t kVideoSenderPluginMissing = 0x1A27;

constexpr std::uint32_t kVideoCodecOpenFailed    = 0x1A30;
constexpr std::uint32_t kVideoCodecPluginMissing = 0x1A31;

} // namespace nimrtc::core