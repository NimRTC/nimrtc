# modules/pacing/CMakeLists.txt
# ----------------------------------------------------------------------------
# The token-bucket pacer (v1.2.0 M3, video plan P1).
#
# Scope:
#   - One reusable token bucket on a monotonic clock
#   - The rate-shaping primitive of the send path (the scheduler owns one
#     bucket per traffic class -- see modules/sched)
#
# Dependencies:
#   - core (the TimePoint type)
#
# The module has no plugin seam of its own: the pacer is a value object
# that the scheduler owns under its own lock.  It is a *module* (not a
# plugin) because nothing outside the send path needs to replace it.
# ----------------------------------------------------------------------------
add_library(nimrtc_pacing STATIC
    src/token_bucket.cpp
    )

nimrtc_apply_options(nimrtc_pacing)
target_link_libraries(nimrtc_pacing
    PUBLIC
        nimrtc::core      # core::TimePoint
    PRIVATE
    )

target_include_directories(nimrtc_pacing
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_pacing PUBLIC NIMRTC_HAS_PACING=1)

add_library(nimrtc::pacing ALIAS nimrtc_pacing)

if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    # ensure a CTestTestfile.cmake gets generated at this level so ctest
    # can recurse into the tests/ subdir from src/modules/ (CMake 4.0
    # VS generator quirk -- same pattern as modules/net_feedback).
    enable_testing()
    add_subdirectory(tests)
endif()
