# modules/video_jb/CMakeLists.txt
# ----------------------------------------------------------------------------
# Video-aware jitter buffer (RFC 6184 reassembly + frame emission).
#
# P1 minimal:
#   - Per-(SSRC, RTP-ts) frame buckets
#   - Sequence reorder + NACK-loss detection
#   - FU-A fragment reassembly (H.264)
#   - VP8 / VP9 partition concatenation (no descriptor reassembly)
#   - Frame emit on marker bit OR max_wait_ms timeout
#
# No vendor libraries; pure C++.  Reuses nimrtc::core and the video_frame /
# video_payload modules.
# ----------------------------------------------------------------------------
add_library(nimrtc_video_jb STATIC
    src/video_jitter_buffer.cpp
    )

nimrtc_apply_options(nimrtc_video_jb)
target_link_libraries(nimrtc_video_jb
    PUBLIC
        nimrtc::core
        nimrtc::video_frame
        nimrtc::video_payload       # for FU-A reassembly
    )

target_include_directories(nimrtc_video_jb
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_video_jb PUBLIC NIMRTC_HAS_VIDEO_JB=1)

add_library(nimrtc::video_jb ALIAS nimrtc_video_jb)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
