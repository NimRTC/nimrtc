# modules/video_sink/CMakeLists.txt
# ----------------------------------------------------------------------------
# Decoded-video sinks: headless (drops pixels) and pinned (asserts capture_ts
# ordering). Real renderer plugins (SDL, Vulkan, OpenGL, DirectX, Metal)
# would ship in separate modules implementing the same plugins::IVideoSink
# interface.
#
# Dependencies:
#   - core        : Status / Logger
#   - plugins     : plugins::IVideoSink interface
# ----------------------------------------------------------------------------
add_library(nimrtc_video_sink STATIC
    src/video_sink.cpp
    )

nimrtc_apply_options(nimrtc_video_sink)
target_link_libraries(nimrtc_video_sink
    PUBLIC
        nimrtc::core
        nimrtc::plugins
    )

target_include_directories(nimrtc_video_sink
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_video_sink
    PUBLIC NIMRTC_HAS_VIDEO_SINK=1)

add_library(nimrtc::video_sink ALIAS nimrtc_video_sink)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
