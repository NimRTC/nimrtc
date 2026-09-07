# modules/sched/tests/CMakeLists.txt
# =============================================================================
# Scheduler module unit tests.
# Built via nimrtc_add_test() defined in cmake/NimRTCTest.cmake.
# =============================================================================
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_scheduler.cpp
    DEPS nimrtc::sched)
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_scheduler_plugin.cpp
    DEPS nimrtc::sched)
