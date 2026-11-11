# modules/pacing/tests/CMakeLists.txt
# =============================================================================
# Token-bucket pacer unit tests.
# Built via nimrtc_add_test() defined in cmake/NimRTCTest.cmake.
# =============================================================================
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_pacing.cpp
    DEPS nimrtc::pacing)
