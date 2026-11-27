/**
 * @file nimrtc/plugins/capture_device_events.hpp
 * @brief Optional seam: a capture device that can report **device-list
 *        changes** (v1.4 plan D4).
 *
 * ## Why this is a *separate* interface
 *
 * `plugins::IVideoCapture` is a **STABLE public header**, and a host
 * may have implemented it themselves.  Adding a pure virtual function
 * to it would break every such host at compile time -- the kind of
 * break a library cannot take back.
 *
 * So the capability is expressed the way the tree already expresses an
 * *optional* capability: a separate interface the engine probes with a
 * `dynamic_cast`.  A device that does not implement it simply does not
 * report changes, and the engine falls back to polling the device list
 * itself.
 *
 * ## What the engine does with the events
 *
 * The events are a **hint**, not a command.  The engine still checks
 * whether the device it holds is still there, because a device that
 * disappeared without saying so (a driver crash, a yanked USB cable on
 * a hub that does not report) still has to be noticed.  What the
 * interface buys is *timeliness*: the engine learns about the change on
 * the tick that it happened rather than on the tick after the next
 * enumeration.
 *
 * The engine's own counters (`video_capture_device_changes` and
 * `video_capture_restarts`) are the observable record; see the engine
 * header.
 */

#ifndef NIMRTC_PLUGINS_CAPTURE_DEVICE_EVENTS_HPP
#define NIMRTC_PLUGINS_CAPTURE_DEVICE_EVENTS_HPP

#include <cstdint>
#include <string>
#include <vector>

#include <nimrtc/plugins/base.hpp>
#include <nimrtc/plugins/video_capture.hpp>

namespace nimrtc::plugins {

/** What happened to the capture device list. */
enum class NIMRTC_API_STABLE CaptureDeviceChange : std::uint8_t {
    kAdded   = 0,   ///< a device appeared
    kRemoved = 1,   ///< a device disappeared
};

/** One change to the capture device list. */
struct NIMRTC_API_STABLE CaptureDeviceChangeEvent {
    /** What happened. */
    CaptureDeviceChange change = CaptureDeviceChange::kAdded;

    /** The device's id -- the same id `enumerate_devices()` reports. */
    std::string device_id;

    /** The device's name, when the backend knows it. */
    std::string device_name;

    /** What kind of device it is, when the backend knows it. */
    CaptureDeviceKind kind = CaptureDeviceKind::kUnknown;
};

/**
 * @brief Optional capability: a capture device that can report changes
 *        to the device list.
 *
 * This is an **independent interface**, not a member of
 * `IVideoCapture` -- see the file comment for why.
 */
class NIMRTC_API_STABLE ICaptureDeviceEventSource {
public:
    virtual ~ICaptureDeviceEventSource() = default;

    /**
     * @brief The changes since the last call.
     *
     *  The returned list is **drained**: calling this again returns
     *  only what happened since.  A source that has nothing to report
     *  returns an empty vector.
     *
     *  A source that cannot tell (a backend with no change
     *  notification at all) returns an empty vector forever -- the
     *  engine then polls the device list itself, which is exactly the
     *  fallback it needs.
     */
    [[nodiscard]] virtual std::vector<CaptureDeviceChangeEvent>
    take_device_changes() noexcept = 0;
};

} // namespace nimrtc::plugins

#endif // NIMRTC_PLUGINS_CAPTURE_DEVICE_EVENTS_HPP
