# modules/bwe/tests/CMakeLists.txt
# =============================================================================
# BWE module unit tests.
# =============================================================================
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_bwe.cpp
    DEPS nimrtc::bwe)
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_bwe_plugin.cpp
    DEPS nimrtc::bwe)