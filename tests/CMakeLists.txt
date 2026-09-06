# tests/CMakeLists.txt — top-level cross-module integration tests
# ----------------------------------------------------------------------------
# Tests that span multiple modules (e.g. RTP+JB+SDP end-to-end) live here.
# Single-module tests live inside each module's `tests/` directory.
# ----------------------------------------------------------------------------

# E2E tests are standalone (custom main(), no GoogleTest). They each use
# raw sockets to verify round-trip behaviour. add_e2e_test is a small wrapper
# that wires the include dirs, links Winsock on Windows, and registers the
# test with CTest.
#
# NOTE: We bypass CMake 4.0's broken CTEST_CONFIGURATION_TYPE conditional in
# CTestTestfile.cmake (which marks non-GTest tests as NOT_AVAILABLE under
# multi-config generators) by hardcoding the Debug/<exe> path here. A
# Release-config build would need a second add_test line — multi-config
# support for non-GTest tests is a known limitation.
function(add_e2e_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        nimrtc::rtp
        ws2_32)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${CMAKE_SOURCE_DIR}/src/plugins/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    nimrtc_apply_options(${name})
    add_test(NAME ${name}
        COMMAND ${CMAKE_BINARY_DIR}/tests/Debug/${name}.exe)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
endfunction()

# RTP round-trip over UDP loopback.
add_e2e_test(test_udp_loopback  test_udp_loopback.cpp)

# RTCP SR/RR/NACK round-trip over UDP loopback.
add_e2e_test(test_rtcp_loopback test_rtcp_loopback.cpp)

# Parser error-path matrix (no network).
add_e2e_test(test_error_matrix  test_error_matrix.cpp)

# ----------------------------------------------------------------------------
# SRTP unit tests (libsrtp2 AES-CM encrypt/decrypt/replay/auth).
# Requires libsrtp2 vendored — guarded by NIMRTC_USE_LIBSRTP definition.
# ----------------------------------------------------------------------------
function(add_srtp_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        nimrtc::srtp
        ws2_32)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${CMAKE_SOURCE_DIR}/src/plugins/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    # Mirror the private definition on the srtp target so the test sees it.
    target_compile_definitions(${name} PRIVATE NIMRTC_USE_LIBSRTP=1)
    nimrtc_apply_options(${name})
    add_test(NAME ${name}
        COMMAND ${CMAKE_BINARY_DIR}/tests/Debug/${name}.exe)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
endfunction()

# Only build if libsrtp was vendored (NIMRTC_USE_LIBSRTP defined on nimrtc_srtp target).
if(TARGET nimrtc::vendor::libsrtp)
    add_srtp_test(test_srtp test_srtp.cpp)
endif()

# ----------------------------------------------------------------------------
# Cross-module DTLS↔SRTP timing integration test.
# Two engines run on loopback, ICE+DTLS handshake; verifies that after
# DTLS Connected, the engine's maybe_install_srtp_keys() flips
# srtp_installed_ to true.
# ----------------------------------------------------------------------------
function(add_dtls_srtp_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        nimrtc::engine
        nimrtc::srtp
        nimrtc::dtls
        ws2_32)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${CMAKE_SOURCE_DIR}/src/plugins/include"
        "${CMAKE_SOURCE_DIR}/src/engine/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    target_compile_definitions(${name} PRIVATE NIMRTC_USE_LIBSRTP=1)
    nimrtc_apply_options(${name})
    add_test(NAME ${name}
        COMMAND ${CMAKE_BINARY_DIR}/tests/Debug/${name}.exe)
    set_tests_properties(${name} PROPERTIES TIMEOUT 60)
endfunction()

if(TARGET nimrtc::engine AND TARGET nimrtc::vendor::libsrtp)
    add_dtls_srtp_test(test_dtls_srtp_timing test_dtls_srtp_timing.cpp)
endif()

# ----------------------------------------------------------------------------
# BCrypt AES-128-GCM round-trip test — verifies what tag length and shape
# BCrypt actually produces/consumes so we can match the NimRTC seal/open
# paths exactly.
# ----------------------------------------------------------------------------
function(add_dtls_gcm_aead_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ws2_32 bcrypt)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
        # Force console subsystem so ctest captures our diagnostic output.
        set_target_properties(${name} PROPERTIES WIN32_EXECUTABLE OFF)
    endif()
    nimrtc_apply_options(${name})
    add_test(NAME ${name}
        COMMAND ${CMAKE_BINARY_DIR}/tests/Debug/${name}.exe)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
