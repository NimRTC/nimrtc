# modules/rd_input/tests/CMakeLists.txt
# =============================================================================
# Remote-desktop input module unit tests (plan G1 / G2 / G3 / G4 / G5 /
# G6 / G7).
#
# The acceptance criteria are covered by four test binaries:
#
#   - test_rd_input.cpp        G3 / G4 / G7: the round trip of every event
#                            type, the compactness of the encoding, the
#                            1000 Hz pointer coalescing, the safety filter
#   - test_rd_input_g5_g6.cpp  G5 / G6: the ref-frame id on the wire and
#                            the binder; the dead-man watchdog's
#                            transitions and the heartbeat emitter
#   - test_control_channel.cpp G1 / G2: the control channel's framing, the
#                            reassembly across transport records, the
#                            malformed-frame rejection and the counters
# =============================================================================
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_rd_input.cpp
    DEPS nimrtc::rd_input)

nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_rd_input_g5_g6.cpp
    DEPS nimrtc::rd_input)

# Plan G1 / G2: the NimRTC-native control channel — the framing, the
# reassembly and the malformed-frame rejection.
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_control_channel.cpp
    DEPS nimrtc::rd_input)
