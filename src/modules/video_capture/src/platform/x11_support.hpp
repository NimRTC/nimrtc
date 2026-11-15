// modules/video_capture/src/platform/x11_support.hpp
// ----------------------------------------------------------------------------
// The X11 bits that the capture device and the cursor source share.
//
// Kept in a header of its own because Xlib's error handler is a
// *process-global* setting: whichever of the two installs it must be
// the same handler, and it must be installed exactly once.
//
// See include/nimrtc/video_capture/x11_capture.hpp and
// include/nimrtc/video_capture/x11_cursor_source.hpp.
// ----------------------------------------------------------------------------
#ifndef NIMRTC_VIDEO_CAPTURE_X11_SUPPORT_HPP
#define NIMRTC_VIDEO_CAPTURE_X11_SUPPORT_HPP

#if defined(__linux__) && defined(NIMRTC_HAVE_X11)

#  include <X11/Xlib.h>

#  include <atomic>
#  include <mutex>

namespace nimrtc::video_capture::x11_detail {

/**
 * @brief The number of X protocol errors seen since the process started.
 *
 * A capture device that fails to grab a frame because the server
 * refused it is a *device error*, not a crash — and the counter is how
 * the backend tells the two apart.
 */
inline std::atomic<int>& x11_error_count() {
    static std::atomic<int> count{0};
    return count;
}

/**
 * @brief The X11 error handler installed in place of Xlib's default.
 *
 * Xlib's default handler prints the error and calls `exit()`.  For a
 * library that is unacceptable: an X server that refuses one grab (a
 * screen that was hot-unplugged, a root window that is being resized)
 * must not take the whole process down with it.
 *
 * Returning 0 tells Xlib the error was handled: it will not abort.
 */
inline int handle_x11_error(Display*, XErrorEvent*) {
    x11_error_count().fetch_add(1, std::memory_order_relaxed);
    return 0;
}

/**
 * @brief Install the non-fatal error handler.
 *
 * Idempotent: the handler is installed once per process and is never
 * removed, because Xlib offers no way to ask "is a handler already
 * installed?" and removing it would race with the other X11 client in
 * the process (the cursor source).  The handler it replaces only
 * aborts, so nothing that works before the installation stops working
 * after it.
 */
inline void install_x11_error_handler() {
    static std::once_flag once;
    std::call_once(once, []() { XSetErrorHandler(handle_x11_error); });
}

} // namespace nimrtc::video_capture::x11_detail

#endif // __linux__ && NIMRTC_HAVE_X11

#endif // NIMRTC_VIDEO_CAPTURE_X11_SUPPORT_HPP
