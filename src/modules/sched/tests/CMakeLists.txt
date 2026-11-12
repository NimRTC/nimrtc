# modules/sched/tests/CMakeLists.txt
# =============================================================================
# Scheduler module unit tests.
# Built via nimrtc_add_test() defined in cmake/NimRTCTest.cmake.
# =============================================================================
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_scheduler.cpp
    DEPS nimrtc::sched)
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_scheduler_plugin.cpp
    DEPS nimrtc::sched)
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_priority.cpp
    DEPS nimrtc::sched)
# v1.2.0 M3 (P2): the per-class pacing integration (the token buckets the
# scheduler owns).  The token bucket itself is tested in modules/pacing.
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_pacing_integration.cpp
    DEPS nimrtc::sched)
# DC-2: P-family + S-family cross-plane priority integration (DataChannel × RTP)
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_sched_priority_dc.cpp
    DEPS nimrtc::sched)
# Architecture remediation plan A8 / A9: the lock-free drain dispatch and
# the S-family flow control (shared BWE credit + per-class pacing).
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_scheduler_flow_control.cpp
    DEPS nimrtc::sched)
