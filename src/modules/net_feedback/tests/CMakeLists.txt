# modules/net_feedback/tests/CMakeLists.txt
# =============================================================================
# Network-feedback collector module unit tests.
# Built via nimrtc_add_test() defined in cmake/NimRTCTest.cmake.
# =============================================================================
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_net_feedback.cpp
    DEPS nimrtc::net_feedback nimrtc::rtp)
