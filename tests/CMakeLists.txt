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
    if(WIN32)
        target_link_libraries(${name} PRIVATE ws2_32)
    endif()
    target_link_libraries(${name} PRIVATE nimrtc::rtp)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${CMAKE_SOURCE_DIR}/src/plugins/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    nimrtc_apply_options(${name})
    # Test exe paths differ between single-config (Linux Makefile) and multi-
    # config (MSVC) generators.  On Linux the exe lives at tests/<name>; on
    # MSVC the multi-config generator places it under tests/<CONFIG>/<name>.exe.
    #
    # We used to hardcode "$<$<BOOL:${WIN32}>:Debug/>$<TARGET_FILE:...>" here,
    # but that double-prefixed the path: $<TARGET_FILE> under MSVC already
    # resolves to "tests/Debug/test.exe" (the absolute path), and the gen-expr
    # appended "Debug/" in front of it, producing "Debug/D:/MyOpen/.../tests/
    # Debug/test.exe" which ctest reports as NOT_AVAILABLE.  Just use
    # $<TARGET_FILE> directly and require `-C <config>` to be passed to ctest
    # (or rely on the default Debug selection when invoked from VS Test
    # Explorer).  This is the standard CMake convention for add_test().
    add_test(NAME ${name}
        COMMAND $<TARGET_FILE:${name}>)
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
    if(WIN32)
        target_link_libraries(${name} PRIVATE ws2_32)
    endif()
    target_link_libraries(${name} PRIVATE
        nimrtc::srtp)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${CMAKE_SOURCE_DIR}/src/plugins/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    # Mirror the private definition on the srtp target so the test sees it.
    target_compile_definitions(${name} PRIVATE NIMRTC_USE_LIBSRTP=1)
    nimrtc_apply_options(${name})
    # See add_e2e_test() above for the rationale on the COMMAND expression.
    add_test(NAME ${name}
        COMMAND $<TARGET_FILE:${name}>)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
endfunction()

# Only build if libsrtp was vendored (NIMRTC_USE_LIBSRTP defined on nimrtc_srtp target).
if(TARGET nimrtc::vendor::libsrtp)
    add_srtp_test(test_srtp test_srtp.cpp)
endif()

# ----------------------------------------------------------------------------
# P2.4 DTLS handshake end-to-end test.
# Two in-process DtlsSession objects (one Client, one Server) drive a real
# wolfSSL handshake over the byte-buffer I/O pipe.  Validates the fix for
# the KEEP_PEER_CERT + WOLFSSL_VERIFY_PEER path that lets
# wolfSSL_get_peer_certificate() return a non-NULL X509 so the SPKI pin
# check at the end of the handshake can compare against the SDP-declared
# fingerprint.
# ----------------------------------------------------------------------------
function(add_dtls_handshake_test name source)
    add_executable(${name} ${source})
    if(WIN32)
        target_link_libraries(${name} PRIVATE ws2_32)
    endif()
    target_link_libraries(${name} PRIVATE nimrtc::dtls)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    nimrtc_apply_options(${name})
    add_test(NAME ${name}
        COMMAND $<TARGET_FILE:${name}>)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
    nimrtc_copy_wolfssl_certs(${name})
endfunction()
add_dtls_handshake_test(test_dtls_handshake_e2e test_dtls_handshake_e2e.cpp)

# ----------------------------------------------------------------------------
# P2.4-DTLS-regression: handshake-flight double-emission guard.
#
# Exercises a single retransmit cycle in isolation and asserts that
# take_outbound() yields at most ONE record per timer fire (the fix for
# the Case D "intermittent Chrome failure" caused by tick() + take_outbound()
# both calling pump_handshake()).  Runs for ~3.6 s (drives two retransmit
# cycles: 1 s initial timer, then 2 s after the first retransmit).
#
# Timeout bumped to 30 s — the test itself is ~4 s, leaving headroom for
# cold-start cert path resolution on Windows where NimRTC's wolfSSL
# cert fallback chain walks `certs/`, `tests/wolfssl_dtls/certs/`, etc.
# ----------------------------------------------------------------------------
add_dtls_handshake_test(test_dtls_retransmit_no_duplicate
                        test_dtls_retransmit_no_duplicate.cpp)
set_tests_properties(test_dtls_retransmit_no_duplicate
    PROPERTIES TIMEOUT 30)

