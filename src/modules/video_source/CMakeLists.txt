# modules/video_source/CMakeLists.txt
# ----------------------------------------------------------------------------
# In-memory video frame source for testing / demo interop.
#
# Per ARCHITECTURE.md §5 ("in-memory frame source for demo/interop"):
# Pure C++ source producing deterministic test patterns (solid, color bars,
# gradient, frame counter) at a configurable cadence.  No OS / device
# dependency; no codec integration.
# ----------------------------------------------------------------------------
add_library(nimrtc_video_source STATIC
    src/video_source.cpp
    src/video_source_plugin.cpp     # adapter wrapping concrete → plugins::IVideoSource
    )

nimrtc_apply_options(nimrtc_video_source)
target_link_libraries(nimrtc_video_source
    PUBLIC
        nimrtc::core
        nimrtc::plugins
        nimrtc::video_frame
    )

target_include_directories(nimrtc_video_source
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_video_source
    PUBLIC NIMRTC_HAS_VIDEO_SOURCE=1)

add_library(nimrtc::video_source ALIAS nimrtc_video_source)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