endfunction()
if(WIN32)
    add_dtls_gcm_aead_test(test_dtls_gcm_aead test_dtls_gcm_aead.cpp)
endif()

# In-process NimRTC seal/open round-trip test (uses identical key/salt/nonce
# to isolate AEAD correctness from key derivation).
add_dtls_gcm_aead_test(test_dtls_inproc test_dtls_inproc.cpp)

# ----------------------------------------------------------------------------
# TLS 1.2 PRF / HMAC-SHA-256 known-answer test.
# Verifies nimrtc::dtls::tls12_prf against an independent Python reference
# (see build/gen_kat.py).  Only depends on the dtls library.
# ----------------------------------------------------------------------------
function(add_dtls_prf_kat_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        nimrtc::dtls
        ws2_32)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${CMAKE_SOURCE_DIR}/src/plugins/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    nimrtc_apply_options(${name})
    add_test(NAME ${name}
        COMMAND ${CMAKE_BINARY_DIR}/tests/Debug/${name}.exe)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
endfunction()
add_dtls_prf_kat_test(test_dtls_prf_kat test_dtls_prf_kat.cpp)

# ----------------------------------------------------------------------------
# X25519 (RFC 7748) ECDH agreement test — drives BCrypt X25519 directly to
# verify NimRTC's ECDH branch in derive_pre_master_secret.  This is the
# regression test for Step A of the Chrome x25519 negotiation roadmap.
# Only depends on the system BCrypt (no DTLS lib needed).
# ----------------------------------------------------------------------------
function(add_dtls_x25519_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ws2_32 bcrypt)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    nimrtc_apply_options(${name})
    add_test(NAME ${name}
        COMMAND ${CMAKE_BINARY_DIR}/tests/Debug/${name}.exe)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
endfunction()
if(WIN32)
    add_dtls_x25519_test(test_dtls_x25519 test_dtls_x25519.cpp)
endif()

# ----------------------------------------------------------------------------
# GoogleTest-based tests for cross-module integration.
# These use GTest for richer assertions; link all modules they exercise.
# ----------------------------------------------------------------------------
function(nimrtc_add_engine_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        nimrtc::engine
        # R2.5: link every concrete module that has a register_default_plugins()
        # so the engine's open() → register_all_default_plugins() chain resolves.
        nimrtc::ice
        nimrtc::rtp
        nimrtc::sdp
        nimrtc::jb
        nimrtc::audio3a
        GTest::gtest_main
        ws2_32)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${CMAKE_SOURCE_DIR}/src/plugins/include"
        "${CMAKE_SOURCE_DIR}/src/engine/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    set_target_properties(${name} PROPERTIES
        VS_LINK_LIBRARY_DEPENDENCIES TRUE)
    nimrtc_apply_options(${name})
    # Hardcode the Debug path to avoid CMake 4.0's broken
    # CTEST_CONFIGURATION_TYPE conditional marking this wrapper test as
    # NOT_AVAILABLE when ctest is invoked without `-C <config>`.
    add_test(NAME ${name}
        COMMAND ${CMAKE_BINARY_DIR}/tests/Debug/${name}.exe)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
    if(COMMAND gtest_discover_tests)
        gtest_discover_tests(${name}
            PROPERTIES TIMEOUT 30
            DISCOVERY_TIMEOUT 10)
    endif()
endfunction()

# ----------------------------------------------------------------------------
# Plugin-layer test (header-only; only needs nimrtc::plugins + GTest).
# Verifies the new video plugin interfaces (IVideoSource / IVideoSink /
# IVideoReceiver / IVideoSender) compile and round-trip through the
# extended PluginRegistry.
# ----------------------------------------------------------------------------
function(nimrtc_add_plugin_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        nimrtc::plugins
        GTest::gtest_main)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${CMAKE_SOURCE_DIR}/src/plugins/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    nimrtc_apply_options(${name})
    add_test(NAME ${name}
        COMMAND ${CMAKE_BINARY_DIR}/tests/Debug/${name}.exe)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
    if(COMMAND gtest_discover_tests)
        gtest_discover_tests(${name}
            PROPERTIES TIMEOUT 30
            DISCOVERY_TIMEOUT 10)
    endif()
endfunction()

# R2-Batch2 validation: video plugin interfaces compile + register.
nimrtc_add_plugin_test(test_video_plugin_interfaces test_video_plugin_interfaces.cpp)

