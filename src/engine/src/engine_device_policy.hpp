/**
 * @file src/engine/src/engine_device_policy.hpp
 * @brief PRIVATE engine collaborator: the **default device policy**
 *        (v1.4 plan D3).
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * ## What this file is
 *
 * A host that wants a screen or a camera used to have to *know* which
 * backend to ask for, and the answer was platform-shaped:
 * `"windows-wgc"` on Windows, `"macos-screencapturekit"` on macOS,
 * `"linux-x11"` on Linux.  That table did not live in the library --
 * it lived in `examples/remote-desktop/rd_device.cpp`, in a function
 * called `default_capture_name()`.
 *
 * That is the wrong place for it, and the reason is not tidiness: a
 * table that only the example can read is a table the *library* cannot
 * honour.  A host that links NimRTC and writes its own application --
 * which is the SDK claim the whole plan is about -- has no way to ask
 * "what does this platform capture with by default?", because the
 * answer is in an example it did not copy.
 *
 * So the table moves here, into the engine, and it becomes
 * **policy**: the same shape the profile system already uses, with the
 * same override rule.
 *
 * ## The override rule
 *
 * `EngineConfig::video_capture_name` keeps its meaning exactly:
 *
 *   - **empty** -- the host feeds frames itself.  Unchanged.  (This
 *     matters: a lot of existing hosts and tests use the empty string
 *     to mean "no capture device", and silently changing that would
 *     open a real camera on every one of them.)
 *   - **a concrete id** -- the host named a backend.  Always wins.
 *   - **`"auto"`** -- the host asked for the *policy*.  This is a new
 *     value of an existing field, not a new field.
 *
 * `"auto"` rather than "empty means the policy" is deliberate.  The
 * profile system uses "empty = the policy", because an empty profile
 * key has no other meaning.  `video_capture_name` does have another
 * meaning, so "auto" has to be said out loud.
 *
 * ## A policy that does not hold is not an error
 *
 * If the policy names a backend that this build does not have (a
 * Linux build with no V4L2 camera, say), the engine continues with
 * *no capture device* and warns once.  That is deliberately different
 * from an unknown **explicit** id, which is reported as
 * `kVideoCapturePluginMissing`:
 *
 *   - an explicit id that does not resolve is a **typo**, and a typo
 *     that silently produced no video would be a session that quietly
 *     never shows a picture;
 *   - a policy id that does not resolve is a **suggestion**, and the
 *     platform may simply not have that device.
 *
 * The plan's §5 D3-1 decides *which* backend the policy names: the
 * content hint decides screen vs. camera, because the product intent
 * already lives there.  `kText` is screen content (a terminal, a
 * spreadsheet -- the remote-desktop shape); anything else is a camera
 * (the conference shape).
 */

#pragma once

#include <string>
#include <string_view>

namespace nimrtc::engine::detail {

/**
 * @brief One default backend per device class.
 *
 * An empty string means "this platform has no default for this class"
 * -- which is a *fact about the platform*, not a failure.
 */
struct DevicePolicy {
    /** Screen-capture backend plugin id ("" = none on this platform). */
    std::string screen_capture_backend;
    /** Camera-capture backend plugin id ("" = none on this platform). */
    std::string camera_capture_backend;
    /** Audio capture backend plugin id ("" = none on this platform). */
    std::string audio_capture_backend;
    /** Audio playback backend plugin id ("" = none on this platform). */
    std::string audio_playback_backend;
};

/**
 * @brief The current platform's default device policy.
 *
 * The table is the one that used to live in the example, moved here
 * unchanged -- this slice adds no backend and registers no plugin.
 */
DevicePolicy default_device_policy() noexcept;

/**
 * @brief Resolve the backend id to use, given what the host asked for.
 *
 * @param explicit_id  The host's `video_capture_name`:
 *                     "" (host feeds frames itself), "auto" (use the
 *                     policy), or a concrete backend id.
 * @param policy       The platform policy.
 * @param want_camera  True to resolve the *camera* backend, false for
 *                     the screen backend.
 * @return The backend id to use, or "" when there is none.
 *
 * The rule, in order:
 *
 *   1. a concrete `explicit_id` wins -- the host always wins;
 *   2. otherwise the policy's value for the requested class;
 *   3. otherwise "" (no capture device).
 */
std::string resolve_capture_backend(std::string_view explicit_id,
                                    const DevicePolicy& policy,
                                    bool want_camera) noexcept;

/**
 * @brief Whether a content hint asks for a screen rather than a camera.
 *
 * `kText` is screen content; everything else (including
 * `kUnspecified`) is a camera.  The plan's §5 D3-1 puts the decision
 * here so the profile -- which already knows the product intent --
 * stays the only thing that has to be configured.
 */
bool content_hint_wants_screen(int content_hint) noexcept;

} // namespace nimrtc::engine::detail
