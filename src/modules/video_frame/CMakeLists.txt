# modules/video_frame/CMakeLists.txt
# ----------------------------------------------------------------------------
# Video frame types — pixel formats, layouts, refcounted buffers, conversions.
#
# P1 minimal:
#   - VideoFrameLayout computation (per-format strides/offsets)
#   - VideoFrameBuffer (refcounted, zero-copy SFU forwarding primitive)
#   - I420 ↔ NV12 / I420 → BGRA conversions (portable reference)
#
# No vendor libraries; pure C++.
# ----------------------------------------------------------------------------
add_library(nimrtc_video_frame STATIC
    src/frame.cpp
    )

nimrtc_apply_options(nimrtc_video_frame)
target_link_libraries(nimrtc_video_frame
    PUBLIC
        nimrtc::core
    )

target_include_directories(nimrtc_video_frame
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_video_frame PUBLIC NIMRTC_HAS_VIDEO_FRAME=1)

add_library(nimrtc::video_frame ALIAS nimrtc_video_frame)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
