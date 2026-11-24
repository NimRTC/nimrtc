# engine/tests/CMakeLists.txt
# =============================================================================
# The v1-device-policy (D3 / D4) and v3-n4-d6 connectivity (N4 / D6)
# deterministic tests.
#
# They live *inside* the engine rather than in the top-level tests/ dir
# because the two halves they pin are engine-private:
#
#   - the device-policy resolution rules (`engine_device_policy.hpp`, a
#     private engine header), and
#   - the capture-reconnect backoff machine (`engine_capture_reconnect.hpp`,
#     also private).
#
# The engine integration half drives a real `NimRTCEngine` with fake
# capture devices (the same pattern the top-level engine tests use), so the
# binary needs the same link set: the engine, the software H.264 adapter for
# the capture pump's encoder, the pipeline's "reference"/"headless" plugins
# and the datachannel registrar (referenced by the unified registrar table).
#
# The connectivity half (`test_engine_connectivity.cpp`) pins the N4
# projection vocabulary (`TransportStatsProjector`) and the D6 profile
# resolution (`engine_connectivity_profile.hpp`) — both private engine
# collaborators — with pure functions, so it needs no network and no
# plugin backends.
# =============================================================================

add_executable(test_engine_device_policy
    test_engine_device_policy.cpp
    )

if(WIN32)
    target_link_libraries(test_engine_device_policy PRIVATE ws2_32)
endif()

target_link_libraries(test_engine_device_policy PRIVATE
    nimrtc::engine
    nimrtc::core
    nimrtc::plugins
    nimrtc::video_source
    nimrtc::video_sink
    nimrtc::video_pipeline
    nimrtc::video_capture
    nimrtc::h264
    # PAL Slice 2 (v0.10.2): the unified registrar table references
    # `nimrtc::datachannel::register_default_plugins`, so the symbol must
    # resolve here too (same rationale as the top-level engine tests).
    nimrtc::datachannel
    GTest::gtest_main)

# The private engine headers (`engine_device_policy.hpp`,
# `engine_capture_reconnect.hpp`) are included by the tests, so the
# engine's `src/` directory has to be on the include path.
target_include_directories(test_engine_device_policy PRIVATE
    "${CMAKE_SOURCE_DIR}/src/core/include"
    "${CMAKE_SOURCE_DIR}/src/plugins/include"
    "${CMAKE_SOURCE_DIR}/src/engine/include"
    "${CMAKE_SOURCE_DIR}/src/engine/src"
    "${CMAKE_SOURCE_DIR}/src/modules/video_source/include"
    "${CMAKE_SOURCE_DIR}/src/modules/video_sink/include"
    "${CMAKE_SOURCE_DIR}/src/modules/video_pipeline/include"
    "${CMAKE_SOURCE_DIR}/src/modules/video_capture/include"
    "${CMAKE_SOURCE_DIR}/src/modules/h264/include")

if(WIN32)
    target_compile_definitions(test_engine_device_policy PRIVATE _WIN32_WINNT=0x0601)
endif()

nimrtc_apply_options(test_engine_device_policy LENIENT)

add_test(NAME engine_device_policy_tests
    COMMAND test_engine_device_policy)
set_tests_properties(engine_device_policy_tests PROPERTIES TIMEOUT 60)

nimrtc_copy_wolfssl_certs(test_engine_device_policy)
# The engine links the WebTransport / QUIC seam, so the test binary needs
# msquic.dll next to it on Windows — otherwise the discovery probe (which
# runs the .exe to enumerate cases) dies with STATUS_DLL_NOT_FOUND.  The
# PRE_TEST mode below defers that probe to test-time, by which point the
# POST_BUILD DLL copy has completed (cmake/NimRTCTest.cmake:130-134).
nimrtc_copy_msquic_dll(test_engine_device_policy)
if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_engine_device_policy
        DISCOVERY_MODE PRE_TEST
        PROPERTIES TIMEOUT 60
        DISCOVERY_TIMEOUT 10)
endif()

