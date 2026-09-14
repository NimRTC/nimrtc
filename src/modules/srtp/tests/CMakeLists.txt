# modules/srtp/tests/CMakeLists.txt
# =============================================================================
# SRTP module unit tests.
# =============================================================================
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_srtp.cpp
    DEPS nimrtc::srtp)