# ----------------------------------------------------------------------------
# Slice 6 follow-up: ARQ-UDP transport e2e tests (A/B/C/D).
#
# These four tests form a progressive stack-validation sequence:
#   A. test_dtls_over_udp_loopback
#        DTLS handshake over raw Winsock/POSIX UDP sockets (no ARQ).
#   B. test_dtls_over_arq_udp
#        DTLS handshake over ArqRawUdp (selective-repeat ARQ over UDP).
#        Proves ARQ framing does not corrupt DTLS records.
#   C. test_srtp_over_dtls_over_arq_udp
#        Same as B, plus SRTP protect/unprotect round-trip with one RTP packet.
#        Proves DTLS-SRTP key derivation + SRTP encrypt/decrypt work through ARQ.
#   D. test_rtp_srtp_arq_e2e
#        Full pipeline: 10-packet burst A→B + 5-packet burst B→A.
#        Proves bidirectional media delivery + ARQ stats + SRTP stats.
#
# All four tests are standalone (custom main(), no GoogleTest).  They use
# raw Winsock/POSIX sockets only — no ICE, no STUN, no libjuice.
# ----------------------------------------------------------------------------
function(add_arq_e2e_test name source)
    add_executable(${name} ${source})
    if(WIN32)
        target_link_libraries(${name} PRIVATE ws2_32)
    endif()
    # Link the modules the test needs.
    target_link_libraries(${name} PRIVATE
        nimrtc::dtls
        nimrtc::raw_udp
        nimrtc::core)
    # SRTP tests also need the SRTP target + NIMRTC_USE_LIBSRTP=1.
    # Add SRTP and NIMRTC_USE_LIBSRTP only when the stub impl is not forced.
    if(TARGET nimrtc::srtp)
        target_link_libraries(${name} PRIVATE nimrtc::srtp)
        target_compile_definitions(${name} PRIVATE NIMRTC_USE_LIBSRTP=1)
    endif()
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${CMAKE_SOURCE_DIR}/src/plugins/include"
        "${CMAKE_SOURCE_DIR}/src/modules/dtls/include"
        "${CMAKE_SOURCE_DIR}/src/raw_udp/include"
        "${CMAKE_SOURCE_DIR}/src/modules/srtp/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    nimrtc_apply_options(${name})
    add_test(NAME ${name}
        COMMAND $<TARGET_FILE:${name}>)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
    # DtlsSession uses wolfSSL certs; copy them into the test binary dir.
    nimrtc_copy_wolfssl_certs(${name})
endfunction()

add_arq_e2e_test(test_dtls_over_udp_loopback   test_dtls_over_udp_loopback.cpp)
add_arq_e2e_test(test_dtls_over_arq_udp        test_dtls_over_arq_udp.cpp)
add_arq_e2e_test(test_srtp_over_dtls_over_arq_udp
                                               test_srtp_over_dtls_over_arq_udp.cpp)
add_arq_e2e_test(test_rtp_srtp_arq_e2e        test_rtp_srtp_arq_e2e.cpp)

# Timeout for the burst test (D): it sends 10+5=15 packets at 5ms intervals,
# plus 3s receive timeout, plus the initial DTLS handshake.
set_tests_properties(test_rtp_srtp_arq_e2e PROPERTIES TIMEOUT 60)

# ----------------------------------------------------------------------------
# Cross-module DTLS↔SRTP timing integration test.
# Two engines run on loopback, ICE+DTLS handshake; verifies that after
# DTLS Connected, the engine's maybe_install_srtp_keys() flips
# srtp_installed_ to true.
# ----------------------------------------------------------------------------
function(add_dtls_srtp_test name source)
    add_executable(${name} ${source})
    if(WIN32)
        target_link_libraries(${name} PRIVATE ws2_32)
    endif()
    target_link_libraries(${name} PRIVATE
        nimrtc::engine
        nimrtc::srtp
        nimrtc::dtls)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${CMAKE_SOURCE_DIR}/src/plugins/include"
        "${CMAKE_SOURCE_DIR}/src/engine/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    target_compile_definitions(${name} PRIVATE NIMRTC_USE_LIBSRTP=1)
    nimrtc_apply_options(${name})
    # See add_e2e_test() for the rationale on the COMMAND expression.
    add_test(NAME ${name}
        COMMAND $<TARGET_FILE:${name}>)
    set_tests_properties(${name} PROPERTIES TIMEOUT 60)
    nimrtc_copy_wolfssl_certs(${name})
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
    if(WIN32)
        target_link_libraries(${name} PRIVATE ws2_32 bcrypt)
    endif()
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
        # Force console subsystem so ctest captures our diagnostic output.
        set_target_properties(${name} PROPERTIES WIN32_EXECUTABLE OFF)
    endif()
    nimrtc_apply_options(${name})
    # See add_e2e_test() above for the rationale on the COMMAND expression.
    add_test(NAME ${name}
        COMMAND $<TARGET_FILE:${name}>)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
