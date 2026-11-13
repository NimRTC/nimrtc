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

# -----------------------------------------------------------------------------
# The C ABI, exercised from C.
#
# This one is NOT built with nimrtc_add_test(): that helper builds a
# GoogleTest executable, and a GoogleTest executable is C++.  The point of
# this test is that it is compiled as **C11**, so that the C ABI header is
# proven to be includable — and the symbols linkable — from a C
# translation unit.  A C++ test could not prove either.
#
# It therefore does not use GoogleTest; it is a plain main() that returns
# non-zero on the first failed check.
# -----------------------------------------------------------------------------
add_executable(test_sched_c_abi ${CMAKE_CURRENT_SOURCE_DIR}/test_sched_c_abi.c)
target_link_libraries(test_sched_c_abi PRIVATE nimrtc::sched)
target_include_directories(test_sched_c_abi PRIVATE
    "${CMAKE_SOURCE_DIR}/src/modules/sched/include")
set_target_properties(test_sched_c_abi PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)
nimrtc_apply_options(test_sched_c_abi LENIENT)
add_test(NAME modules_sched_test_sched_c_abi
    COMMAND $<TARGET_FILE:test_sched_c_abi>)
set_tests_properties(modules_sched_test_sched_c_abi PROPERTIES TIMEOUT 30)
