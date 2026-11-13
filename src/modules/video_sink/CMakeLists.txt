# modules/video_sink/CMakeLists.txt
# ----------------------------------------------------------------------------
# Decoded-video sinks: headless (drops pixels), pinned (asserts capture_ts
# ordering) and the Windows D3D11 / DXGI swap-chain renderer.
#
# The renderer is the Windows leg of the remote-desktop render track
# (plan E2): `Present(0)` plus a presentation clock, a latest-wins
# single-slot mailbox, and the drop counter that proves the
# back-pressure policy works (plan E8).
#
# Dependencies:
#   - core        : Status / Logger
#   - plugins     : plugins::IVideoSink interface
#   - video_frame : the pixel-format helpers the renderer's CPU path uses
#                   (I420 -> BGRA conversion, layout computation)
# ----------------------------------------------------------------------------
add_library(nimrtc_video_sink STATIC
    src/video_sink.cpp
    src/d3d11_renderer.cpp
    )

nimrtc_apply_options(nimrtc_video_sink)
target_link_libraries(nimrtc_video_sink
    PUBLIC
        nimrtc::core
        nimrtc::plugins
    PRIVATE
        # The renderer's CPU path converts a decoded I420 picture into the
        # present target's BGRA format, so it needs the video_frame
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

# The Windows renderer needs the Direct3D 11 / DXGI import libraries.
# On a non-Windows build the renderer compiles to the "unsupported" stub
# (see d3d11_renderer.cpp), so nothing is linked.
if(WIN32)
    target_link_libraries(nimrtc_video_sink PRIVATE d3d11 dxgi)
endif()

add_library(nimrtc::video_sink ALIAS nimrtc_video_sink)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
