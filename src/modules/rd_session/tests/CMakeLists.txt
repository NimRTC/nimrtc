# modules/rd_session/tests/CMakeLists.txt
# =============================================================================
# The remote-desktop session-guard module's unit tests.
#
# The tests cover the *portable* half of the module: the three-state
# contract, the operator-facing wording and the never-silent failure
# of the recovery.  The Windows backend's behaviour against a locked
# workstation is verified by the device example's own diagnostics (and
# by the LAN runbook), because locking a session interrupts whatever
# is running in it.
# =============================================================================
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_session_guard.cpp
    DEPS nimrtc::rd_session)
