# modules/rd_input/tests/CMakeLists.txt
# =============================================================================
# Remote-desktop input module unit tests (plan G3 / G4 / G7).
#
# The three acceptance criteria are covered by three test suites in one
# binary:
#
#   - input_event_codec_*   G3: the round trip of every event type, and the
#                            compactness of the encoding
#   - input_coalescer_*     G4: a 1000 Hz pointer stream produces at most one
#                            event per display frame; the key events are
#                            lossless
#   - input_safety_filter_* G7: a denied key never reaches the remote side,
#                            and the denial is logged
# =============================================================================
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_rd_input.cpp
    DEPS nimrtc::rd_input)
