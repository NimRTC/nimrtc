# modules/audio_rx/tests/CMakeLists.txt
# =============================================================================
# Audio receive-pipeline module unit tests.
# Built via nimrtc_add_test() defined in cmake/NimRTCTest.cmake.
# =============================================================================
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_audio_rx.cpp
    DEPS nimrtc::audio_rx)
