# modules/video_sdp_attributes/CMakeLists.txt
# ----------------------------------------------------------------------------
# Video SDP attribute parsers (H.264/VP8/VP9 fmtp).
#
# Per ARCHITECTURE.md §5 + §6: video codecs carry per-stream parameters in
# `a=fmtp:<pt> ...` and codec identification in `a=rtpmap:<pt> <enc>/<clock>`.
# This module provides the strongly-typed codec-specific parser / builder on
# top of the generic SDP module.
#
# P1 minimal:
#   - H.264 (RFC 6184) packetization-mode / profile-level-id / sprop / max-*
#   - VP8  (RFC 7741) max-fr / max-fs
#   - VP9  (RFC 9559) profile-id / max-fr / max-fs / max-br
#
# No vendor libraries; pure C++.  Reuses nimrtc::core for Result/Error and
# nimrtc::video_frame for CodecKind.
# ----------------------------------------------------------------------------
add_library(nimrtc_video_sdp_attributes STATIC
    src/video_sdp_attributes.cpp
    )

nimrtc_apply_options(nimrtc_video_sdp_attributes)
target_link_libraries(nimrtc_video_sdp_attributes
    PUBLIC
        nimrtc::core
        nimrtc::video_frame
    )

target_include_directories(nimrtc_video_sdp_attributes
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_video_sdp_attributes
    PUBLIC NIMRTC_HAS_VIDEO_SDP_ATTRIBUTES=1)

add_library(nimrtc::video_sdp_attributes ALIAS nimrtc_video_sdp_attributes)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
