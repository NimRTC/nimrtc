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
    # The frameworks are named with the *single-string* spelling
    # ("-framework QuartzCore"), not the two-item list form
    # ("-framework" "QuartzCore").
    #
    # The two-item form breaks down silently: CMake de-duplicates the
    # link items by name, so the repeated bare `-framework` entry
    # survives only once and every framework after it loses its prefix
    # -- the linker then looks for a *library* called "QuartzCore"
    # ("ld: library 'QuartzCore' not found") instead of the framework.
    # The single-string form keeps each framework paired with its
    # prefix, so nothing is de-duplicated away.
    target_link_libraries(nimrtc_video_sink PRIVATE
        "-framework Metal"
        "-framework QuartzCore"
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework Foundation"
        "-framework AppKit")
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
    #
    # The `wayland-protocols` package ships the **XML**, not the C
    # headers: `xdg-shell-client-protocol.h` is a *generated* artefact
    # (`wayland-scanner client-header`).  Looking for the header with
    # `find_path` therefore never succeeds -- the two macros below were
    # undefined on every build host, and the ~20 `HAS_XDG_SHELL` /
    # `HAS_DMABUF` branches in `egl_renderer.cpp` were never compiled.
    # The generation step below is what makes them reachable.
    find_program(NIMRTC_WAYLAND_SCANNER
        NAMES wayland-scanner
        DOC "The Wayland protocol header generator")

    # The protocol XML is located through `pkg-config` first (that is
    # where `wayland-protocols` publishes its `pkgdatadir`) and only
    # then through the conventional install locations.
    pkg_check_modules(PC_WAYLAND_PROTOCOLS QUIET wayland-protocols)
    set(_nimrtc_wayland_protocols_root "")
    if(PC_WAYLAND_PROTOCOLS_FOUND)
        set(_nimrtc_wayland_protocols_root
            "${PC_WAYLAND_PROTOCOLS_PKGDATADIR}")
        string(REGEX REPLACE "//+" "/" _nimrtc_wayland_protocols_root
               "${_nimrtc_wayland_protocols_root}")
    endif()

    set(_nimrtc_wayland_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/wayland-protocols")
    set(_nimrtc_wayland_generated "")

    # Generates one protocol: the XML in, the client header *and* the
    # marshalling code out.
    #
    #   _nimrtc_generate_wayland_protocol(<xml> <header-name> <code-name>)
    #
    # The scanner produces **two** artefacts per protocol:
    #
    #   - `client-header` — the C declarations (the inline wrappers and
    #     the `*_interface` symbols),
    #   - `private-code` — a `.c` file with the *marshalling* code that
    #     implements those symbols.
    #
    # The header alone is not enough: `xdg_surface_interface` and
    # friends are declared in the header but defined in the code file,
    # so a build that only generates the header fails at link time with
    # "undefined reference to `xdg_surface_interface`".
    #
    # The output file names are chosen here rather than derived from the
    # protocol's own name, because the protocol name changed across
    # `wayland-protocols` releases (`linux_dmabuf_unstable_v1` became
    # `linux_dmabuf_v1`).  The *interface* names stayed the same, so
    # the C code that includes the header is unaffected.
    function(_nimrtc_generate_wayland_protocol xml_file header_name code_name)
        if(NOT NIMRTC_WAYLAND_SCANNER OR NOT EXISTS "${xml_file}")
            return()
        endif()

        set(_out_h "${_nimrtc_wayland_gen_dir}/${header_name}")
        set(_out_c "${_nimrtc_wayland_gen_dir}/${code_name}")
        file(MAKE_DIRECTORY "${_nimrtc_wayland_gen_dir}")

        add_custom_command(
            OUTPUT "${_out_h}"
            COMMAND "${NIMRTC_WAYLAND_SCANNER}" client-header
                    "${xml_file}" "${_out_h}"
            DEPENDS "${xml_file}"
            COMMENT "Generating ${header_name}"
            VERBATIM)

        add_custom_command(
            OUTPUT "${_out_c}"
            COMMAND "${NIMRTC_WAYLAND_SCANNER}" private-code
                    "${xml_file}" "${_out_c}"
            DEPENDS "${xml_file}"
            COMMENT "Generating ${code_name}"
            VERBATIM)

        set(_nimrtc_wayland_generated
            "${_nimrtc_wayland_generated}" "${_out_h}" "${_out_c}"
            PARENT_SCOPE)
        set(_nimrtc_wayland_sources
            "${_nimrtc_wayland_sources}" "${_out_c}" PARENT_SCOPE)
    endfunction()

    set(_nimrtc_wayland_sources "")

    # ---- `xdg-shell`: the toplevel surface ------------------------------
    #
    # It lives in `stable/` since wayland-protocols 1.20.
    set(_nimrtc_xdg_shell_xml "")
    if(_nimrtc_wayland_protocols_root)
        if(EXISTS
           "${_nimrtc_wayland_protocols_root}/stable/xdg-shell/xdg-shell.xml")
            set(_nimrtc_xdg_shell_xml
                "${_nimrtc_wayland_protocols_root}/stable/xdg-shell/xdg-shell.xml")
        endif()
    endif()
    if(NOT _nimrtc_xdg_shell_xml)
        find_path(NIMRTC_XDG_SHELL_XML
            NAMES xdg-shell.xml
            HINTS /usr/share /usr/local/share
            PATH_SUFFIXES share/wayland-protocols/stable/xdg-shell
                        wayland-protocols/stable/xdg-shell)
        if(NIMRTC_XDG_SHELL_XML)
            set(_nimrtc_xdg_shell_xml "${NIMRTC_XDG_SHELL_XML}/xdg-shell.xml")
        endif()
    endif()
    if(_nimrtc_xdg_shell_xml)
        _nimrtc_generate_wayland_protocol(
            "${_nimrtc_xdg_shell_xml}"
            "xdg-shell-client-protocol.h"
            "xdg-shell-protocol.c")
        list(APPEND _nimrtc_video_sink_egl_defs
            NIMRTC_VIDEO_SINK_HAS_XDG_SHELL=1)
        list(APPEND _nimrtc_video_sink_egl_include_dirs
            "${_nimrtc_wayland_gen_dir}")
    endif()

    # ---- `linux-dmabuf`: the zero-copy route ----------------------------
    #
    # It is an **unstable** protocol (the interface is
    # `zwp_linux_dmabuf_v1`), so it lives in `unstable/`.  The generated
    # header is renamed to `linux-dmabuf-v1-client-protocol.h` so the
    # include in the renderer does not carry the `-unstable-v1` infix.
    set(_nimrtc_linux_dmabuf_xml "")
    if(_nimrtc_wayland_protocols_root)
        if(EXISTS
           "${_nimrtc_wayland_protocols_root}/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml")
            set(_nimrtc_linux_dmabuf_xml
                "${_nimrtc_wayland_protocols_root}/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml")
        endif()
    endif()
    if(NOT _nimrtc_linux_dmabuf_xml)
        find_path(NIMRTC_LINUX_DMABUF_XML
            NAMES linux-dmabuf-unstable-v1.xml
            HINTS /usr/share /usr/local/share
            PATH_SUFFIXES share/wayland-protocols/unstable/linux-dmabuf
                        wayland-protocols/unstable/linux-dmabuf)
        if(NIMRTC_LINUX_DMABUF_XML)
            set(_nimrtc_linux_dmabuf_xml
                "${NIMRTC_LINUX_DMABUF_XML}/linux-dmabuf-unstable-v1.xml")
        endif()
    endif()
    if(_nimrtc_linux_dmabuf_xml)
        _nimrtc_generate_wayland_protocol(
            "${_nimrtc_linux_dmabuf_xml}"
            "linux-dmabuf-v1-client-protocol.h"
            "linux-dmabuf-v1-protocol.c")
        list(APPEND _nimrtc_video_sink_egl_defs
            NIMRTC_VIDEO_SINK_HAS_DMABUF=1)
        list(APPEND _nimrtc_video_sink_egl_include_dirs
            "${_nimrtc_wayland_gen_dir}")
    endif()

    if(_nimrtc_wayland_generated)
        # The generated headers are produced by a build step, so the
        # translation unit that includes them must wait for it.
        set_source_files_properties(src/egl_renderer.cpp
            PROPERTIES OBJECT_DEPENDS "${_nimrtc_wayland_generated}")
    endif()

    if(_nimrtc_wayland_sources)
        # The marshalling code the scanner generates for each protocol.
        #
        # It is plain C, so it goes into its own OBJECT library instead
        # of straight into `nimrtc_video_sink`: the library's warning
        # set is C++-oriented (`-Wnon-virtual-dtor`, `-Wold-style-cast`,
        # `-Woverloaded-virtual`), and GCC *rejects* those options for a
        # C translation unit -- with `-Werror` that rejection is fatal.
        # A separate target keeps the generated code out of the C++
        # warning set entirely.
        add_library(nimrtc_video_sink_wayland_protocols OBJECT
            ${_nimrtc_wayland_sources})
        target_include_directories(nimrtc_video_sink_wayland_protocols
            PRIVATE "${_nimrtc_wayland_gen_dir}")
        if(PC_WAYLAND_INCLUDE_DIRS)
            target_include_directories(nimrtc_video_sink_wayland_protocols
                PRIVATE ${PC_WAYLAND_INCLUDE_DIRS})
        endif()

        # The objects are folded into the sink's archive.
        target_sources(nimrtc_video_sink PRIVATE
            $<TARGET_OBJECTS:nimrtc_video_sink_wayland_protocols>)
        add_dependencies(nimrtc_video_sink
            nimrtc_video_sink_wayland_protocols)
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
