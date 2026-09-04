# modules/rtp/tests/CMakeLists.txt
# =============================================================================
# RTP module unit tests.
# Built via nimrtc_add_test() defined in cmake/NimRTCTest.cmake.
# =============================================================================
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_rtp.cpp
    DEPS nimrtc::rtp)
