# modules/video_capture/CMakeLists.txt
# ----------------------------------------------------------------------------
# The capture-device layer (remote-desktop plan A1 / A2 / A2b).
#
# This module owns the *portable* half of screen capture:
#
#   - the bounded frame queue with its explicit drop policy,
#   - the dirty-rectangle model,
#   - the format-negotiation contract,
#   - and the synthetic device that exercises all of it on every CI leg.
#
# The three platform backends (Windows DXGI Desktop Duplication /
# Windows Graphics Capture, macOS ScreenCaptureKit, Linux XDG Desktop
# Portal + PipeWire / X11) are thin adapters over this mechanism; they
# live in src/modules/video_capture/src/platform/ and are added here as
# they land (M1-c).
#
# Per Layout Invariant 3 (AGENTS.md): the module is self-contained --
# own CMakeLists.txt, include/, src/, tests/ -- and depends only on
# nimrtc::core and nimrtc::plugins (the seams it implements).
# ----------------------------------------------------------------------------
add_library(nimrtc_video_capture STATIC
    src/synthetic_capture.cpp
    src/capture_source_adapter.cpp
    src/cursor_compositor.cpp
    )

# Remote-desktop plan A3: the Windows display-capture backend.  The
# file is compiled on every platform because it carries a portable
# stub (the class exists everywhere; on a non-Windows host `open()`
# fails with kErrUnsupported).  Only the *link* libraries are
# Windows-only: d3d11 (the device and the staging texture) and dxgi
# (the factory and the duplication object).
target_sources(nimrtc_video_capture PRIVATE
    src/platform/dxgi_desktop_dup.cpp
    src/platform/win_cursor_source.cpp
    src/platform/wgc_capture.cpp
    )

if(WIN32)
    target_link_libraries(nimrtc_video_capture PRIVATE
        d3d11
        dxgi
        # The cursor source reads the pointer through user32
        # (GetCursorInfo / GetIconInfo / GetDIBits).
        user32
        gdi32
        # The WGC backend talks to the WinRT capture APIs: the
        # activation factory (RoGetActivationFactory), the HSTRING
        # helpers (WindowsCreateString) and the C++/WinRT projection's
        # runtime support all live in windowsapp.lib.
        windowsapp
        )
endif()

nimrtc_apply_options(nimrtc_video_capture)
target_link_libraries(nimrtc_video_capture
    PUBLIC
        nimrtc::core
        nimrtc::plugins
    )
# The capture -> VideoSourceFrame adapter needs the video_frame module's
# layout maths.  `nimrtc::plugins` already re-exports the video_frame
# types, so this is a link-time dependency only.
target_link_libraries(nimrtc_video_capture
    PUBLIC
        nimrtc::video_frame
    )

target_include_directories(nimrtc_video_capture
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_video_capture
    PUBLIC NIMRTC_HAS_VIDEO_CAPTURE=1)

add_library(nimrtc::video_capture ALIAS nimrtc_video_capture)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
