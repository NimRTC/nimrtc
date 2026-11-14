// modules/video_capture/src/platform/portable/portable_cursor_stubs.cpp
// ----------------------------------------------------------------------------
// The "no cursor source on this platform" stubs.
//
// The cursor *sources* are platform adapters: the Windows one
// (`GetCursorInfo` + `DrawIconEx`) and the X11 one
// (`XFixesGetCursorImage`) are each compiled only on the platform that
// owns them, and behind the CMake guard that decides whether the
// platform's development headers were found.
//
// The portable factory layer (`src/cursor_source.cpp`) dispatches to
// them, so every declaration the public header makes must have a
// definition on every platform -- otherwise a host that names the
// X11 source on Windows would get a *link error* instead of the clean
// "this platform has no cursor source" the header promises.
//
// These stubs provide exactly that: a null source, which the caller
// reads as "unsupported".
// ----------------------------------------------------------------------------

#include <nimrtc/video_capture/cursor_source.hpp>

namespace nimrtc::video_capture {

#if !defined(_WIN32)
// The Windows cursor source is compiled only on Windows.
std::shared_ptr<ICursorSource> make_windows_cursor_source() noexcept {
    return nullptr;
}
#endif // !defined(_WIN32)

#if !defined(NIMRTC_HAS_X11_CAPTURE)
// The X11 cursor source is compiled only when the X11 development
// headers were found at configure time (and, of course, only on Linux).
std::shared_ptr<ICursorSource> make_x11_cursor_source() noexcept {
    return nullptr;
}
#endif // !defined(NIMRTC_HAS_X11_CAPTURE)

} // namespace nimrtc::video_capture
