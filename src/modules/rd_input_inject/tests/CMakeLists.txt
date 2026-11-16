# modules/rd_input_inject/tests/CMakeLists.txt
# =============================================================================
# Remote-desktop input-injection module unit tests.
#
# The tests cover the *portable* half of the module — the key layout, the
# injection state machine and the release-everything path — because that
# is the half that can be tested on every CI leg.  The platform backends
# need a real desktop session, so they are exercised by the example
# application (examples/remote-desktop/) rather than by a unit test.
# =============================================================================
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_rd_input_inject.cpp
    DEPS nimrtc::rd_input_inject)
