# modules/video_bitrate_control/CMakeLists.txt
# ----------------------------------------------------------------------------
# The video rate controller (remote-desktop plan D1 / D2 / D3 / D4;
# video plan B1 / B2 / B3 / B4 / B9).
#
# Scope:
#   - D1: the plugin seam (in src/plugins/) and the module that
#          implements it
#   - D2: the resolution floor (the screen-content legibility floor)
#   - D3: the keyframe-cost budget (a geometry step costs an IDR)
#   - D4: the ladder mode is configuration, not a hard-coded constant
#
# Why a module and not engine-internal code:
#   The ladder is the piece of the video path a host most often wants
#   to replace: a screen-content product has a different notion of
#   "good enough" than a camera call.  A module keeps that replaceable
#   through the registry, and keeps the engine free of the policy.
#
# Dependencies:
#   - core     (always) — the byte / error / log vocabulary
#   - plugins  (the seam this module implements)
#
# Per Layout Invariant 3 (AGENTS.md): the module is self-contained —
# own CMakeLists.txt, include/, src/, tests/.
# ----------------------------------------------------------------------------
add_library(nimrtc_video_bitrate_control STATIC
    src/controller.cpp
    src/factory.cpp
    src/ladder.cpp
    )

nimrtc_apply_options(nimrtc_video_bitrate_control)
target_link_libraries(nimrtc_video_bitrate_control
    PUBLIC
        nimrtc::core
        nimrtc::plugins
    )

target_include_directories(nimrtc_video_bitrate_control
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_video_bitrate_control
    PUBLIC NIMRTC_HAS_VIDEO_BITRATE_CONTROL=1)

add_library(nimrtc::video_bitrate_control ALIAS nimrtc_video_bitrate_control)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
