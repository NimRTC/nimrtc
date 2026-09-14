# modules/video_payload/CMakeLists.txt
# ----------------------------------------------------------------------------
# Video RTP payload packetizers / depacketizers (RFC 6184 / 7741 / 9559).
#
# P1 minimal:
#   - H.264 Single NAL, STAP-A, FU-A packetizers/depacketizers
#   - VP8 descriptor (RFC 7741, base + I-bit extension)
#   - VP9 descriptor (RFC 9559, Z/Y/F + picture id + layer info)
#
# No vendor libraries; pure C++.  Reuses nimrtc::core for ByteSpan / Result.
# ----------------------------------------------------------------------------
add_library(nimrtc_video_payload STATIC
    src/h264.cpp
    src/vp8.cpp
    src/vp9.cpp
    )

nimrtc_apply_options(nimrtc_video_payload)
target_link_libraries(nimrtc_video_payload
    PUBLIC
        nimrtc::core
    )

target_include_directories(nimrtc_video_payload
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_video_payload PUBLIC NIMRTC_HAS_VIDEO_PAYLOAD=1)

add_library(nimrtc::video_payload ALIAS nimrtc_video_payload)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