# R2-Batch2 validation: video module plugin adapters wire concrete impls
# through core::PluginRegistry (memory source / headless+pinned sinks /
# reference receiver+sender).
function(nimrtc_add_video_plugin_adapter_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        nimrtc::video_source
        nimrtc::video_sink
        nimrtc::video_pipeline
        nimrtc::plugins
        nimrtc::core
        GTest::gtest_main
        ws2_32)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${CMAKE_SOURCE_DIR}/src/plugins/include"
        "${CMAKE_SOURCE_DIR}/src/modules/video_source/include"
        "${CMAKE_SOURCE_DIR}/src/modules/video_sink/include"
        "${CMAKE_SOURCE_DIR}/src/modules/video_pipeline/include"
        "${CMAKE_SOURCE_DIR}/src/modules/video_frame/include"
        "${CMAKE_SOURCE_DIR}/src/modules/video_jb/include"
        "${CMAKE_SOURCE_DIR}/src/modules/video_payload/include"
        "${CMAKE_SOURCE_DIR}/src/modules/rtp/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    nimrtc_apply_options(${name})
    add_test(NAME ${name}
        COMMAND ${CMAKE_BINARY_DIR}/tests/Debug/${name}.exe)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
    if(COMMAND gtest_discover_tests)
        gtest_discover_tests(${name}
            PROPERTIES TIMEOUT 30
            DISCOVERY_TIMEOUT 10)
    endif()
endfunction()

nimrtc_add_video_plugin_adapter_test(test_video_plugin_adapters
                                      test_video_plugin_adapters.cpp)

# R3-Batch validation: engine resolves + exposes video plugins.
function(nimrtc_add_engine_video_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        nimrtc::engine
        nimrtc::core
        nimrtc::plugins
        nimrtc::video_source
        nimrtc::video_sink
        nimrtc::video_pipeline
        GTest::gtest_main
        ws2_32)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${CMAKE_SOURCE_DIR}/src/plugins/include"
        "${CMAKE_SOURCE_DIR}/src/engine/include"
        "${CMAKE_SOURCE_DIR}/src/modules/video_source/include"
        "${CMAKE_SOURCE_DIR}/src/modules/video_sink/include"
        "${CMAKE_SOURCE_DIR}/src/modules/video_pipeline/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    nimrtc_apply_options(${name})
    add_test(NAME ${name}
        COMMAND ${CMAKE_BINARY_DIR}/tests/Debug/${name}.exe)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
    if(COMMAND gtest_discover_tests)
        gtest_discover_tests(${name}
            PROPERTIES TIMEOUT 30
            DISCOVERY_TIMEOUT 10)
    endif()
endfunction()

nimrtc_add_engine_video_test(test_engine_video_plugins
                              test_engine_video_plugins.cpp)

# R3-Batch validation: HW plugin seam + capability flags.
function(nimrtc_add_hw_seam_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        nimrtc::core
        nimrtc::plugins
        nimrtc::ice
        nimrtc::sdp
        nimrtc::jb
        nimrtc::audio3a
        nimrtc::video_source
        nimrtc::video_sink
        nimrtc::video_pipeline
        GTest::gtest_main)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${CMAKE_SOURCE_DIR}/src/plugins/include"
        "${CMAKE_SOURCE_DIR}/src/modules/video_source/include"
        "${CMAKE_SOURCE_DIR}/src/modules/video_sink/include"
        "${CMAKE_SOURCE_DIR}/src/modules/video_pipeline/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    nimrtc_apply_options(${name})
    add_test(NAME ${name}
        COMMAND ${CMAKE_BINARY_DIR}/tests/Debug/${name}.exe)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
    if(COMMAND gtest_discover_tests)
        gtest_discover_tests(${name}
            PROPERTIES TIMEOUT 30
            DISCOVERY_TIMEOUT 10)
    endif()
endfunction()

nimrtc_add_hw_seam_test(test_hw_plugin_seam test_hw_plugin_seam.cpp)

# R2.5 validation: engine consumes PluginRegistry for plugin slots.
nimrtc_add_engine_test(test_engine_plugin_loading test_engine_plugin_loading.cpp)

# ---------------------------------------------------------------------------
# P2 PCM tap test (audio3a pre/post 3A tap, §8.7).
# Verifies round-trip: IAudio3A instance, install counting tap, feed 100 frames,
# assert tap invocation count.
# ---------------------------------------------------------------------------
nimrtc_add_engine_test(test_audio3a_tap plugins/test_audio3a_tap.cpp)