# -----------------------------------------------------------------------------
# v1-audio-device-engine (D1) — the engine-driven audio device layer.
#
# The audio-device half drives a real `NimRTCEngine` with fake capture /
# playback devices and the same fake ICE / DTLS / codec set the top-level
# audio tests use, so the binary needs the device-policy link set plus the
# audio_device module (whose registrar entry is inert in the unified table:
# `NIMRTC_HAS_AUDIO_DEVICE` is not defined for `nimrtc_core_objects`).
# -----------------------------------------------------------------------------
add_executable(test_engine_audio_device
    test_engine_audio_device.cpp
    )

if(WIN32)
    target_link_libraries(test_engine_audio_device PRIVATE ws2_32)
endif()

target_link_libraries(test_engine_audio_device PRIVATE
    nimrtc::engine
    nimrtc::core
    nimrtc::plugins
    nimrtc::video_source
    nimrtc::video_sink
    nimrtc::video_pipeline
    nimrtc::video_capture
    nimrtc::h264
    nimrtc::datachannel
    GTest::gtest_main)

if(NIMRTC_MODULE_AUDIO_DEVICE)
    target_link_libraries(test_engine_audio_device PRIVATE nimrtc::audio_device)
endif()

target_include_directories(test_engine_audio_device PRIVATE
    "${CMAKE_SOURCE_DIR}/src/core/include"
    "${CMAKE_SOURCE_DIR}/src/plugins/include"
    "${CMAKE_SOURCE_DIR}/src/engine/include"
    "${CMAKE_SOURCE_DIR}/src/engine/src"
    "${CMAKE_SOURCE_DIR}/src/modules/audio_device/include"
    "${CMAKE_SOURCE_DIR}/src/modules/dtls/include")

if(WIN32)
    target_compile_definitions(test_engine_audio_device PRIVATE _WIN32_WINNT=0x0601)
endif()

nimrtc_apply_options(test_engine_audio_device LENIENT)

add_test(NAME engine_audio_device_tests
    COMMAND test_engine_audio_device)
set_tests_properties(engine_audio_device_tests PROPERTIES TIMEOUT 60)

nimrtc_copy_wolfssl_certs(test_engine_audio_device)
nimrtc_copy_msquic_dll(test_engine_audio_device)
if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_engine_audio_device
        DISCOVERY_MODE PRE_TEST
        PROPERTIES TIMEOUT 60
        DISCOVERY_TIMEOUT 10)
endif()

# -----------------------------------------------------------------------------
# v3-n4-d6 connectivity (N4 / D6) — pure-function tests of the two private
# engine collaborators.
# -----------------------------------------------------------------------------
add_executable(test_engine_connectivity
    test_engine_connectivity.cpp
    )

if(WIN32)
    target_link_libraries(test_engine_connectivity PRIVATE ws2_32)
endif()

# The connectivity tests call only the private engine collaborators, so
# they need no concrete plugin backends — the engine library carries the
# two TUs (`engine_transport_stats.cpp`, `engine_connectivity_profile.cpp`)
# whose symbols the tests resolve.
target_link_libraries(test_engine_connectivity PRIVATE
    nimrtc::engine
    nimrtc::core
    nimrtc::plugins
    GTest::gtest_main)

target_include_directories(test_engine_connectivity PRIVATE
    "${CMAKE_SOURCE_DIR}/src/core/include"
    "${CMAKE_SOURCE_DIR}/src/plugins/include"
    "${CMAKE_SOURCE_DIR}/src/engine/include"
    "${CMAKE_SOURCE_DIR}/src/engine/src"
    "${CMAKE_SOURCE_DIR}/src/modules/ice/include"
    "${CMAKE_SOURCE_DIR}/src/modules/dtls/include")

if(WIN32)
    target_compile_definitions(test_engine_connectivity PRIVATE _WIN32_WINNT=0x0601)
endif()

nimrtc_apply_options(test_engine_connectivity LENIENT)

