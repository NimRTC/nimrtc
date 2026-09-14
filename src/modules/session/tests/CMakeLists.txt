# modules/session/tests/CMakeLists.txt
# =============================================================================
# Session module unit tests.
# Built via nimrtc_add_test() defined in cmake/NimRTCTest.cmake.
# =============================================================================
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_session.cpp
    DEPS nimrtc::session)