endfunction()
if(WIN32)
    add_dtls_gcm_aead_test(test_dtls_gcm_aead test_dtls_gcm_aead.cpp)
endif()

# In-process NimRTC seal/open round-trip test (uses identical key/salt/nonce
# to isolate AEAD correctness from key derivation).
add_dtls_gcm_aead_test(test_dtls_inproc test_dtls_inproc.cpp)
nimrtc_copy_wolfssl_certs(test_dtls_inproc)

# ----------------------------------------------------------------------------
# TLS 1.2 PRF / HMAC-SHA-256 known-answer test.
#
# Verifies nimrtc::dtls::tls12_prf against an independent Python reference
# (see build/gen_kat.py).  Now backed by wolfSSL HMAC-SHA-256
# (see src/modules/dtls/src/dtls_prf.cpp) instead of BCrypt, so the test
# runs identically on Windows / Linux / macOS — no `#ifdef _WIN32` gate.
# ----------------------------------------------------------------------------
function(add_dtls_prf_kat_test name source)
    add_executable(${name} ${source})
    if(WIN32)
        target_link_libraries(${name} PRIVATE ws2_32)
    endif()
    target_link_libraries(${name} PRIVATE
        nimrtc::dtls)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${CMAKE_SOURCE_DIR}/src/plugins/include"
        "${CMAKE_SOURCE_DIR}/src/modules/dtls/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    nimrtc_apply_options(${name})
    # See add_e2e_test() above for the rationale on the COMMAND expression.
    add_test(NAME ${name}
        COMMAND $<TARGET_FILE:${name}>)
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
    if(WIN32)
        target_link_libraries(${name} PRIVATE ws2_32 bcrypt)
    endif()
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include")
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    nimrtc_apply_options(${name})
    # See add_e2e_test() above for the rationale on the COMMAND expression.
    add_test(NAME ${name}
        COMMAND $<TARGET_FILE:${name}>)
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
    if(WIN32)
        target_link_libraries(${name} PRIVATE ws2_32)
    else()
        # GCC on Linux/libstdc++ requires explicit -latomic for 64-bit
        # std::atomic operations on 32-bit platforms or for non-trivial
        # atomics (e.g. PcmFrameMetadata).  Safe to add unconditionally.
        target_link_libraries(${name} PRIVATE atomic)
    endif()
    target_link_libraries(${name} PRIVATE
        nimrtc::engine
        # R2.5: link every concrete module that has a register_default_plugins()
        # so the engine's open() → register_all_default_plugins() chain resolves.
        nimrtc::ice
        nimrtc::rtp
        nimrtc::sdp
        nimrtc::jb
        nimrtc::audio3a
        GTest::gtest_main)
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
    # See add_e2e_test() for the rationale on the COMMAND expression.
    add_test(NAME ${name}
        COMMAND $<TARGET_FILE:${name}>)
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
    if(WIN32)
        target_link_libraries(${name} PRIVATE ws2_32)
    endif()
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
    # See add_e2e_test() for the rationale on the COMMAND expression.
    add_test(NAME ${name}
        COMMAND $<TARGET_FILE:${name}>)
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
    if(WIN32)
        target_link_libraries(${name} PRIVATE ws2_32)
    endif()
    target_link_libraries(${name} PRIVATE
        nimrtc::video_source
        nimrtc::video_sink
        nimrtc::video_pipeline
        nimrtc::plugins
        nimrtc::core
        GTest::gtest_main)
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
    # See add_e2e_test() for the rationale on the COMMAND expression.
    add_test(NAME ${name}
        COMMAND $<TARGET_FILE:${name}>)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
    if(COMMAND gtest_discover_tests)
        gtest_discover_tests(${name}
            PROPERTIES TIMEOUT 30
            DISCOVERY_TIMEOUT 10)
    endif()
endfunction()