nimrtc_copy_msquic_dll(test_engine_connectivity)
if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_engine_connectivity
        DISCOVERY_MODE PRE_TEST
        PROPERTIES TIMEOUT 60
        DISCOVERY_TIMEOUT 10)
endif()

# -----------------------------------------------------------------------------
# v1-ice-recovery (N3) — tests of the private ICE-recovery state machine.
#
# `engine_ice_recovery.hpp` is a **private** engine TU header (same shape as
# `engine_transport_stats.hpp` / `engine_connectivity_profile.hpp`), so the
# include path needs `src/engine/src`. The live-engine cases also reach the
# public `engine.hpp` and the plugin `ice_transport.hpp` seam.
# -----------------------------------------------------------------------------
add_executable(test_engine_ice_recovery
    test_engine_ice_recovery.cpp
    )

if(WIN32)
    target_link_libraries(test_engine_ice_recovery PRIVATE ws2_32)
endif()

target_link_libraries(test_engine_ice_recovery PRIVATE
    nimrtc::engine
    nimrtc::core
    nimrtc::plugins
    nimrtc::datachannel
    GTest::gtest_main)

target_include_directories(test_engine_ice_recovery PRIVATE
    "${CMAKE_SOURCE_DIR}/src/core/include"
    "${CMAKE_SOURCE_DIR}/src/plugins/include"
    "${CMAKE_SOURCE_DIR}/src/engine/include"
    "${CMAKE_SOURCE_DIR}/src/engine/src"
    "${CMAKE_SOURCE_DIR}/src/modules/ice/include"
    "${CMAKE_SOURCE_DIR}/src/modules/dtls/include")

if(WIN32)
    target_compile_definitions(test_engine_ice_recovery PRIVATE _WIN32_WINNT=0x0601)
endif()

nimrtc_apply_options(test_engine_ice_recovery LENIENT)

nimrtc_copy_msquic_dll(test_engine_ice_recovery)
if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_engine_ice_recovery
        DISCOVERY_MODE PRE_TEST
        PROPERTIES TIMEOUT 60
        DISCOVERY_TIMEOUT 10)
endif()

# -----------------------------------------------------------------------------
# v1.4 (v1-turn-credentials, N7): the TURN-credentials provider seam.
# -----------------------------------------------------------------------------
# The same shape as `test_engine_ice_recovery` above: the first half pins the
# private `detail::TurnCredentialsState` collaborator with a virtual clock
# (every machine entry point takes `now_us` explicitly, so no test sleeps);
# the second half drives a real `NimRTCEngine` with the stateful fake ICE
# transport (reused from the recovery test's pattern) to prove the wiring
# end-to-end: open() consults the provider once, a static server wins, a
# reconnect re-consults.
add_executable(test_engine_turn_credentials
    test_engine_turn_credentials.cpp
    )

if(WIN32)
    target_link_libraries(test_engine_turn_credentials PRIVATE ws2_32)
endif()

target_link_libraries(test_engine_turn_credentials PRIVATE
    nimrtc::engine
    nimrtc::core
    nimrtc::plugins
    nimrtc::datachannel
    GTest::gtest_main)

target_include_directories(test_engine_turn_credentials PRIVATE
    "${CMAKE_SOURCE_DIR}/src/core/include"
    "${CMAKE_SOURCE_DIR}/src/plugins/include"
    "${CMAKE_SOURCE_DIR}/src/engine/include"
    "${CMAKE_SOURCE_DIR}/src/engine/src"
    "${CMAKE_SOURCE_DIR}/src/modules/ice/include"
    "${CMAKE_SOURCE_DIR}/src/modules/dtls/include")

if(WIN32)
    target_compile_definitions(test_engine_turn_credentials PRIVATE _WIN32_WINNT=0x0601)
endif()

nimrtc_apply_options(test_engine_turn_credentials LENIENT)

nimrtc_copy_msquic_dll(test_engine_turn_credentials)
if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_engine_turn_credentials
        DISCOVERY_MODE PRE_TEST
        PROPERTIES TIMEOUT 60
        DISCOVERY_TIMEOUT 10)
endif()
