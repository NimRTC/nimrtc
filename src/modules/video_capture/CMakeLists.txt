# modules/video_capture/CMakeLists.txt
# ----------------------------------------------------------------------------
# The capture-device layer (remote-desktop plan A1 / A2 / A2b / A3 / A4 /
# A5).
#
# This module owns the *portable* half of screen capture:
#
#   - the bounded frame queue with its explicit drop policy,
#   - the dirty-rectangle model,
#   - the format-negotiation contract,
#   - the portable pixel-diff + region crop (the A7 / A8 fallbacks the
#     macOS and Linux backends use), and
#   - the synthetic device that exercises all of it on every CI leg.
#
# The platform backends are thin adapters over this mechanism; they live
# in src/modules/video_capture/src/platform/ and are added here as they
# land:
#
#   - Windows: DXGI Desktop Duplication + Windows Graphics Capture
#     (src/platform/windows/) -- built only on WIN32.
#   - macOS:   ScreenCaptureKit (src/platform/macos/) -- built only on
#     APPLE.
#   - Linux:   XDG Desktop Portal + PipeWire (the Wayland path) and X11
#     (the fallback) (src/platform/linux/) -- built only on UNIX and
#     NOT APPLE.
#
# The *portable* capture core (`src/unix_capture_core.cpp`) and the
# portable device base class (`src/unix_capture.cpp`) are compiled on
# EVERY platform, so the mechanism the macOS and Linux adapters delegate
# to is unit-tested on every CI leg -- including the Windows leg, where
# the adapters themselves cannot be compiled at all.
#
# Per Layout Invariant 3 (AGENTS.md): the module is self-contained --
# own CMakeLists.txt, include/, src/, tests/ -- and depends only on
# nimrtc::core and nimrtc::plugins (the seams it implements).
# ----------------------------------------------------------------------------
set(NIMRTC_VIDEO_CAPTURE_SOURCES
    src/synthetic_capture.cpp
    # The portable capture core (remote-desktop plan A2b / A4 / A5).
    # It is platform-neutral -- no OS header, no OS call -- so it is
    # compiled and unit-tested on EVERY platform, including the Windows
    # leg where the macOS and Linux adapters cannot be compiled at all.
    src/unix_capture_core.cpp
    # The portable capture-device base class + the three factories.
    src/unix_capture.cpp
    # The portable cursor-compositing layer (remote-desktop plan A6).
    # It is platform-neutral -- no OS header, no OS call -- so the blend,
    # the clipping, the recoverability and the pinned fallback are
    # unit-tested on EVERY platform, including the Windows leg where the
    # macOS and Linux cursor sources cannot be compiled at all.
    src/cursor_compositor.cpp
    # The portable cursor-source seam (the pinned fallback + the
    # platform dispatch).
    src/cursor_source.cpp
    # The "no cursor source on this platform" stubs.  Every declaration
    # the public cursor-source header makes must have a definition on
    # every platform -- otherwise a host that names the X11 source on
    # Windows would get a link error instead of the clean "this
    # platform has no cursor source" the header promises.
    src/platform/portable/portable_cursor_stubs.cpp
    # The portable camera capture layer (remote-desktop plan A9).
    src/camera_capture.cpp
    )

# ---------------------------------------------------------------------------
# Windows capture backends (remote-desktop plan A3, milestone M1-c)
#
# DXGI Desktop Duplication needs d3d11 + dxgi; Windows Graphics Capture
# additionally needs the WinRT projection headers (cppwinrt) and
# windowsapp.lib for the activation factory.
# ---------------------------------------------------------------------------
if(WIN32)
    list(APPEND NIMRTC_VIDEO_CAPTURE_SOURCES
        src/platform/windows/d3d11_support.cpp
        src/platform/windows/dxgi_desktop_duplication.cpp
        src/platform/windows/wgc_capture.cpp
        src/platform/windows/windows_capture.cpp
        # The Windows cursor source (remote-desktop plan A6):
        # GetCursorInfo + DrawIconEx into a DIB.
        src/platform/windows/windows_cursor.cpp
        # The Windows camera source (remote-desktop plan A9):
        # Media Foundation's IMFSourceReader.
        src/platform/windows/mf_camera.cpp
        )
    # The feature macros are applied *after* the target exists (see
    # below); this is only the source list here.
