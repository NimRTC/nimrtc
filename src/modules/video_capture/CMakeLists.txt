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
    # The portable pixel-diff (the "what changed" fallback).  It is the
    # engine-independent half of the dirty-rect story: a device that
    # cannot report per-frame dirty rectangles can compute them from
    # two consecutive frames.
    src/dirty_rect_diff.cpp
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

    # The AVFoundation camera source is compiled with ARC.  Without it
    # every `alloc` has to be paired with a `release` by hand, and the
    # `dispatch_queue_t` objects have to be released explicitly too --
    # a discipline that is easy to get wrong and impossible to check
    # with the compiler.  With ARC the ownership is described by the
    # strong `__strong` variables and the compiler enforces it.
    set_source_files_properties(
        src/platform/macos/avfoundation_camera.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
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
    #
    # The X11 path has *two* dependencies, not one:
    #
    #   - `libx11`    -- `XGetImage`, for the screen capture itself.
    #   - `libxfixes` -- `XFixesGetCursorImage`, for the cursor source.
    #
    # They ship in two different Debian/Ubuntu packages (`libx11-dev` and
    # `libxfixes-dev`), and a build host may well have only the first.
    # `find_package(X11)` finds `libx11` alone, so it cannot answer the
    # second question -- `pkg-config` can, because the XFixes headers
    # are only installed together with its `.pc` file.
    #
    # The two are therefore detected separately, and each source file is
    # compiled only when its own dependency is present.  The cursor
    # source in particular *includes* `X11/extensions/Xfixes.h`, so
    # compiling it without `libxfixes-dev` is a hard build failure --
    # not a runtime "unavailable" that the stub could paper over.
    find_package(X11 QUIET)
    find_package(PkgConfig QUIET)

    set(_nimrtc_have_x11 FALSE)
    if(X11_FOUND)
        set(_nimrtc_have_x11 TRUE)
        list(APPEND NIMRTC_VIDEO_CAPTURE_SOURCES
            src/platform/linux/x11_capture.cpp
            )
        message(STATUS "video_capture: the X11 capture backend is enabled")
    else()
        message(STATUS
            "video_capture: the X11 capture backend is disabled "
            "(the X11 development headers were not found)")
    endif()

    set(_nimrtc_have_xfixes FALSE)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(NIMRTC_XFIXES QUIET xfixes)
        if(NIMRTC_XFIXES_FOUND)
            set(_nimrtc_have_xfixes TRUE)
        endif()
    endif()
    if(_nimrtc_have_xfixes)
        list(APPEND NIMRTC_VIDEO_CAPTURE_SOURCES
            # The X11 cursor source (remote-desktop plan A6):
            # XFixesGetCursorImage.  It needs the XFixes extension.
            src/platform/linux/x11_cursor.cpp
            )
        message(STATUS "video_capture: the X11 cursor source is enabled")
    elseif(_nimrtc_have_x11)
        message(STATUS
            "video_capture: the X11 cursor source is disabled "
            "(the XFixes development headers were not found)")
    endif()

    if(_nimrtc_have_x11 OR _nimrtc_have_xfixes)
        set(_nimrtc_linux_factory_glue TRUE)
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
    target_link_libraries(nimrtc_video_capture PRIVATE
        "-framework ScreenCaptureKit"
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework CoreGraphics"
        # Foundation.  It is the framework the Objective-C runtime is
        # built on, and the backend uses it directly: the `@[]` array
        # literal in the content filter, and `NSString` for the
        # device's description.
        "-framework Foundation"
        # The Objective-C runtime itself.  The backend defines an
        # Objective-C class (`NimRTCSCKStreamOutput`), and a class
        # definition needs the runtime's metadata tables -- the
        # `objc_empty_cache` hook and the `NSObject` base class among
        # them.  Without it the *compile* succeeds and the *link* fails
        # with undefined `_OBJC_CLASS_$_NSObject` and
        # `_OBJC_METACLASS_$_NimRTCSCKStreamOutput`.
        "-lobjc"
        )
    message(STATUS "  ScreenCaptureKit  : enabled (macOS)")
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

# The X11 *cursor* source is a separate capability from the X11 capture:
# it needs the XFixes extension, which ships in its own package.  The
# source is compiled only when XFixes is present, and the portable stub
# provides the entry point otherwise.
if(UNIX AND NOT APPLE AND _nimrtc_have_xfixes)
    target_include_directories(nimrtc_video_capture SYSTEM PRIVATE
        ${NIMRTC_XFIXES_INCLUDE_DIRS})
    target_link_libraries(nimrtc_video_capture PRIVATE
        ${NIMRTC_XFIXES_LIBRARIES})
    target_compile_definitions(nimrtc_video_capture
        PRIVATE NIMRTC_HAS_X11_CURSOR=1)
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
        # Several SDK versions can be installed side by side, so the
        # `cppwinrt` directory has to be picked from the SDK the
        # *toolchain actually compiles against* -- not from whichever
        # one the glob happens to return first.
        #
        # `file(GLOB)` returns its matches in lexicographic order, so
        # `list(GET ... 0)` picks the OLDEST version, not the newest:
        #
        #   10.0.17763.0   (2018)  <-- what the glob used to select
        #   10.0.22621.0   (2023)
        #   10.0.26100.0   (2024)  <-- what the compiler is really using
        #
        # An old projection is not merely stale, it is *incompatible*:
        # the 2018-era `winrt/base.h` hard-includes `<experimental/coroutine>`
        # unconditionally, and that header `#error`s (C1189) as soon as
        # `__cpp_impl_coroutine` is defined -- which `/std:c++20` does.
        # The build then fails with an error that names `<experimental/coroutine>`
        # and points nowhere near the include path that caused it.
        #
        # So the selection is, in order:
        #   1. the SDK version the compiler is using ($ENV{WindowsSDKVersion},
        #      set by vcvarsall / MSBuild),
        #   2. otherwise the newest SDK version present on disk.
        set(_nimrtc_cppwinrt_dir "")

        # (1) The SDK the toolchain targets.
        if(DEFINED ENV{WindowsSDKVersion})
            # vcvarsall exports this with a trailing backslash
            # ("10.0.26100.0\"), so strip it before turning the dotted
            # version into a directory name.
            string(REGEX REPLACE "[\\\\/]+$" "" _nimrtc_sdk_ver "$ENV{WindowsSDKVersion}")
            string(REPLACE "." "/" _nimrtc_sdk_ver_slash "${_nimrtc_sdk_ver}")
            if(IS_DIRECTORY "${_nimrtc_sdk_root}/Include/${_nimrtc_sdk_ver_slash}/cppwinrt")
                set(_nimrtc_cppwinrt_dir
                    "${_nimrtc_sdk_root}/Include/${_nimrtc_sdk_ver_slash}/cppwinrt")
            endif()
        endif()

        if(_nimrtc_cppwinrt_dir)
            # Sanity: the directory must actually carry the projection.
            # A directory that exists but has no `winrt/base.h` would
            # fail later as a confusing "cannot open include file"
            # rather than as the selection error it actually is.
            if(NOT EXISTS "${_nimrtc_cppwinrt_dir}/winrt/base.h")
                message(WARNING
                    "video_capture: ${_nimrtc_cppwinrt_dir} has no "
                    "winrt/base.h -- falling back to the newest SDK")
                set(_nimrtc_cppwinrt_dir "")
            endif()
        endif()

        if(NOT _nimrtc_cppwinrt_dir)
            file(GLOB _nimrtc_cppwinrt_dirs
                 "${_nimrtc_sdk_root}/Include/*/cppwinrt")
            set(_nimrtc_cppwinrt_newest "")
            foreach(_dir IN LISTS _nimrtc_cppwinrt_dirs)
                if(EXISTS "${_dir}/winrt/base.h")
                    list(APPEND _nimrtc_cppwinrt_newest "${_dir}")
                endif()
            endforeach()
            if(_nimrtc_cppwinrt_newest)
                # Sort descending, then take the first.  Every SDK since
                # 10.0.1xxxx uses a fixed-width four-digit build number,
                # so a natural-order compare is correct for the versions
                # that actually exist.
                list(SORT _nimrtc_cppwinrt_newest
                     COMPARE NATURAL ORDER DESCENDING)
                list(GET _nimrtc_cppwinrt_newest 0 _nimrtc_cppwinrt_dir)
            endif()
        endif()

        if(_nimrtc_cppwinrt_dir)
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
