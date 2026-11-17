# examples/demo-agent-realtime/CMakeLists.txt
# ----------------------------------------------------------------------------
# demo-agent-realtime — real-time AI game decision + voice-agent demo.
#
# Slice v2-ai-decision: the executable is self-contained (no NimRTC
# linkage), so it needs no msquic DLL / DTLS-cert staging.
# Slice v4-voice-agent: the voice chain links nimrtc::opus for the Opus TTS
# return path (OpusTts → Encoder::encode_int16); the ASR/LLM backends stay
# runtime-pluggable (no compile-time model SDK).
# ----------------------------------------------------------------------------

if(NOT NIMRTC_BUILD_EXAMPLES)
    return()
endif()

add_executable(demo-agent-realtime
    demo-agent-realtime.cpp
    ai_decision.hpp
    voice_agent.hpp)
nimrtc_apply_options(demo-agent-realtime)

# The voice chain's TTS return path encodes int16 PCM with the vendored
# libopus wrapper (nimrtc::opus).  The ASR/LLM model backends are *not*
# linked — they are runtime-pluggable by design (spec requirement 6).
target_link_libraries(demo-agent-realtime PRIVATE nimrtc::opus)

# The self-test is the acceptance entry point for slice v2-ai-decision: it
# runs 100 consecutive decisions under the per-decision latency instrument,
# compares 100 decisions against the expert oracle, exercises strategy
# switching, and proves the debug trace — the four acceptance criteria.
# Deterministic and headless; exits non-zero if any criterion fails.
add_test(NAME demo-agent-realtime_self_test
         COMMAND $<TARGET_FILE:demo-agent-realtime>
                 --self-test --decisions 100 --debug)
set_tests_properties(demo-agent-realtime_self_test PROPERTIES TIMEOUT 60)

# The voice self-test is the acceptance entry point for slice v4-voice-agent:
# post-3A int16 tap batching (100 ms), streaming LLM consumption, Opus TTS,
# barge-in → stop-playback latency, backend selection and the missing-model
# error policy.  Deterministic and headless; exits non-zero on any failure.
add_test(NAME demo-agent-realtime_voice_self_test
         COMMAND $<TARGET_FILE:demo-agent-realtime>
                 --voice-self-test --frames 300 --barge-in-at 150)
set_tests_properties(demo-agent-realtime_voice_self_test PROPERTIES TIMEOUT 60)

# demo-agent-teammate — the v4-ai-teammate demo: the M1 mock-rule AI decision
# path is replaced by a genuinely local small model (a tiny policy network)
# confined to the narrow action set move_left / move_right / stop.  Also
# self-contained (no NimRTC linkage); registers its own self-test ctest case.
add_executable(demo-agent-teammate
    demo-agent-teammate.cpp
    ai_teammate.hpp)
nimrtc_apply_options(demo-agent-teammate)

# The self-test is the acceptance entry point for slice v4-ai-teammate: it
# runs the local-model inference point, the narrow-role whitelist rejection,
# the frame-anchor provenance check, the "时光作证" old-frame citation, and the
# model-unavailable error+count path — the five acceptance criteria.
# Deterministic and headless; exits non-zero if any criterion fails.
add_test(NAME demo-agent-teammate_self_test
         COMMAND $<TARGET_FILE:demo-agent-teammate> --self-test)
set_tests_properties(demo-agent-teammate_self_test PROPERTIES TIMEOUT 60)
