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
    src/dirty_rect_diff.cpp
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
    src/platform/x11_capture.cpp
    src/platform/x11_cursor_source.cpp
    src/platform/portal_capture.cpp
    )

# -----------------------------------------------------------------------------
# The macOS capture backend (remote-desktop plan A4).
#
# ScreenCaptureKit is an Objective-C framework, so the backend is
# Objective-C++ (`.mm`).  It is compiled on every platform because it
# carries a portable stub — the class exists everywhere, and on a
# non-macOS host `open()` fails with kErrUnsupported.  Only the *link*
# libraries are macOS-only:
#
#   - ScreenCaptureKit — the capture stream itself,
#   - AVFoundation     — the sample buffers' media types,
#   - CoreMedia        — CMSampleBuffer,
#   - CoreVideo        — CVPixelBuffer,
#   - CoreGraphics     — the screen-recording permission probe.
# -----------------------------------------------------------------------------
if(APPLE)
    target_sources(nimrtc_video_capture PRIVATE
        src/platform/screen_capture_kit.mm
        )
    target_link_libraries(nimrtc_video_capture PRIVATE
        "-framework ScreenCaptureKit"
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework CoreGraphics"
        )
    message(STATUS "  ScreenCaptureKit  : enabled (macOS)")
else()
    # The portable stub of the same backend, so that a host can name
    # "sck" in a cross-platform configuration and get a clean
    # "unsupported" rather than "plugin missing".
    target_sources(nimrtc_video_capture PRIVATE
        src/platform/screen_capture_kit_stub.cpp
        )
endif()

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

# -----------------------------------------------------------------------------
# The Linux capture backends (remote-desktop plan A5).
#
# Two of them, with very different needs:
#
#   * **X11** — `XGetImage` needs the X11 client library and the
#     Xfixes extension (for the cursor source).  Both are core X11
#     packages that any desktop install has.
#   * **XDG Portal + PipeWire** — the portal needs GIO (for the D-Bus
#     session bus) and the PipeWire client library.
#
# Neither is a hard requirement: when the libraries are absent the
# backends compile to their portable stub (the class exists, the
# factory is not registered, `open()` reports kErrUnsupported), so a
# Linux build without them still works — it just has no screen capture
# on Linux.
# -----------------------------------------------------------------------------
if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        # ---- X11 + Xfixes --------------------------------------------------
        pkg_check_modules(NIMRTC_X11 QUIET x11 xfixes)
        if(NIMRTC_X11_FOUND)
            # SYSTEM: the third-party headers are not held to this
            # project's warning set (X11's own macros trip
            # -Wold-style-cast and friends).
            target_include_directories(nimrtc_video_capture SYSTEM PRIVATE
                ${NIMRTC_X11_INCLUDE_DIRS})
            target_link_libraries(nimrtc_video_capture PRIVATE
                ${NIMRTC_X11_LIBRARIES})
            target_compile_definitions(nimrtc_video_capture PUBLIC
                NIMRTC_HAVE_X11=1)
            message(STATUS "  X11 capture        : found "
                           "(${NIMRTC_X11_LIBRARIES})")
        else()
            message(STATUS "  X11 capture        : not found (install "
                           "libx11-dev libxfixes-dev)")
        endif()

        # ---- GIO (the portal's D-Bus client) --------------------------------
        pkg_check_modules(NIMRTC_GIO QUIET gio-2.0)
        if(NIMRTC_GIO_FOUND)
            target_include_directories(nimrtc_video_capture SYSTEM PRIVATE
                ${NIMRTC_GIO_INCLUDE_DIRS})
            target_link_libraries(nimrtc_video_capture PRIVATE
                ${NIMRTC_GIO_LIBRARIES})
            target_compile_definitions(nimrtc_video_capture PUBLIC
                NIMRTC_HAVE_GIO=1)
        else()
            message(STATUS "  XDG portal capture : not found (install "
                           "libglib2.0-dev)")
        endif()

        # ---- PipeWire -------------------------------------------------------
        pkg_check_modules(NIMRTC_PIPEWIRE QUIET libpipewire-0.3)
        if(NIMRTC_PIPEWIRE_FOUND)
            target_include_directories(nimrtc_video_capture SYSTEM PRIVATE
                ${NIMRTC_PIPEWIRE_INCLUDE_DIRS})
            target_link_libraries(nimrtc_video_capture PRIVATE
                ${NIMRTC_PIPEWIRE_LIBRARIES})
            target_compile_definitions(nimrtc_video_capture PUBLIC
                NIMRTC_HAVE_PIPEWIRE=1)
            message(STATUS "  PipeWire capture   : found "
                           "(${NIMRTC_PIPEWIRE_LIBRARIES})")
        else()
            message(STATUS "  PipeWire capture   : not found (install "
                           "libpipewire-0.3-dev)")
        endif()
    endif()
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