endif()

# ---------------------------------------------------------------------------
# macOS capture backend (remote-desktop plan A4, milestone M1).
#
# ScreenCaptureKit is macOS 12.3+ and lives in the ScreenCaptureKit
# framework.  The adapter is written in Objective-C++ (the framework is
# Objective-C), so the Objective-C++ language is enabled here.
# ---------------------------------------------------------------------------
if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

    list(APPEND NIMRTC_VIDEO_CAPTURE_SOURCES
        src/platform/macos/screencapturekit_capture.mm
        src/platform/macos/macos_capture.cpp
        # The macOS camera source (remote-desktop plan A9):
        # AVFoundation's AVCaptureSession.
        src/platform/macos/avfoundation_camera.mm
        )
endif()

# ---------------------------------------------------------------------------
# Linux capture backends (remote-desktop plan A5, milestone M1).
#
# Two paths:
#
#   - **X11** -- the fallback for an X11 session.  It needs the X11
#     development headers (`X11/Xlib.h`).
#   - **PipeWire** -- the Wayland path.  The XDG Desktop Portal owns
#     the source selection and hands back a PipeWire node.
#
# Both are *optional*: when the development packages are missing the
# corresponding adapter is either not compiled or is compiled into a
# stub that reports "unsupported".  The build must never fail for a
# missing optional dependency.
#
# `src/platform/linux/linux_capture.cpp` is the factory glue; it is
# added exactly once (whenever either backend is available).
# ---------------------------------------------------------------------------
if(UNIX AND NOT APPLE)
    set(_nimrtc_linux_factory_glue FALSE)

    # ---- X11 --------------------------------------------------------------
    find_package(X11 QUIET)
    if(X11_FOUND)
        list(APPEND NIMRTC_VIDEO_CAPTURE_SOURCES
            src/platform/linux/x11_capture.cpp
            # The X11 cursor source (remote-desktop plan A6):
            # XFixesGetCursorImage.  It needs the XFixes extension, which
            # is part of the X11 development package.
            src/platform/linux/x11_cursor.cpp
            )
        set(_nimrtc_linux_factory_glue TRUE)
        message(STATUS "video_capture: the X11 capture backend is enabled")
    else()
        message(STATUS
            "video_capture: the X11 capture backend is disabled "
            "(the X11 development headers were not found)")
    endif()

    # ---- V4L2 camera (remote-desktop plan A9) ----------------------------
    #
    # The V4L2 headers live in the Linux kernel's uapi headers, which
    # every build host has.  The adapter needs no extra library.
    list(APPEND NIMRTC_VIDEO_CAPTURE_SOURCES
        src/platform/linux/v4l2_camera.cpp
        )
    set(_nimrtc_linux_factory_glue TRUE)

    # ---- PipeWire ---------------------------------------------------------
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PIPEWIRE QUIET libpipewire-0.3)
        pkg_check_modules(SPA QUIET libspa-0.2)
    endif()

    if(PIPEWIRE_FOUND AND SPA_FOUND)
        list(APPEND NIMRTC_VIDEO_CAPTURE_SOURCES
            src/platform/linux/pipewire_capture.cpp
            )
        set(_nimrtc_linux_factory_glue TRUE)
        # The link libraries are applied *after* the target exists (see
        # below); this is only the source list here.
        message(STATUS "video_capture: the PipeWire capture backend is enabled")
    else()
        message(STATUS
            "video_capture: the PipeWire capture backend is disabled "
            "(libpipewire-0.3 / libspa-0.2 were not found)")
    endif()

    if(_nimrtc_linux_factory_glue)
        list(APPEND NIMRTC_VIDEO_CAPTURE_SOURCES
            src/platform/linux/linux_capture.cpp
            )
    endif()
endif()

