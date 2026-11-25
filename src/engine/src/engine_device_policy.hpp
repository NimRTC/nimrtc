/**
 * @file src/engine/src/engine_device_policy.hpp
 * @brief The per-platform default device policy (v1-device-policy D3).
 *
 * ## What this file is
 *
 * The default capture-backend table that used to live in the remote-desktop
 * example's `default_capture_name()` now lives here, in the engine core.
 * The engine is the owner of "what a host gets when it asks for the
 * platform default", so every host gets the same table without copying it.
 *
 * The file is **private** (src/engine/src/, compiled into nimrtc_engine
 * only): the table is a list of plugin ids, which the core library must
 * not know, and the device modules must not depend on.
 *
 * It is a plain function pair (no dependence on the engine, the plugins
 * or the core library) so the resolution rules are pinned by a
 * **deterministic** unit test rather than by driving a real capture
 * backend (`src/engine/tests/test_engine_device_policy.cpp`).
 */

#pragma once

#include <string>
#include <string_view>

namespace nimrtc::engine::detail {

/** One default device policy: a default plugin id per device kind. */
struct DevicePolicy {
    /** Screen-capture backend plugin id ("" = no default on this
     *  platform). */
    std::string screen_capture_backend;
    /** Camera backend plugin id ("" = no default on this platform). */
    std::string camera_capture_backend;
    /** Audio-capture device plugin id -- **reserved**: filled in by D1
     *  (engine-driven device layer).  Deliberately empty today. */
    std::string audio_capture_backend;
    /** Audio-playback device plugin id -- **reserved**: filled in by D1. */
    std::string audio_playback_backend;
};

/** The current platform's default device policy.  Never throws. */
DevicePolicy default_device_policy() noexcept;

/**
 * Fold the host's explicit id with the policy into the final id.
 *
 * Priority (mirrors the profile system's override rule):
 *   1. a non-empty `explicit_id` wins verbatim (the host always wins);
 *   2. otherwise the policy's value for the wanted direction;
 *   3. an empty policy value returns "" -- the engine then installs no
 *      capture device.
 *
 * `want_camera` selects the direction: false -> screen backend, true ->
 * camera backend.  This is the D3-1 mapping (`video_content_hint ==
 * kText` -> screen, everything else -> camera).
 */
std::string resolve_capture_backend(std::string_view explicit_id,
                                    const DevicePolicy& policy,
                                    bool want_camera) noexcept;

/**
 * The engine's "auto" rule (D3 + the degrade rule).
 *
 * "auto" resolves through the policy exactly like
 * `resolve_capture_backend("", ...)`, then applies the one extra rule:
 * a policy backend id that is **not registered** on this build is not an
 * error -- the engine degrades to "no capture device" (warn + "") rather
 * than firing `kVideoCapturePluginMissing`.  A policy with no default
 * backend also degrades to "".
 *
 * `is_registered` is the registry-membership check, injected so the
 * rule can be unit-tested without a process-wide registry.
 */
std::string resolve_auto_capture_backend(const DevicePolicy& policy,
                                         bool want_camera,
                                         bool (*is_registered)(
                                             std::string_view id) noexcept) noexcept;

/**
 * The engine's audio "auto" rule (v1-audio-device-engine D1).
 *
 * Audio "auto" resolves through **runtime availability**, not the device
 * policy table: the reserved `audio_capture_backend` /
 * `audio_playback_backend` fields are deliberately empty (v1-device-policy
 * D3), and whether the PortAudio backend is usable is only known at run
 * time.  Registry membership *is* the availability signal, because the
 * audio_device module registers "portaudio" only when
 * `portaudio_available()` is true.
 *
 * The rule picks the first registered candidate in order: "portaudio"
 * when usable, else "synthetic".  If neither is registered on this build
 * it returns "" -- the engine then degrades to "no audio device" (warn)
 * rather than failing `open()`.
 *
 * `capture` selects the slot direction (the audio-source registry for
 * capture, the audio-sink registry for playback); `is_registered` is the
 * registry-membership check, injected so the rule can be unit-tested
 * without a process-wide registry (same shape as
 * `resolve_auto_capture_backend`).
 */
std::string resolve_auto_audio_backend(bool capture,
                                       bool (*is_registered)(
                                           std::string_view id) noexcept) noexcept;

}  // namespace nimrtc::engine::detail
