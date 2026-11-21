# engine/tests/CMakeLists.txt
# =============================================================================
# The v1-device-policy (D3 / D4) deterministic tests.
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
