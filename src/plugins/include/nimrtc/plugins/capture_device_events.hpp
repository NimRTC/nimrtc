/**
 * @file src/plugins/include/nimrtc/plugins/capture_device_events.hpp
 * @brief Optional capability: a capture device that can report device-list
 *        changes (v1-device-policy D4).
 *
 * ## Why this is a separate interface
 *
 * `plugins::IVideoCapture` is a STABLE public header.  Adding a pure
 * virtual to it would break every existing implementer (external hosts
 * may implement it themselves), so the "device hot-plug" capability is a
 * **separate, independent interface** in its own header.  The engine
 * probes it with a `dynamic_cast` (the same extension pattern used by
 * `plugins::ITransport` / `plugins::IIceTransport`, see
 * `ice_transport.hpp`): a device that implements `ICaptureDeviceEventSource`
 * reports structured change events; a device that does not falls back to
 * engine-side polling of `IVideoCapture::enumerate_devices()`.
 *
 * The camera backends (Media Foundation / AVFoundation / V4L2) do NOT
 * implement this in v1-device-policy — wiring native hot-plug
 * notifications (MF's `KSCaptureDevice`, `AVCaptureDevice` session
 * notifications, V4L2's `VIDIOC_EVENT`) is deliberately deferred.  The
 * seam must exist so that later work can use native notifications
 * without another breaking change to `IVideoCapture`.
 *
 * @note v1-device-policy D4. Interface is stable.
 */

// base.hpp must be before include guard — see transport.hpp for rationale.
#include <nimrtc/core/api_version.hpp>
#include "nimrtc/plugins/base.hpp"
// `CaptureDeviceKind` lives in the capture seam (`CaptureDevice`).
#include "nimrtc/plugins/video_capture.hpp"

#ifndef NIMRTC_PLUGINS_CAPTURE_DEVICE_EVENTS_HPP
#define NIMRTC_PLUGINS_CAPTURE_DEVICE_EVENTS_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).

#include <cstdint>
#include <string>
#include <vector>

namespace nimrtc::plugins {

/** What kind of change a device-list event describes. */
enum class NIMRTC_API_STABLE CaptureDeviceChange : std::uint8_t {
    kAdded   = 0,  ///< a device appeared
    kRemoved = 1,  ///< a device disappeared
};

/** One device-list change. */
struct NIMRTC_API_STABLE CaptureDeviceChangeEvent {
    CaptureDeviceChange change = CaptureDeviceChange::kAdded;
    /** The affected device's stable id (as reported by
     *  `IVideoCapture::enumerate_devices()`). */
    std::string device_id;
    /** The affected device's human-readable name, if known. */
    std::string device_name;
    CaptureDeviceKind kind = CaptureDeviceKind::kUnknown;
};

/**
 * @brief Optional capability: a capture device that can report device-list
 *        changes.
 *
 * This is a separate interface, NOT members of `IVideoCapture` — adding
 * pure virtuals to `IVideoCapture` would break every existing implementer
 * of the stable capture seam.
 *
 * The engine probes it with `dynamic_cast` on every capture poll; a
 * device that implements it must return the changes accumulated **since
 * the last call** and then clear them.
 */
class NIMRTC_API_STABLE ICaptureDeviceEventSource {
public:
    virtual ~ICaptureDeviceEventSource() = default;

    /** The changes since the last call, oldest first (cleared on return). */
    virtual std::vector<CaptureDeviceChangeEvent>
    take_device_changes() noexcept = 0;
};

}  // namespace nimrtc::plugins

#endif  // NIMRTC_PLUGINS_CAPTURE_DEVICE_EVENTS_HPP