if(NIMRTC_MODULE_VIDEO_SOURCE AND NIMRTC_MODULE_VIDEO_SINK AND NIMRTC_MODULE_VIDEO_PIPELINE)
nimrtc_add_video_plugin_adapter_test(test_video_plugin_adapters
                                      test_video_plugin_adapters.cpp)
endif()

# R3-Batch validation: engine resolves + exposes video plugins.
function(nimrtc_add_engine_video_test name source)
    add_executable(${name} ${source})
    if(WIN32)
        target_link_libraries(${name} PRIVATE ws2_32)
    endif()
    target_link_libraries(${name} PRIVATE
        nimrtc::engine
        nimrtc::core
        nimrtc::plugins
        nimrtc::video_source
        nimrtc::video_sink
        nimrtc::video_pipeline
        GTest::gtest_main)
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
    # See add_e2e_test() for the rationale on the COMMAND expression.
    add_test(NAME ${name}
        COMMAND $<TARGET_FILE:${name}>)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
    if(COMMAND gtest_discover_tests)
        gtest_discover_tests(${name}
            PROPERTIES TIMEOUT 30
            DISCOVERY_TIMEOUT 10)
    endif()
endfunction()

# (Function above is referenced only inside the video-source-gated block below;
# defined unconditionally because CMake does not allow function/endfunction
# inside an if()/endif() block per CMP0017.)

if(NIMRTC_MODULE_VIDEO_SOURCE AND NIMRTC_MODULE_VIDEO_SINK AND NIMRTC_MODULE_VIDEO_PIPELINE)
nimrtc_add_engine_video_test(test_engine_video_plugins
                              test_engine_video_plugins.cpp)
endif()

# R3-Batch validation: video codec pipeline — encode/decode round-trip.
function(nimrtc_add_engine_codec_test name source)
    add_executable(${name} ${source})
    if(WIN32)
        target_link_libraries(${name} PRIVATE ws2_32)
    endif()
    target_link_libraries(${name} PRIVATE
        nimrtc::engine
        nimrtc::core
        nimrtc::plugins
        nimrtc::video_source
        nimrtc::video_sink
        nimrtc::video_pipeline
        nimrtc::h264
        GTest::gtest_main)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${CMAKE_SOURCE_DIR}/src/plugins/include"
        "${CMAKE_SOURCE_DIR}/src/engine/include"
        "${CMAKE_SOURCE_DIR}/src/modules/video_source/include"
        "${CMAKE_SOURCE_DIR}/src/modules/video_sink/include"
        "${CMAKE_SOURCE_DIR}/src/modules/video_pipeline/include"
        "${CMAKE_SOURCE_DIR}/src/modules/h264/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    nimrtc_apply_options(${name})
    # See add_e2e_test() for the rationale on the COMMAND expression.
    add_test(NAME ${name}
        COMMAND $<TARGET_FILE:${name}>)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
    if(COMMAND gtest_discover_tests)
        gtest_discover_tests(${name}
            PROPERTIES TIMEOUT 30
            DISCOVERY_TIMEOUT 10)
    endif()
    # test_engine_video_codec links nimrtc::engine → nimrtc::dtls and needs certs.
    nimrtc_copy_wolfssl_certs(${name})
endfunction()

# R3-Batch validation: HW plugin seam + capability flags.
# NOTE: function/endfunction MUST live at file scope (CMP0017). Define it
# unconditionally here; the call site below gates with the same if() guard.
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
    # See add_e2e_test() for the rationale on the COMMAND expression.
    add_test(NAME ${name}
        COMMAND $<TARGET_FILE:${name}>)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
    if(COMMAND gtest_discover_tests)
        gtest_discover_tests(${name}
            PROPERTIES TIMEOUT 30
            DISCOVERY_TIMEOUT 10)
    endif()
endfunction()

if(NIMRTC_MODULE_VIDEO_SOURCE AND NIMRTC_MODULE_VIDEO_SINK AND NIMRTC_MODULE_VIDEO_PIPELINE)
nimrtc_add_hw_seam_test(test_hw_plugin_seam test_hw_plugin_seam.cpp)
endif()

if(NIMRTC_MODULE_VIDEO_SOURCE AND NIMRTC_MODULE_VIDEO_SINK AND NIMRTC_MODULE_VIDEO_PIPELINE
   AND NIMRTC_MODULE_H264)
nimrtc_add_engine_codec_test(test_engine_video_codec
                              test_engine_video_codec.cpp)
endif()

# R2.5 validation: engine consumes PluginRegistry for plugin slots.
nimrtc_add_engine_test(test_engine_plugin_loading test_engine_plugin_loading.cpp)

