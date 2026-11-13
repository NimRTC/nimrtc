# modules/video_frame/CMakeLists.txt
# ----------------------------------------------------------------------------
# Video frame types — pixel formats, layouts, refcounted buffers, conversions.
#
# P1 minimal:
#   - VideoFrameLayout computation (per-format strides/offsets)
#   - VideoFrameBuffer (refcounted, zero-copy SFU forwarding primitive)
#   - I420 ↔ NV12 / I420 → BGRA conversions (portable reference)
#
# Remote-desktop plan C2 (Track C "Pixel"):
#   - VideoFrameScaler: SIMD I420 scaling + BGRA ↔ I420 conversion,
#     backed by the vendored libyuv (see src/third_party/libyuv/).
#     The module still compiles without libyuv: the scaler then reports
#     available() == false and every call returns a clean error.
# ----------------------------------------------------------------------------
add_library(nimrtc_video_frame STATIC
    src/frame.cpp
    src/scaler.cpp
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

# Remote-desktop plan C1/C2 — the vendored libyuv.  The helper is a no-op
# when the submodule is absent or NIMRTC_VENDORED_LIBYUV=OFF: the module
# then compiles its "unavailable" path and the scaler reports
# available() == false instead of failing the build.
nimrtc_link_vendor_libyuv(nimrtc_video_frame)

add_library(nimrtc::video_frame ALIAS nimrtc_video_frame)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