# On every platform that has no Unix backend at all (Windows, and a
# Linux box without either development package), the portable stub
# provides the `detail::create_*` entry points, so a host that names a
# Unix backend gets a clean "unsupported" rather than a link error.
if(NOT APPLE AND NOT UNIX)
    list(APPEND NIMRTC_VIDEO_CAPTURE_SOURCES
        src/platform/portable/portable_capture_stubs.cpp
        )
elseif(UNIX AND NOT APPLE AND NOT _nimrtc_linux_factory_glue)
    list(APPEND NIMRTC_VIDEO_CAPTURE_SOURCES
        src/platform/portable/portable_capture_stubs.cpp
        )
endif()

add_library(nimrtc_video_capture STATIC
    ${NIMRTC_VIDEO_CAPTURE_SOURCES}
    )

nimrtc_apply_options(nimrtc_video_capture)
target_link_libraries(nimrtc_video_capture
    PUBLIC
        nimrtc::core
        nimrtc::plugins
    )

target_include_directories(nimrtc_video_capture
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        # The platform adapters include their siblings by relative path
        # (e.g. "macos/screencapturekit_capture.hpp"), so the module's
        # `src/` directory is on the private include path.
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(nimrtc_video_capture
    PUBLIC NIMRTC_HAS_VIDEO_CAPTURE=1)

# ---------------------------------------------------------------------------
# The per-backend feature macros and link libraries that could not be
# applied above (the target did not exist yet at that point).
# ---------------------------------------------------------------------------
if(WIN32)
    target_compile_definitions(nimrtc_video_capture
        PRIVATE NIMRTC_HAS_MEDIA_FOUNDATION_CAMERA=1)
endif()

if(APPLE)
    target_compile_definitions(nimrtc_video_capture
        PRIVATE NIMRTC_HAS_AVFOUNDATION_CAMERA=1)
endif()

if(UNIX AND NOT APPLE)
    target_compile_definitions(nimrtc_video_capture
        PRIVATE NIMRTC_HAS_V4L2_CAMERA=1)
    if(PIPEWIRE_FOUND AND SPA_FOUND)
        target_link_libraries(nimrtc_video_capture
            PRIVATE ${PIPEWIRE_LIBRARIES} ${SPA_LIBRARIES})
        target_link_directories(nimrtc_video_capture
            PRIVATE ${PIPEWIRE_LIBRARY_DIRS} ${SPA_LIBRARY_DIRS})
    endif()
endif()

# ---------------------------------------------------------------------------
# The per-backend feature macros.
#
# They are PRIVATE to this module: they tell the *adapters* whether
# their backend was built, and they are what the availability probes
# (`screen_capture_kit_available()` and friends) report.
# ---------------------------------------------------------------------------
if(APPLE)
    target_compile_definitions(nimrtc_video_capture
        PRIVATE NIMRTC_HAS_SCREEN_CAPTURE_KIT=1)
    target_link_libraries(nimrtc_video_capture
        PRIVATE
            "-framework ScreenCaptureKit"
            "-framework CoreMedia"
            "-framework CoreVideo"
            "-framework CoreGraphics"
            "-framework AVFoundation"
        )
endif()

if(UNIX AND NOT APPLE AND X11_FOUND)
    target_link_libraries(nimrtc_video_capture PRIVATE ${X11_LIBRARIES})
    target_include_directories(nimrtc_video_capture PRIVATE ${X11_INCLUDE_DIR})
    target_compile_definitions(nimrtc_video_capture
        PRIVATE NIMRTC_HAS_X11_CAPTURE=1)
endif()

if(UNIX AND NOT APPLE AND PIPEWIRE_FOUND AND SPA_FOUND)
    target_compile_definitions(nimrtc_video_capture
        PRIVATE NIMRTC_HAS_PIPEWIRE_CAPTURE=1)
endif()

if(WIN32)
    # d3d11 / dxgi  -- the Desktop Duplication + D3D11 surface path.
    # windowsapp    -- the WinRT activation factory used by WGC.
    # mfplat / mfreadwrite / mfuuid -- the Media Foundation
    # camera path (remote-desktop plan A9): the platform entry points,
    # the source reader, and the Media Foundation UUIDs.  (The media
    # type / media buffer factories live in mfplat itself, so no
    # separate mfobjects is needed.)  `mf.lib` carries the device
    # enumeration (`MFEnumDeviceSources`).
    # d3d11.lib's DXGI surface access (IDirect3DDxgiInterfaceAccess) is
    # declared in the SDK header <windows.graphics.directx.direct3d11.interop.h>
    # and resolves against d3d11.lib.
    target_link_libraries(nimrtc_video_capture
        PRIVATE
            d3d11
            dxgi
            windowsapp
            mf
            mfplat
            mfreadwrite
            mfuuid
        )

    # The C++/WinRT projection headers ship with the Windows SDK but are
    # NOT on the default include path -- MSBuild only adds the UM /
    # shared / winrt directories.  They live in the SDK's `cppwinrt`
    # directory; find it through the SDK root.
    #
    # Three sources are tried, in order:
    #   1. the `WindowsSdkDir` environment variable (set inside a Visual
    #      Studio developer prompt),
    #   2. the registry key the Windows SDK installer writes
    #      (`HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots` ->
    #      `KitsRoot10`),
    #   3. the default install locations.
    if(DEFINED ENV{WindowsSdkDir} AND IS_DIRECTORY "$ENV{WindowsSdkDir}")
        set(_nimrtc_sdk_root "$ENV{WindowsSdkDir}")
    else()
        set(_nimrtc_sdk_root "")
        find_program(_nimrtc_reg_exe reg)
        if(_nimrtc_reg_exe)
            execute_process(
                COMMAND "${_nimrtc_reg_exe}" query
                        "HKLM\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots"
                        /v KitsRoot10
                OUTPUT_VARIABLE _nimrtc_reg_output
                ERROR_QUIET
                RESULT_VARIABLE _nimrtc_reg_result
                OUTPUT_STRIP_TRAILING_WHITESPACE
                )
            if(_nimrtc_reg_result EQUAL 0)
                # The answer is on the last line:
                #     KitsRoot10    REG_SZ    D:\Windows Kits\10\
                # Collapse the output to one line first (a CMake regex
                # does not match across a newline), then take the value.
                string(REPLACE "\n" " " _nimrtc_reg_flat "${_nimrtc_reg_output}")
                string(REGEX REPLACE ".*KitsRoot10[ \t]+REG_SZ[ \t]+"
                       "([^ \t\r\n]+).*" "\\1"
                       _nimrtc_sdk_root "${_nimrtc_reg_flat}")
            endif()
        endif()
        if(NOT _nimrtc_sdk_root)
            foreach(_candidate
                    "C:/Program Files (x86)/Windows Kits/10"
                    "C:/Program Files/Windows Kits/10"
                    "D:/Windows Kits/10")
                if(IS_DIRECTORY "${_candidate}/Include")
                    set(_nimrtc_sdk_root "${_candidate}")
                    break()
                endif()
            endforeach()
        endif()
    endif()

    if(_nimrtc_sdk_root)
        file(GLOB _nimrtc_cppwinrt_dirs
             "${_nimrtc_sdk_root}/Include/*/cppwinrt")
        if(_nimrtc_cppwinrt_dirs)
            list(GET _nimrtc_cppwinrt_dirs 0 _nimrtc_cppwinrt_dir)
            target_include_directories(nimrtc_video_capture
                PRIVATE "${_nimrtc_cppwinrt_dir}")
            message(STATUS
                "video_capture: using the C++/WinRT headers in "
                "${_nimrtc_cppwinrt_dir}")
        else()
            message(WARNING
                "video_capture: no C++/WinRT headers found under "
                "${_nimrtc_sdk_root}/Include -- the WGC backend will not "
                "compile.")
        endif()
    else()
        message(WARNING
            "video_capture: the Windows SDK root could not be located -- "
            "the WGC backend will not compile.")
    endif()
endif()

add_library(nimrtc::video_capture ALIAS nimrtc_video_capture)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
