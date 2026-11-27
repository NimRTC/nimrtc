// src/engine/src/engine_device_policy.cpp
// =============================================================================
// The default device policy (v1.4 plan D3).
//
// The table below is the one that used to live in
// `examples/remote-desktop/rd_device.cpp` as `default_capture_name()`,
// moved here unchanged.  See the header for *why* it moves and for the
// override rule.
//
// Two things are worth reading before the code:
//
//   1. The ids are **plugin ids the registry already knows**.  This
//      slice registers nothing and adds no backend; it only names the
//      ones that exist.
//
//   2. The audio rows are filled in **after** D1 landed.  D1 gave the
//      engine the audio device plane, so the policy can now name the
//      audio backends too -- and an empty row would have been a
//      statement that the platform has no default audio device, which
//      is false.
// =============================================================================

#include "engine_device_policy.hpp"

namespace nimrtc::engine::detail {

namespace {

#ifdef _WIN32
constexpr const char* kDefaultScreenBackend = "windows-wgc";
constexpr const char* kDefaultCameraBackend = "camera-mediafoundation";
constexpr const char* kDefaultAudioCapture  = "portaudio";
constexpr const char* kDefaultAudioPlayback = "portaudio";
#elif defined(__APPLE__)
constexpr const char* kDefaultScreenBackend = "macos-screencapturekit";
constexpr const char* kDefaultCameraBackend = "camera-avfoundation";
constexpr const char* kDefaultAudioCapture  = "portaudio";
constexpr const char* kDefaultAudioPlayback = "portaudio";
#elif defined(__linux__)
constexpr const char* kDefaultScreenBackend = "linux-x11";
constexpr const char* kDefaultCameraBackend = "camera-v4l2";
constexpr const char* kDefaultAudioCapture  = "portaudio";
constexpr const char* kDefaultAudioPlayback = "portaudio";
#else
constexpr const char* kDefaultScreenBackend = "synthetic";
constexpr const char* kDefaultCameraBackend = "synthetic-camera";
constexpr const char* kDefaultAudioCapture  = "";
constexpr const char* kDefaultAudioPlayback = "";
#endif

// The value the host writes to ask for the policy.
constexpr const char* kAutoBackendId = "auto";

// `plugins::ContentHint::kText` (src/plugins/include/nimrtc/plugins/
// video_codec.hpp) -- the hint that means "this is screen content".
//
// The value is spelled out rather than named so that a change to the
// enum cannot silently repoint the policy.  The comparison is on the
// integer on purpose: this collaborator is private and must not pull
// `plugins::ContentHint` into a header the plugin seam would then have
// to include.
constexpr int kContentHintText = 3;

} // namespace

DevicePolicy default_device_policy() noexcept {
    DevicePolicy policy;
    policy.screen_capture_backend = kDefaultScreenBackend;
    policy.camera_capture_backend = kDefaultCameraBackend;
    policy.audio_capture_backend  = kDefaultAudioCapture;
    policy.audio_playback_backend = kDefaultAudioPlayback;
    return policy;
}

std::string resolve_capture_backend(std::string_view explicit_id,
                                    const DevicePolicy& policy,
                                    bool want_camera) noexcept {
    // 1. The empty string is **not** "auto".  It means "the host feeds
    //    frames itself", and it stays empty.  Silently turning it into
    //    the policy would open a real camera on every host that wrote
    //    `video_capture_name = ""` to mean "no capture device" -- and
    //    there are a lot of those.
    if (explicit_id.empty()) {
        return {};
    }

    // 2. The host's explicit id wins -- always, and whatever it says.
    //    "auto" is *not* explicit: it is the host asking for the policy,
    //    so it falls through to step 3.
    if (explicit_id != kAutoBackendId) {
        return std::string(explicit_id);
    }

    // 3. The policy's value for the requested class.
    const std::string& from_policy =
        want_camera ? policy.camera_capture_backend : policy.screen_capture_backend;

    // 4. No policy value -- there is no capture device on this platform
    //    for this class.  The engine continues without one; it is not an
    //    error, and the caller warns.
    return from_policy;
}

bool content_hint_wants_screen(int content_hint) noexcept {
    // The plan's §5 D3-1: the *profile* knows the product intent, so it
    // is the profile that says "this is screen content".  The hint
    // carries that: kText is screen content (a terminal, a spreadsheet
    // -- the remote-desktop shape), and everything else -- including
    // "no hint" -- is a camera (the conference shape).
    return content_hint == kContentHintText;
}

} // namespace nimrtc::engine::detail