# ---------------------------------------------------------------------------
# P2 PCM tap test (audio3a pre/post 3A tap, §8.7).
# Verifies round-trip: IAudio3A instance, install counting tap, feed 100 frames,
# assert tap invocation count.
# ---------------------------------------------------------------------------
nimrtc_add_engine_test(test_audio3a_tap plugins/test_audio3a_tap.cpp)

# ---------------------------------------------------------------------------
# Chrome↔NimRTC DTLS-SRTP interop bridge (tests/wolfssl_dtls/bridge/).
#
# This sub-project is gated by `NIMRTC_BUILD_BRIDGE_TESTS` (off by default)
# because:
#   1. It needs to be run interactively with a real browser (Chrome, Firefox,
#      Safari) — it's not a unit test and CI would always skip it.
#   2. It builds a SECOND standalone wolfSSL (DTLS + SRTP + ECC + AES-GCM)
#      which slows clean-configure times by 1-2 minutes.
#   3. The bridge binary is launched by `run_bridge.py` and lives outside
#      ctest's purview.
#
# Enable with:
#   cmake -B build_msvc -S . -DNIMRTC_BUILD_BRIDGE_TESTS=ON
#   cmake --build build_msvc --target bridge
# ---------------------------------------------------------------------------
if(NIMRTC_BUILD_BRIDGE_TESTS)
    message(STATUS "NIMRTC_BUILD_BRIDGE_TESTS=ON — adding "
                   "tests/wolfssl_dtls/bridge/ (interoperability responder)")
    add_subdirectory(wolfssl_dtls/bridge EXCLUDE_FROM_ALL)
endif()

# ---------------------------------------------------------------------------
# P3 NVENC hardware encoder smoke test.
#
# Talks directly to the NVIDIA Video Codec SDK 13.1 — opens a D3D11 device,
# configures an H.264 session, encodes 30 frames of a synthetic gradient,
# and verifies the output bitstream is valid Annex B (00 00 00 01 prefix
# + IDR NAL unit).  Skipped (ctest code 77) when no NVIDIA GPU / SDK is
# detected; fails with code 1 if NVENC was detected but produced 0 bytes.
#
# Run on any machine with an RTX/Ampere/Hopper card and the driver installed.
# Headers must be downloaded once into the build tree:
#   https://developer.nvidia.com/nvidia-video-codec-sdk
# (Header-only download: video_codec_interface_13.1.15.zip).  The
# FindNVENC.cmake module falls back to nvEncodeAPI64.dll as the import lib
# so the CUDA Toolkit isn't required.
# ---------------------------------------------------------------------------
if(NIMRTC_HW_HAVE_NVENC)
    add_executable(test_nvenc_probe nvenc_probe.cpp)
    target_compile_definitions(test_nvenc_probe PRIVATE NIMRTC_PLUGINS_NVENC_ON=1)
    if(WIN32)
        target_link_libraries(test_nvenc_probe PRIVATE d3d11)
        target_compile_definitions(test_nvenc_probe PRIVATE _WIN32_WINNT=0x0601)
    endif()
    target_include_directories(test_nvenc_probe PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${NVENC_INCLUDE_DIR}")
    nimrtc_apply_options(test_nvenc_probe)
    # See add_e2e_test() for the rationale on the COMMAND expression.
    add_test(NAME test_nvenc_probe
        COMMAND $<TARGET_FILE:test_nvenc_probe>)
    set_tests_properties(test_nvenc_probe PROPERTIES TIMEOUT 60)

    # ABI probe: auto-detects the correct API / struct version combination.
    add_executable(test_nvenc_abi_probe nvenc_abi_probe.cpp)
    target_compile_definitions(test_nvenc_abi_probe PRIVATE NIMRTC_PLUGINS_NVENC_ON=1)
    if(WIN32)
        target_link_libraries(test_nvenc_abi_probe PRIVATE d3d11)
        target_compile_definitions(test_nvenc_abi_probe PRIVATE _WIN32_WINNT=0x0601)
    endif()
    target_include_directories(test_nvenc_abi_probe PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${NVENC_INCLUDE_DIR}")
    nimrtc_apply_options(test_nvenc_abi_probe)
    # Not registered with ctest — run manually as: cmake --build --target test_nvenc_abi_probe && build/tests/Debug/test_nvenc_abi_probe.exe
endif()
