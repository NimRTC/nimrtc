# examples/demo-agent-realtime/CMakeLists.txt
# ----------------------------------------------------------------------------
# demo-agent-realtime — real-time AI game decision processing demo
# (slice v2-ai-decision).  The executable is self-contained (no NimRTC
# linkage), so it needs no msquic DLL / DTLS-cert staging.
# ----------------------------------------------------------------------------

if(NOT NIMRTC_BUILD_EXAMPLES)
    return()
endif()

add_executable(demo-agent-realtime
    demo-agent-realtime.cpp
    ai_decision.hpp)
nimrtc_apply_options(demo-agent-realtime)

# The self-test is the acceptance entry point for slice v2-ai-decision: it
# runs 100 consecutive decisions under the per-decision latency instrument,
# compares 100 decisions against the expert oracle, exercises strategy
# switching, and proves the debug trace — the four acceptance criteria.
# Deterministic and headless; exits non-zero if any criterion fails.
add_test(NAME demo-agent-realtime_self_test
         COMMAND $<TARGET_FILE:demo-agent-realtime>
                 --self-test --decisions 100 --debug)
set_tests_properties(demo-agent-realtime_self_test PROPERTIES TIMEOUT 60)
