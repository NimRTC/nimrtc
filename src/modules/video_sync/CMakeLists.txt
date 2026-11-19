# modules/video_sync/CMakeLists.txt
# ----------------------------------------------------------------------------
# The render clock — the video half of the A/V-sync correction
# (v1.3 plan Q6 / remote-desktop plan R3).
#
# Scope:
#   - the tolerance band and the present / drop / delay mapping
#   - the bounded-debt bookkeeping (an unbounded hold is a latency bomb)
#   - the switch that turns the correction off (Q6-1)
#
# The skew the clock consumes is the one the ADR-017 timeline already
# produces -- the module does NOT derive a skew of its own.  That is the
# plan's hardest constraint ("no second skew model"), and it is why the
# skew is a *parameter* of `decide()` rather than something the clock
# computes.
#
# Why a module and not a field in the engine:
#   The policy is a pure function of the skew: no clock, no allocation,
#   no thread, no GPU, no window-system type.  That makes it unit-tested
#   on every platform -- including the CI legs that have no display.
#
#   The engine may not include a concrete module's header (Layout
#   Invariant 4 / ADR-025), so the module also ships a thin adapter
#   (`src/factory.cpp`) that implements the `plugins::IVideoSyncController`
#   seam from src/plugins/.  The adapter adds no policy: it is a
#   translation layer, so there is exactly one implementation of the
#   correction policy in the tree.
#
# Dependencies:
#   - core     (always) -- the log / error vocabulary
#   - plugins  (the IVideoSyncController seam the engine drives)
# ----------------------------------------------------------------------------
add_library(nimrtc_video_sync STATIC
    src/render_clock.cpp
    src/factory.cpp
    )

nimrtc_apply_options(nimrtc_video_sync)
target_link_libraries(nimrtc_video_sync
    PUBLIC
        nimrtc::core
        nimrtc::plugins
    )

target_include_directories(nimrtc_video_sync
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_video_sync PUBLIC NIMRTC_HAS_VIDEO_SYNC=1)

add_library(nimrtc::video_sync ALIAS nimrtc_video_sync)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
