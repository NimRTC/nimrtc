# modules/video_sink/CMakeLists.txt
# ----------------------------------------------------------------------------
# Decoded-video sinks: headless (drops pixels), pinned (asserts capture_ts
# ordering) and the presentation renderers.
#
# The renderers are the remote-desktop render track (plan E2 / E3 / E4):
# the Windows D3D11 / DXGI swap chain, the macOS Metal /
# AVSampleBufferDisplayLayer layer and the Linux EGL / Wayland / X11
# surface.  All three share the **portable** presenter core -- the
# latest-wins single-slot mailbox, the presentation clock and the drop
# counter that proves the back-pressure policy works (plan E8).
#
# Dependencies:
#   - core        : Status / Logger
#   - plugins     : plugins::IVideoSink interface
#   - video_frame : the pixel-format helpers the renderers' CPU paths use
#                   (I420 -> BGRA conversion, layout computation)
# ----------------------------------------------------------------------------
add_library(nimrtc_video_sink STATIC
    src/video_sink.cpp
    src/displayed_frame.cpp
    src/presenter_core.cpp
    src/d3d11_renderer.cpp
    src/metal_renderer.cpp
    src/egl_renderer.cpp
    src/video_sync.cpp
    )

nimrtc_apply_options(nimrtc_video_sink)
target_link_libraries(nimrtc_video_sink
    PUBLIC
        nimrtc::core
        nimrtc::plugins
    PRIVATE
        # The renderers' CPU paths convert a decoded I420 picture into the
        # present target's BGRA format, so they need the video_frame
        # conversion helpers.  (The include path already arrives through
        # `nimrtc::plugins`, which re-exports the video_frame types.)
        nimrtc::video_frame
    )

target_include_directories(nimrtc_video_sink
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_video_sink
    PUBLIC NIMRTC_HAS_VIDEO_SINK=1)

# ----------------------------------------------------------------------------
# The Windows renderer needs the Direct3D 11 / DXGI import libraries.
# On a non-Windows build the renderer compiles to the "unsupported" stub
# (see d3d11_renderer.cpp), so nothing is linked.
# ----------------------------------------------------------------------------
if(WIN32)
    target_link_libraries(nimrtc_video_sink PRIVATE d3d11 dxgi)
endif()

# ----------------------------------------------------------------------------
# The macOS renderer (plan E3): Metal + QuartzCore + AVFoundation.
#
# The renderer source is compiled on every platform (the class must exist
# everywhere so a configuration that names it resolves -- on a non-Apple
# build `open()` reports `kErrUnsupported`), but the *implementation* is
# guarded by `#if defined(__APPLE__)`.
#
# The implementation needs Objective-C++ (`id<MTLDevice>`, `CAMetalLayer`,
# `CMSampleBuffer`, ...), so the translation unit is compiled with the
# Objective-C++ dialect and ARC on Apple platforms.
# ----------------------------------------------------------------------------
if(APPLE)
    set_source_files_properties(src/metal_renderer.cpp PROPERTIES
        COMPILE_OPTIONS "-x;objective-c++;-fobjc-arc")
    target_link_libraries(nimrtc_video_sink PRIVATE
        "-framework;Metal"
        "-framework;QuartzCore"
        "-framework;AVFoundation"
        "-framework;CoreMedia"
        "-framework;CoreVideo"
        "-framework;Foundation"
        "-framework;AppKit")
endif()

