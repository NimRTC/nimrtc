# modules/dtls/tests/CMakeLists.txt
# =============================================================================
# DTLS module unit tests.
# =============================================================================
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_dtls.cpp
    DEPS nimrtc::dtls)