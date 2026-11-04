# modules/jb_strategy/tests/CMakeLists.txt
# =============================================================================
# Shared JB strategy module tests.
# =============================================================================
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_adaptive_delay.cpp
    DEPS nimrtc::jb_strategy)