# ----------------------------------------------------------------------------
# The Linux renderer (plan E4): EGL / GLES2 + Wayland + X11.
#
# EVERY dependency here is **optional**.  A missing library never breaks
# the build: the corresponding code path is simply not compiled, and
# `open()` reports `kErrUnsupported` when no usable path is left.  This
# is what lets the same source tree build on a headless CI container
# with neither Wayland nor X11 installed.
# ----------------------------------------------------------------------------
if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)

    set(_nimrtc_video_sink_egl_defs "")
    set(_nimrtc_video_sink_egl_libs "")

    if(PKG_CONFIG_FOUND)
        pkg_check_modules(PC_EGL QUIET egl)
        pkg_check_modules(PC_GLESV2 QUIET glesv2)
        pkg_check_modules(PC_WAYLAND QUIET wayland-client)
        pkg_check_modules(PC_WAYLAND_EGL QUIET wayland-egl)
        pkg_check_modules(PC_X11 QUIET x11)
        pkg_check_modules(PC_XCB_SHM QUIET xcb-shm)

        if(PC_EGL_FOUND AND PC_GLESV2_FOUND)
            list(APPEND _nimrtc_video_sink_egl_defs
                NIMRTC_VIDEO_SINK_HAS_EGL=1)
            list(APPEND _nimrtc_video_sink_egl_libs
                ${PC_EGL_LIBRARIES} ${PC_GLESV2_LIBRARIES})
            list(APPEND _nimrtc_video_sink_egl_include_dirs
                ${PC_EGL_INCLUDE_DIRS} ${PC_GLESV2_INCLUDE_DIRS})
        endif()

        if(PC_WAYLAND_FOUND)
            list(APPEND _nimrtc_video_sink_egl_defs
                NIMRTC_VIDEO_SINK_HAS_WAYLAND=1)
            list(APPEND _nimrtc_video_sink_egl_libs
                ${PC_WAYLAND_LIBRARIES})
            list(APPEND _nimrtc_video_sink_egl_include_dirs
                ${PC_WAYLAND_INCLUDE_DIRS})
        endif()

        if(PC_WAYLAND_EGL_FOUND)
            list(APPEND _nimrtc_video_sink_egl_libs
                ${PC_WAYLAND_EGL_LIBRARIES})
            list(APPEND _nimrtc_video_sink_egl_include_dirs
                ${PC_WAYLAND_EGL_INCLUDE_DIRS})
        endif()

        if(PC_X11_FOUND)
            list(APPEND _nimrtc_video_sink_egl_defs
                NIMRTC_VIDEO_SINK_HAS_X11=1)
            list(APPEND _nimrtc_video_sink_egl_libs
                ${PC_X11_LIBRARIES})
            list(APPEND _nimrtc_video_sink_egl_include_dirs
                ${PC_X11_INCLUDE_DIRS})
        endif()

        if(PC_XCB_SHM_FOUND)
            list(APPEND _nimrtc_video_sink_egl_defs
                NIMRTC_VIDEO_SINK_HAS_XCB_SHM=1)
            list(APPEND _nimrtc_video_sink_egl_libs
                ${PC_XCB_SHM_LIBRARIES})
            list(APPEND _nimrtc_video_sink_egl_include_dirs
                ${PC_XCB_SHM_INCLUDE_DIRS})
        endif()
    endif()

    # The Wayland protocol extensions.  `xdg-shell` provides the
    # toplevel surface; `linux-dmabuf` provides the zero-copy route.
    # Both are optional headers -- they ship with `wayland-protocols`.
    find_path(NIMRTC_XDG_SHELL_PROTOCOL_DIR
        NAMES xdg-shell-client-protocol.h
        PATH_SUFFIXES wayland-protocols)
    if(NIMRTC_XDG_SHELL_PROTOCOL_DIR)
        list(APPEND _nimrtc_video_sink_egl_defs
            NIMRTC_VIDEO_SINK_HAS_XDG_SHELL=1)
        list(APPEND _nimrtc_video_sink_egl_include_dirs
            ${NIMRTC_XDG_SHELL_PROTOCOL_DIR})
    endif()

    find_path(NIMRTC_LINUX_DMABUF_PROTOCOL_DIR
        NAMES linux-dmabuf-v1-client-protocol.h
        PATH_SUFFIXES wayland-protocols)
    if(NIMRTC_LINUX_DMABUF_PROTOCOL_DIR)
        list(APPEND _nimrtc_video_sink_egl_defs
            NIMRTC_VIDEO_SINK_HAS_DMABUF=1)
        list(APPEND _nimrtc_video_sink_egl_include_dirs
            ${NIMRTC_LINUX_DMABUF_PROTOCOL_DIR})
    endif()

    if(_nimrtc_video_sink_egl_defs)
        target_compile_definitions(nimrtc_video_sink PRIVATE
            ${_nimrtc_video_sink_egl_defs})
    endif()
    if(_nimrtc_video_sink_egl_libs)
        target_link_libraries(nimrtc_video_sink PRIVATE
            ${_nimrtc_video_sink_egl_libs})
    endif()
    if(_nimrtc_video_sink_egl_include_dirs)
        target_include_directories(nimrtc_video_sink PRIVATE
            ${_nimrtc_video_sink_egl_include_dirs})
    endif()

    message(STATUS "  video_sink: EGL=${_nimrtc_video_sink_egl_defs}")
endif()

add_library(nimrtc::video_sink ALIAS nimrtc_video_sink)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
