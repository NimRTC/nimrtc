/**
 * @file src/engine/src/engine_device_policy.cpp
 * @brief Implementation of the default device policy (v1-device-policy D3).
 */

#include "engine_device_policy.hpp"

#include <array>
#include <string>
#include <string_view>

namespace nimrtc::engine::detail {

DevicePolicy default_device_policy() noexcept {
#if defined(_WIN32)
    return {.screen_capture_backend = "windows-wgc",
            .camera_capture_backend = "camera-mediafoundation",
            .audio_capture_backend = "",
            .audio_playback_backend = ""};
#elif defined(__APPLE__)
    return {.screen_capture_backend = "macos-screencapturekit",
            .camera_capture_backend = "camera-avfoundation",
            .audio_capture_backend = "",
            .audio_playback_backend = ""};
#elif defined(__linux__)
    return {.screen_capture_backend = "linux-x11",
            .camera_capture_backend = "camera-v4l2",
            .audio_capture_backend = "",
            .audio_playback_backend = ""};
#else
    return {.screen_capture_backend = "synthetic",
            .camera_capture_backend = "synthetic-camera",
            .audio_capture_backend = "",
            .audio_playback_backend = ""};
#endif
}

std::string resolve_capture_backend(std::string_view explicit_id,
                                    const DevicePolicy& policy,
                                    bool want_camera) noexcept {
    if (!explicit_id.empty()) {
        return std::string(explicit_id);
    }
    if (want_camera) {
        return policy.camera_capture_backend;
    }
    return policy.screen_capture_backend;
}

std::string resolve_auto_capture_backend(const DevicePolicy& policy,
                                         bool want_camera,
                                         bool (*is_registered)(
                                             std::string_view id) noexcept) noexcept {
    const std::string resolved =
        resolve_capture_backend("", policy, want_camera);
    if (resolved.empty() || is_registered(resolved)) {
        return resolved;
    }
    // The policy named a backend this build does not register.  That is a
    // *recommendation*, not a host requirement, so it degrades to "no
    // capture device" instead of firing kVideoCapturePluginMissing.
    return "";
}

namespace {
// The "auto" default order.  Capture and playback share the same order
// today ("portaudio" when usable, else "synthetic"); the direction is
// still a parameter so a backend that is capture-only (the system-audio
// loopback) can join the capture list without changing this signature.
constexpr std::array<std::string_view, 2> kAutoAudioDefaultsCapture = {
    "portaudio", "synthetic"};
constexpr std::array<std::string_view, 2> kAutoAudioDefaultsPlayback = {
    "portaudio", "synthetic"};
}  // namespace

std::string resolve_auto_audio_backend(bool capture,
                                       bool (*is_registered)(
                                           std::string_view id) noexcept) noexcept {
    const std::array<std::string_view, 2>& defaults = capture
        ? kAutoAudioDefaultsCapture
        : kAutoAudioDefaultsPlayback;
    for (std::string_view id : defaults) {
        if (is_registered(id)) {
            return std::string(id);
        }
    }
    // Neither backend is registered on this build.  "auto" is a
    // recommendation, not a host requirement, so it degrades to "no
    // audio device" instead of failing open().
    return "";
}

}  // namespace nimrtc::engine::detail
