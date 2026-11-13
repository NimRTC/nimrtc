# tests/CMakeLists.txt — top-level cross-module integration tests
# ----------------------------------------------------------------------------
# Tests that span multiple modules (e.g. RTP+JB+SDP end-to-end) live here.
# Single-module tests live inside each module's `tests/` directory.
# ----------------------------------------------------------------------------

# ----------------------------------------------------------------------------
# v1.0.0-alpha.1 DoD §12.3.1 gate #6 — core API stability framework tests.
#
# Two GoogleTest executables:
#   - test_api_version:        macro framework smoke test (7/7 cases).
#   - test_api_classification: per-header ≥1 STABLE/UNSTABLE marker check
#                              + baseline header-count check (89 files).
#
# Both tests link only `nimrtc::core` + GTest.  The classification test
# also receives NIMRTC_SOURCE_ROOT via target_compile_definitions so it
# can walk the source tree at runtime (see test_api_classification.cpp).
# ----------------------------------------------------------------------------
function(nimrtc_add_core_api_test name source)
    # Extra sources (headers) may be listed after the translation unit.
    # Listing the generated expectation header here keeps it visible in
    # IDEs and makes the build depend on it, so that regenerating it
    # actually rebuilds the test.
    add_executable(${name} ${source} ${ARGN})
    if(WIN32)
        target_link_libraries(${name} PRIVATE ws2_32)
    endif()
    target_link_libraries(${name} PRIVATE
        nimrtc::core
        GTest::gtest_main)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include")
    target_compile_definitions(${name} PRIVATE
        NIMRTC_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    nimrtc_apply_options(${name} LENIENT)
    add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
    if(COMMAND gtest_discover_tests)
        gtest_discover_tests(${name}
            PROPERTIES TIMEOUT 30
            DISCOVERY_TIMEOUT 10)
    endif()
endfunction()

nimrtc_add_core_api_test(test_api_version        test_api_version.cpp
                                                  api_version_expectations.hpp)
nimrtc_add_core_api_test(test_api_classification test_api_classification.cpp)

# ----------------------------------------------------------------------------
# Architecture-remediation Phase 0.2 — registry consistency guard.
#
# Pins the contract that the `StackConfig` defaults match the ids the
# modules actually register, that every resolvable registry slot is
# populated, that a registration id survives the lifetime of the string
# that carried it, and that a failed factory lookup yields a nullptr
# rather than a half-built stack.
#
# See `docs/plan/architecture-remediation-plan.md` §0.2 (defects A1, A2,
# A3, A11) and the file header of `test_registry_contract.cpp`.
#
# This test is deliberately RED on the unfixed tree — that is what makes
# it a guard rather than a passing decoration.
#
# NOTE: the target is created further down, right after
# `nimrtc_add_engine_test()` is defined, because it must link every
# module that appears in the unified `kDefaultRegistrars[]` table
# (see the LNK2001 rationale in `nimrtc_add_engine_test`).
# ----------------------------------------------------------------------------

# ----------------------------------------------------------------------------
# v0.14.0 alpha.2 — WebTransport seam compile-only test
# (`docs/plan/v0.14-plan.md` §3.1 Gate #2 + `docs/adr/ADR-014`).
#
# The seam is pure-virtual + vendor-agnostic; this test only needs
# `nimrtc::core` + the new seam headers (no msquic link).  The test
# verifies that:
#   - Every interface (`IWebTransportSession` / `IBidirectionalStream` /
#     `IUnidirectionalStream` / `DatagramFacade`) is well-formed and
#     its `static_assert`s fire at compile time.
#   - The `WebTransportConfig` defaults match the v1.0-plan.md §10.4
#     contract (RFC 9000 / RFC 9221).
#   - The `WebTransportState` enum values round-trip.
#
# The actual implementation (`WebTransportMsQuicSession`, etc.) lands
# in v0.14.0-beta.1 alongside the msquic integration; the alpha.2
# cycle ships only the seam + this compile-only test.
# ----------------------------------------------------------------------------
function(nimrtc_add_web_transport_seam_test name source)
    add_executable(${name} ${source})
    if(WIN32)
        target_link_libraries(${name} PRIVATE ws2_32)
    endif()
    target_link_libraries(${name} PRIVATE
        nimrtc::core
        GTest::gtest_main)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${CMAKE_SOURCE_DIR}/src/modules/web_transport/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    nimrtc_apply_options(${name} LENIENT)
    add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
    if(COMMAND gtest_discover_tests)
        gtest_discover_tests(${name}
            PROPERTIES TIMEOUT 30
            DISCOVERY_TIMEOUT 10)
    endif()
endfunction()

nimrtc_add_web_transport_seam_test(test_web_transport_seam
                                    test_web_transport_seam.cpp)

# ----------------------------------------------------------------------------
# v0.14.0-beta.1 (DC-1 land) — WebTransportDataChannel unit test.
#
# The test drives the `WebTransportDataChannel` against a
# `MockWebTransportSession` (no msquic dependency at the test TU;
# the `nimrtc_web_transport` library is linked PRIVATE so the test
# does not need to know about msquic headers).
#
# What this test verifies (per v1.0-plan.md §12.3.3 Gate #2):
#   - `WebTransportDataChannelFactory` is registered with
#     `core::PluginRegistry` under id `"web_transport"`.
#   - The factory produces a channel that implements
#     `plugins::IDataChannel` with the documented `name()`.
#   - The channel routes `send()` to `send_datagram` (kUnreliable)
#     or a bidi stream (kReliableOrdered) per the cached config.
#   - The bidi stream is opened lazily on first `send()` and reused
#     across multiple sends.
#   - `open()` / `close()` are idempotent; `close()` releases the
#     stream.
# ----------------------------------------------------------------------------
function(nimrtc_add_web_transport_dc_test name source)
    add_executable(${name} ${source})
    if(WIN32)
        target_link_libraries(${name} PRIVATE ws2_32)
    endif()
    target_link_libraries(${name} PRIVATE
        nimrtc::web_transport
        nimrtc::core
        GTest::gtest_main)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${CMAKE_SOURCE_DIR}/src/plugins/include"
        "${CMAKE_SOURCE_DIR}/src/modules/web_transport/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    nimrtc_apply_options(${name} LENIENT)
    add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
    # PRE_TEST discovery defers the .exe invocation (needed to enumerate
    # GTest cases) to ctest-time, after the POST_BUILD msquic.dll copy
    # in `nimrtc_copy_msquic_dll()` has run.  Without PRE_TEST, the
    # POST_BUILD gtest_discover_tests probe fails with STATUS_DLL_NOT_FOUND
    # (0xc0000135) on Windows.
    nimrtc_copy_msquic_dll(${name})
    if(COMMAND gtest_discover_tests)
        gtest_discover_tests(${name}
            PROPERTIES TIMEOUT 30
            DISCOVERY_TIMEOUT 10
            DISCOVERY_MODE PRE_TEST)
    endif()
endfunction()

nimrtc_add_web_transport_dc_test(test_web_transport_data_channel
                                  test_web_transport_data_channel.cpp)

# ----------------------------------------------------------------------------
# v1.2.0 (audio pipeline, WP-1) -- the two new plugin seams.
#
# `tests/test_audio_seams.cpp` pins down the contract that WP-2 (the
# Opus-side decoder) and WP-3 (the audio_rx module) build on:
#
#   - the seam headers compile standalone and expose the documented
#     negotiation defaults,
#   - the registry has a *distinct* category per seam, and plugin ids
#     are per-category -- the Opus decoder may use the id "opus" even
#     though the Opus codec plugin already uses "opus" in the codec
#     category,
#   - the static registration macros work,
#   - the engine-side resolvers find what the registry holds and
#     return nullptr for an unknown id (no silent fallback).
#
# The test is dependency-free by design: it uses fake implementations,
# so it needs neither the Opus module nor the audio_rx module.
# ----------------------------------------------------------------------------
function(nimrtc_add_audio_seam_test name source)
    add_executable(${name} ${source})
    if(WIN32)
        target_link_libraries(${name} PRIVATE ws2_32)
    endif()
    target_link_libraries(${name} PRIVATE
        nimrtc::core
        GTest::gtest_main)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${CMAKE_SOURCE_DIR}/src/plugins/include"
        # The engine-side resolvers live in the engine's include tree;
        # the resolver header is header-only, so no engine link is needed.
        "${CMAKE_SOURCE_DIR}/src/engine/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    nimrtc_apply_options(${name} LENIENT)
    add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
    if(COMMAND gtest_discover_tests)
        gtest_discover_tests(${name}
            PROPERTIES TIMEOUT 30
            DISCOVERY_TIMEOUT 10)
    endif()
endfunction()

nimrtc_add_audio_seam_test(test_audio_seams test_audio_seams.cpp)

# NOTE: A real-msquic loopback integration test (test_web_transport_msquic.cpp)
# was drafted for v0.14.0-beta.1 (DC-1 land, Gate #2), but the bundled
# msquic v2.x header surface (QUIC_API_TABLE) lacks the symbols the full
# loopback test depends on (e.g. ConnectionAccept, QUIC_LISTENER_EVENT_LISTENING,
# QUIC_PARAM_LEVEL_LISTENER) — those landed in a later msquic API rev.
# The current test uses a MockWebTransportSession to verify the factory
# registration + public interface. A real-loopback msquic test (8 subtests)
# is deferred to the msquic upgrade in v1.x.

# ----------------------------------------------------------------------------
# v0.14.0-beta.1 (DC-1 land) — WebTransportMsQuicSessionFactory smoke test.
#
# Verifies the v0.14.0-beta.1 WebTransport/QUIC public factory seam:
#   1. Factory `id()` returns "msquic" and is registered with the
#      core::PluginRegistry under that id.
#   2. `factory->create(cfg)` returns a non-null IWebTransportSession.
#   3. A fresh session is in kConnecting; close() transitions to
#      kClosed and is idempotent.
#   4. open_bidi_stream() does not crash in any state.
#   5. list_web_transports() contains "msquic".
#
# The test uses ONLY the public factory seam (IWebTransportSessionFactory +
# IWebTransportSession) with a MockWebTransportSession — no direct .cpp include,
# no LoopbackMsQuicPeer, no Schannel cert provisioning.  Full QUIC loopback
# handshake validation is exercised end-to-end by the Chrome e2e Case E
# harness (interop/chrome/test_chrome_datachannel.html).
# ----------------------------------------------------------------------------
function(nimrtc_add_web_transport_msquic_test name source)
    add_executable(${name} ${source})
    if(WIN32)
        target_link_libraries(${name} PRIVATE ws2_32)
    endif()
    # Link the web_transport library so we get the factory + session
    # types via the public headers.  The test uses ONLY the
    # IWebTransportSessionFactory / IWebTransportSession public seam
    # (no direct .cpp include, which would cause multiple-definition
    # errors with the linked-in implementation).
    target_link_libraries(${name} PRIVATE
        nimrtc::web_transport
        nimrtc::core
        GTest::gtest_main)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${CMAKE_SOURCE_DIR}/src/plugins/include"
        "${CMAKE_SOURCE_DIR}/src/modules/web_transport/include")
    # The server seam needs the WebTransport TLS material (build directory).
    # Compiling the binary directory in lets the test probe for it no matter
    # which working directory ctest starts the case from.
    target_compile_definitions(${name} PRIVATE
        NIMRTC_DEFAULT_BUILD_ROOT="${CMAKE_BINARY_DIR}")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    nimrtc_apply_options(${name} LENIENT)
    add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>)
    set_tests_properties(${name} PROPERTIES TIMEOUT 60)
    # PRE_TEST discovery defers the .exe invocation (needed to enumerate
    # GTest cases) to ctest-time, after the POST_BUILD msquic.dll copy
    # in `nimrtc_copy_msquic_dll()` has run.  Without PRE_TEST, the
    # POST_BUILD gtest_discover_tests probe fails with STATUS_DLL_NOT_FOUND
    # (0xc0000135) on Windows.
    nimrtc_copy_msquic_dll(${name})
    if(COMMAND gtest_discover_tests)
        gtest_discover_tests(${name}
            PROPERTIES TIMEOUT 60
            DISCOVERY_TIMEOUT 10
            DISCOVERY_MODE PRE_TEST)
    endif()
endfunction()

nimrtc_add_web_transport_msquic_test(test_web_transport_msquic
                                    test_web_transport_msquic.cpp)

# ----------------------------------------------------------------------------
# v0.14.0-beta.2 (Case E / WT-SEVER) — WebTransport server seam test.
#
# Drives the msquic-backed IWebTransportServer through its public surface:
#   - factory produces a non-null server
#   - running() is false before start()
#   - start() on unprivileged port succeeds (listener binds)
#   - running() is true after start()
#   - stop() is idempotent; running() goes false
#   - restart after stop is safe
#   - start() when already running returns false (no double-start)
#   - set_on_session accepts lambdas; empty std::function unregisters
#
# Compile-time assertions verify IWebTransportServer is abstract and
# create_msquic_webtransport_server has the correct return type.
#
# Note: this test does NOT connect a real Chrome client — that is
# covered by tests/chrome_interop_harness/run_webtransport_harness.py.
# ----------------------------------------------------------------------------
nimrtc_add_web_transport_msquic_test(test_web_transport_server_seam
                                    test_web_transport_server_seam.cpp)

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
    nimrtc_apply_options(${name} LENIENT)
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
    nimrtc_apply_options(${name} LENIENT)
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
    nimrtc_apply_options(${name} LENIENT)
    add_test(NAME ${name}
        COMMAND $<TARGET_FILE:${name}>)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
    nimrtc_copy_wolfssl_certs(${name})
endfunction()
add_dtls_handshake_test(test_dtls_handshake_e2e test_dtls_handshake_e2e.cpp)

# ----------------------------------------------------------------------------
# DTLS ApplicationData (ct=23) loopback test (v0.14.x DataChannel wiring).
#
# Two in-process DtlsSession objects complete a real wolfSSL DTLS 1.2
# handshake, then exchange encrypted AppData in both directions via
# send_app_data() / take_outbound() -> feed_inbound() / take_app_data().
# Also verifies the pre-Connected kErrNotReady guard.
# ----------------------------------------------------------------------------
add_dtls_handshake_test(test_dtls_app_data_loopback test_dtls_app_data_loopback.cpp)

# ----------------------------------------------------------------------------
# ADR-013 (v0.12.0) — GMSSL TLCP DTLS 1.2 handshake round-trip test.
#
# OH-DOD-4 gate: verifies DtlsSessionGmSSL completes a full TLCP
# (GM/T 0044) handshake in-process, exports symmetric SRTP keying material,
# fires on_handshake_complete callbacks, and has zero error counts.
# Compiles ONLY when NIMRTC_ENABLE_DTLS_GMSSL=ON (default OFF).
#
# OH-DOD-5 (OHOS-aarch64 cross-compile) uses the same test binary.
# ----------------------------------------------------------------------------
if(NIMRTC_ENABLE_DTLS_GMSSL)
    function(add_dtls_gmssl_test name source)
        add_executable(${name} ${source})
        if(WIN32)
            target_link_libraries(${name} PRIVATE ws2_32)
        endif()
        target_link_libraries(${name} PRIVATE nimrtc::dtls_seam)
        nimrtc_link_gmssl(${name})
        target_include_directories(${name} PRIVATE
            "${CMAKE_SOURCE_DIR}/src/core/include"
            "${CMAKE_SOURCE_DIR}/src/modules/dtls/include")
        if(WIN32)
            target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
        endif()
        nimrtc_apply_options(${name} LENIENT)
        add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>)
        set_tests_properties(${name} PROPERTIES TIMEOUT 30)
        nimrtc_copy_wolfssl_certs(${name})
    endfunction()
    add_dtls_gmssl_test(test_dtls_gmssl_tlcp test_dtls_gmssl_tlcp.cpp)
endif()

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
# v1.1 Dtls3/D4: pump reentrancy + export/retransmit observability.
# D3: consecutive pump/tick/drain + reentrant pump inside
#     on_handshake_complete must not deadlock/crash (pumping_ guard).
# D4: export pre-handshake kErrNotReady, post-handshake kOk + non-zero
#     60B + client/server agree; retransmits monotonic observable.
# ----------------------------------------------------------------------------
add_dtls_handshake_test(test_dtls_pump_reentrancy
                        test_dtls_pump_reentrancy.cpp)
set_tests_properties(test_dtls_pump_reentrancy
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
        "${CMAKE_SOURCE_DIR}/src/modules/raw_udp/include"
        "${CMAKE_SOURCE_DIR}/src/modules/srtp/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    nimrtc_apply_options(${name} LENIENT)
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

# Diagnostic-only: replay a captured (or synthesised) Chrome DTLS
# ClientHello against a server-side DtlsSessionWolfSSL and print the
# outbound records the server produces.  Used to triage Case D
# interop; not part of the normal ctest acceptance gate.
add_executable(test_dtls_replay_chrome_hello
    test_dtls_replay_chrome_hello.cpp)
target_include_directories(test_dtls_replay_chrome_hello PRIVATE
    "${CMAKE_SOURCE_DIR}/src/core/include"
    "${CMAKE_SOURCE_DIR}/src/plugins/include"
    "${CMAKE_SOURCE_DIR}/src/modules/dtls/include"
    "${CMAKE_SOURCE_DIR}/src/modules/raw_udp/include"
    "${CMAKE_SOURCE_DIR}/src/modules/srtp/include")
target_link_libraries(test_dtls_replay_chrome_hello PRIVATE
    nimrtc::dtls_seam)
if(WIN32)
    target_compile_definitions(test_dtls_replay_chrome_hello PRIVATE
        _WIN32_WINNT=0x0601)
endif()
nimrtc_apply_options(test_dtls_replay_chrome_hello LENIENT)
nimrtc_copy_wolfssl_certs(test_dtls_replay_chrome_hello)
nimrtc_copy_wolfssl_certs(test_dtls_replay_chrome_hello)

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
        nimrtc::dtls
        # PAL Slice 2 (v0.10.2) — see nimrtc_add_engine_video_test above
        # for the rationale. Linking nimrtc::engine pulls in the
        # unified kDefaultRegistrars[] table which references
        # `nimrtc::datachannel::register_default_plugins`.
        nimrtc::datachannel)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${CMAKE_SOURCE_DIR}/src/plugins/include"
        "${CMAKE_SOURCE_DIR}/src/engine/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    target_compile_definitions(${name} PRIVATE NIMRTC_USE_LIBSRTP=1)
    nimrtc_apply_options(${name} LENIENT)
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
    nimrtc_apply_options(${name} LENIENT)
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
    nimrtc_apply_options(${name} LENIENT)
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
    nimrtc_apply_options(${name} LENIENT)
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
    elseif(APPLE)
        # macOS clang has built-in atomics in libc++; usrsctp uses
        # OSAtomicAdd32Barrier which requires linking against the C++ runtime.
        # Do not link -latomic - it does not exist on macOS.
        target_link_libraries(${name} PRIVATE c++ pthread)
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
        # Slice 8 (v0.10.2): the four new typed registry hooks
        # (DTLS / SCTP / raw_udp / transport-stack) require these
        # modules' register_default_plugins() symbols to be linked.
        # Without them, the PAL Slice 2 kDefaultRegistrars[] table
        # would have unresolved symbols and the unified entry point
        # would fail at link time.
        nimrtc::dtls_seam
        nimrtc::sctp
        nimrtc::raw_udp
        nimrtc::transport
        # TPAL-5 (v0.11.0): DataChannel plugin seam — links the
        # datachannel module's `register_default_plugins()` symbol
        # referenced by the PAL Slice 2 kDefaultRegistrars[] table.
        # P1: the symbol is a no-op stub; P2: Subagent A wires it to
        # register `SctpDataChannelFactory`.  Linking here keeps the
        # Slice 2 table additive — no future PR is required for tests.
        nimrtc::datachannel
        # v1.2.0 M1 (F2): the network-feedback collector module — its
        # register_default_plugins() symbol is referenced by the PAL
        # Slice 2 kDefaultRegistrars[] table, so the engine's
        # open() → register_all_default_plugins() chain resolves.
        nimrtc::net_feedback
        # v0.14.0 alpha.2 (ADR-014): the WebTransport / QUIC seam. The
        # engine test exercises the typed `register_web_transport()` /
        # `get_web_transport()` registry slots and the auto-registered
        # "msquic" factory, so it needs the seam's headers.  Linking it
        # here propagates the module's PUBLIC include path -- the core
        # layer no longer exports the optional-module include paths
        # (P1 hardening, 2026-11-09).
        nimrtc::web_transport
        # v1.2.0 (session capability plan N1 / T1): the session-model
        # module — its register_default_plugins() symbol populates the
        # registry's session slot, so the engine's
        # open() → register_all_default_plugins() chain resolves.
        nimrtc::session
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
    nimrtc_apply_options(${name} LENIENT)
    # See add_e2e_test() for the rationale on the COMMAND expression.
    add_test(NAME ${name}
        COMMAND $<TARGET_FILE:${name}>)
    # Engine tests that talk to a real usrsctp loopback can take up to
    # ~60 s on slow CI runners (two 30 s wait_for_established calls +
    # handshake jitter).  The default gtest TIMEOUT of 30 s is too tight
    # and causes spurious "Test #... (Timeout)" failures on GitHub-hosted
    # Windows runners; bump to 90 s to leave headroom without making
    # real failures take too long to surface.
    set_tests_properties(${name} PROPERTIES TIMEOUT 90)
    if(COMMAND gtest_discover_tests)
        gtest_discover_tests(${name}
            PROPERTIES TIMEOUT 90
            DISCOVERY_TIMEOUT 10)
    endif()
    # Linking nimrtc::engine pulls in nimrtc::dtls (via the unified
    # kDefaultRegistrars[] table), and the wolfSSL backend resolves its
    # certificate from disk at DTLS-session setup time.  Seed the certs
    # into the test output directories so any test that opens a real DTLS
    # session finds them.  See cmake/NimRTCTest.cmake for the rationale.
    nimrtc_copy_wolfssl_certs(${name})
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
    nimrtc_apply_options(${name} LENIENT)
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

# ----------------------------------------------------------------------------
# v1.1 Q3 — closed video feedback loop.
#
# Spans three modules:
#   - nimrtc::video_pipeline       : receiver loss detection + sender cache
#   - nimrtc::video_rtcp_feedback : NACK / PLI wire format
#
# The test drives: packet loss -> receiver feedback -> RTCP NACK ->
# sender retransmission -> re-delivery, and the PLI -> key-frame path.
# ----------------------------------------------------------------------------
function(nimrtc_add_video_feedback_test name source)
    add_executable(${name} ${source})
    if(WIN32)
        target_link_libraries(${name} PRIVATE ws2_32)
    endif()
    target_link_libraries(${name} PRIVATE
        nimrtc::video_pipeline
        nimrtc::video_rtcp_feedback
        nimrtc::video_frame
        nimrtc::video_jb
        nimrtc::rtp
        nimrtc::core
        GTest::gtest_main)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${CMAKE_SOURCE_DIR}/src/plugins/include"
        "${CMAKE_SOURCE_DIR}/src/modules/video_pipeline/include"
        "${CMAKE_SOURCE_DIR}/src/modules/video_rtcp_feedback/include"
        "${CMAKE_SOURCE_DIR}/src/modules/video_frame/include"
        "${CMAKE_SOURCE_DIR}/src/modules/video_jb/include"
        "${CMAKE_SOURCE_DIR}/src/modules/rtp/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    nimrtc_apply_options(${name} LENIENT)
    add_test(NAME ${name}
        COMMAND $<TARGET_FILE:${name}>)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
    if(COMMAND gtest_discover_tests)
        gtest_discover_tests(${name}
            PROPERTIES TIMEOUT 30
            DISCOVERY_TIMEOUT 10)
    endif()
endfunction()

if(NIMRTC_MODULE_VIDEO_PIPELINE AND NIMRTC_MODULE_VIDEO_RTCP_FEEDBACK)
nimrtc_add_video_feedback_test(test_video_feedback_loop
                                test_video_feedback_loop.cpp)
endif()

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
    nimrtc_apply_options(${name} LENIENT)
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
        # PAL Slice 2 / TPAL-5 (v0.11.0): `pal_default_registrars.cpp` holds
        # a reference to `&nimrtc::datachannel::register_default_plugins`,
        # so the test must link nimrtc::datachannel.lib for the symbol to
        # resolve. The same fix applies to every `nimrtc_add_*_test()`
        # function below that links nimrtc::engine (Slice 2 unified table).
        nimrtc::datachannel
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
    nimrtc_apply_options(${name} LENIENT)
    # See add_e2e_test() for the rationale on the COMMAND expression.
    add_test(NAME ${name}
        COMMAND $<TARGET_FILE:${name}>)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
    if(COMMAND gtest_discover_tests)
        gtest_discover_tests(${name}
            PROPERTIES TIMEOUT 30
            DISCOVERY_TIMEOUT 10)
    endif()
    # Links nimrtc::engine → nimrtc::dtls; the wolfSSL backend resolves
    # its certificate from disk at DTLS-session setup time.
    nimrtc_copy_wolfssl_certs(${name})
endfunction()

# (Function above is referenced only inside the video-source-gated block below;
# defined unconditionally because CMake does not allow function/endfunction
# inside an if()/endif() block per CMP0017.)

if(NIMRTC_MODULE_VIDEO_SOURCE AND NIMRTC_MODULE_VIDEO_SINK AND NIMRTC_MODULE_VIDEO_PIPELINE)
nimrtc_add_engine_video_test(test_engine_video_plugins
                              test_engine_video_plugins.cpp)
endif()

# ----------------------------------------------------------------------------
# v1.1 Q3 — engine-level RTCP feedback loop.
#
# The module-level loop lives in `test_video_feedback_loop.cpp` and the
# plugin adapters in `test_video_plugin_adapters.cpp`.  This target covers
# the *engine* half: the receiver's loss report -> engine -> RTCP bytes ->
# ICE, and the peer's NACK / PLI -> engine -> sender / encoder.
#
# The engine is driven through a fake ICE transport registered under the id
# "fake-ice" (see the test source), so no real networking is involved.
# ----------------------------------------------------------------------------
function(nimrtc_add_engine_video_feedback_test name source)
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
        # PAL Slice 2 (v0.10.2): the unified registrar table references
        # `nimrtc::datachannel::register_default_plugins`, so the symbol
        # must resolve here too (same rationale as the function above).
        nimrtc::datachannel
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
    nimrtc_apply_options(${name} LENIENT)
    add_test(NAME ${name}
        COMMAND $<TARGET_FILE:${name}>)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
    if(COMMAND gtest_discover_tests)
        gtest_discover_tests(${name}
            PROPERTIES TIMEOUT 30
            DISCOVERY_TIMEOUT 10)
    endif()
    # Links nimrtc::engine → nimrtc::dtls; the wolfSSL backend resolves
    # its certificate from disk at DTLS-session setup time.
    nimrtc_copy_wolfssl_certs(${name})
endfunction()

if(NIMRTC_MODULE_VIDEO_SOURCE AND NIMRTC_MODULE_VIDEO_SINK AND NIMRTC_MODULE_VIDEO_PIPELINE)
nimrtc_add_engine_video_feedback_test(test_engine_video_feedback
                                       test_engine_video_feedback.cpp)
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
        # PAL Slice 2 (v0.10.2) — see nimrtc_add_engine_video_test above for
        # the rationale. The unified kDefaultRegistrars[] table references
        # every module's `register_default_plugins()` symbol, so any test
        # linking `nimrtc::engine` must also link every module whose
        # registrar is in the table — datachannel is one of those.
        nimrtc::datachannel
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
    nimrtc_apply_options(${name} LENIENT)
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
    nimrtc_apply_options(${name} LENIENT)
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

# B6 (remediation plan §2.4) — the video codec family contract.
# `IVideoCodec::kind()` is authoritative; the plugin id is only a
# lookup hint.  The test needs the video pipeline (receiver / sender
# "reference") and the H.264 module (the default codec), so it reuses
# `nimrtc_add_engine_codec_test`'s link set.
if(NIMRTC_MODULE_VIDEO_SOURCE AND NIMRTC_MODULE_VIDEO_SINK AND NIMRTC_MODULE_VIDEO_PIPELINE
   AND NIMRTC_MODULE_H264)
nimrtc_add_engine_codec_test(test_engine_video_codec_family
                              test_engine_video_codec_family.cpp)
endif()

# Phase 3 / C3 — the GPU zero-copy video send path.  The test needs the
# video pipeline (receiver / sender "reference") so it reuses
# `nimrtc_add_engine_codec_test`'s link set.
if(NIMRTC_MODULE_VIDEO_SOURCE AND NIMRTC_MODULE_VIDEO_SINK AND NIMRTC_MODULE_VIDEO_PIPELINE
   AND NIMRTC_MODULE_H264)
nimrtc_add_engine_codec_test(test_engine_video_zero_copy
                              test_engine_video_zero_copy.cpp)
endif()

# ----------------------------------------------------------------------------
# The capture-*device* seam wiring (remote-desktop plan A1; architecture
# remediation plan Phase 2 §2.2).
#
# `EngineConfig::video_capture_name` was the one `*_name` field the engine
# never read — naming a capture device did nothing.  The test proves the
# id is resolved, the device is created and opened, an unknown id is
# reported loudly, and the device is closed on teardown.
#
# It needs the capture module's registrar (the PAL Slice 2 default-
# registrar table references `video_capture::register_default_plugins`),
# so the link set is the plain `nimrtc_add_engine_test` one plus the
# capture module.
if(NIMRTC_MODULE_VIDEO_CAPTURE)
  add_executable(test_engine_video_capture test_engine_video_capture.cpp)
  if(WIN32)
    target_link_libraries(test_engine_video_capture PRIVATE ws2_32)
  elseif(APPLE)
    target_link_libraries(test_engine_video_capture PRIVATE c++ pthread)
  else()
    target_link_libraries(test_engine_video_capture PRIVATE atomic)
  endif()
  target_link_libraries(test_engine_video_capture PRIVATE
      nimrtc::engine
      nimrtc::ice
      nimrtc::rtp
      nimrtc::sdp
      nimrtc::jb
      nimrtc::audio3a
      nimrtc::dtls_seam
      nimrtc::sctp
      nimrtc::raw_udp
      nimrtc::transport
      nimrtc::datachannel
      nimrtc::net_feedback
      nimrtc::web_transport
      nimrtc::session
      nimrtc::video_capture
      GTest::gtest_main)
  target_include_directories(test_engine_video_capture PRIVATE
      "${CMAKE_SOURCE_DIR}/src/core/include"
      "${CMAKE_SOURCE_DIR}/src/plugins/include"
      "${CMAKE_SOURCE_DIR}/src/engine/include")
  if(WIN32)
    target_compile_definitions(test_engine_video_capture PRIVATE _WIN32_WINNT=0x0601)
  endif()
  set_target_properties(test_engine_video_capture PROPERTIES
      VS_LINK_LIBRARY_DEPENDENCIES TRUE)
  nimrtc_apply_options(test_engine_video_capture LENIENT)
  add_test(NAME test_engine_video_capture
      COMMAND $<TARGET_FILE:test_engine_video_capture>)
  set_tests_properties(test_engine_video_capture PROPERTIES TIMEOUT 90)
  if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_engine_video_capture
        PROPERTIES TIMEOUT 90
        DISCOVERY_TIMEOUT 10)
  endif()
endif()

# R2.5 validation: engine consumes PluginRegistry for plugin slots.
nimrtc_add_engine_test(test_engine_plugin_loading test_engine_plugin_loading.cpp)

# ----------------------------------------------------------------------------
# Architecture-remediation Phase 0.2 — registry consistency guard.
#
# The same module set as `nimrtc_add_engine_test` is required, because
# `core::register_all_default_plugins()` walks the unified
# `kDefaultRegistrars[]` table and every entry in that table is an
# unresolved external until its module is linked (the LNK2001 rationale
# documented in `nimrtc_add_engine_test`).
#
# `nimrtc::transport` is additionally linked for the `StackConfig`
# defaults and the `ITransportStackFactory` the guard drives.
# ----------------------------------------------------------------------------
add_executable(test_registry_contract test_registry_contract.cpp)
if(WIN32)
    target_link_libraries(test_registry_contract PRIVATE ws2_32)
elseif(APPLE)
    target_link_libraries(test_registry_contract PRIVATE c++ pthread)
else()
    target_link_libraries(test_registry_contract PRIVATE atomic)
endif()
target_link_libraries(test_registry_contract PRIVATE
    # LINK-ORDER CRITICAL (GNU ld single-pass archive scan).
    #
    # `nimrtc_core_objects` holds `pal_default_registrars.cpp.o`, i.e. the
    # unified `kDefaultRegistrars[]` table.  Every entry of that table is an
    # unresolved external until the owning module's archive is scanned, so
    # the table's object file MUST be pulled into the link BEFORE the
    # module archives -- otherwise GNU ld has already walked past
    # `libnimrtc_audio3a.a` & friends when the table introduces
    # `nimrtc::audio3a::register_default_plugins()` and the link fails with
    # "undefined reference" (the LNK2001 rationale documented in
    # `nimrtc_add_engine_test`).
    #
    # `nimrtc::core` cannot provide that ordering on its own: every module
    # depends on `nimrtc::core`, so CMake always emits core's artefacts
    # (including `libnimrtc_registry.a`) at the END of the link line.  The
    # engine tests dodge this because `nimrtc::engine` PUBLICly links
    # `nimrtc_core_objects` and therefore absorbs the table's object file
    # into `libnimrtc_engine.a`, which lands FIRST on the link line.
    #
    # Linking the OBJECT library directly gives this test the same shape
    # without dragging the whole engine into it.
    nimrtc_core_objects
    nimrtc::core
    nimrtc::transport
    nimrtc::ice
    nimrtc::rtp
    nimrtc::sdp
    nimrtc::jb
    nimrtc::audio3a
    nimrtc::bwe
    nimrtc::sched
    nimrtc::dtls_seam
    nimrtc::sctp
    nimrtc::raw_udp
    nimrtc::datachannel
    nimrtc::net_feedback
    nimrtc::web_transport
    nimrtc::session
    GTest::gtest_main)
# The remaining modules from the unified `kDefaultRegistrars[]` table.
# They are linked here (rather than through a helper) because the set is
# build-configuration dependent: several of them are gated behind
# `NIMRTC_MODULE_*` options and their `register_default_plugins()`
# symbol only exists when the module is compiled in.
foreach(_mod opus h264 h265 av1 video_source video_sink video_pipeline
              video_capture audio_rx dtmf srtp)
    if(TARGET nimrtc::${_mod})
        target_link_libraries(test_registry_contract PRIVATE nimrtc::${_mod})
    endif()
endforeach()
target_include_directories(test_registry_contract PRIVATE
    "${CMAKE_SOURCE_DIR}/src/core/include"
    "${CMAKE_SOURCE_DIR}/src/plugins/include"
    "${CMAKE_SOURCE_DIR}/src/modules/transport/include"
    "${CMAKE_SOURCE_DIR}/src/modules/dtls/include"
    "${CMAKE_SOURCE_DIR}/src/modules/sctp/include"
    "${CMAKE_SOURCE_DIR}/src/modules/raw_udp/include")
if(WIN32)
    target_compile_definitions(test_registry_contract PRIVATE _WIN32_WINNT=0x0601)
endif()
set_target_properties(test_registry_contract PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)
nimrtc_apply_options(test_registry_contract LENIENT)
add_test(NAME test_registry_contract
    COMMAND $<TARGET_FILE:test_registry_contract>)
set_tests_properties(test_registry_contract PROPERTIES TIMEOUT 30)
# The target links `nimrtc::web_transport`, whose msquic dependency is a
# runtime DLL on Windows.  Without it the exe fails to load (0xc0000135),
# which `gtest_discover_tests` reports as a build failure.
nimrtc_copy_msquic_dll(test_registry_contract)
# The target links the DTLS seam (and thus the wolfSSL backend), which
# resolves its certificate from disk at DTLS-session setup time.
nimrtc_copy_wolfssl_certs(test_registry_contract)
if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_registry_contract
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 10
        DISCOVERY_MODE PRE_TEST)
endif()

# ---------------------------------------------------------------------------
# TPAL-4 cleanup (v0.11.0) — DTLS seam engine-routing regression test.
#
# Verifies that engine.cpp routes DTLS through the PAL Slice 4 seam
# (IDtlsSessionFactory) via `core::PluginRegistry::get_dtls_session(id)`:
#   1. `cfg.dtls_name == ""` resolves to the built-in wolfSSL factory
#      at id="wolfssl" — source-compat for every v0.10.x caller.
#   2. `cfg.dtls_name = "mock"` resolves to a user-registered
#      MockDtlsSessionFactory and the mock's create()/open() are
#      invoked by the engine (asserted via atomic counters).
#   3. An un-registered `cfg.dtls_name` produces an open() failure
#      with kEngineInternal — no silent fallback to "wolfssl".
#   4. Re-registering under an existing id is last-writer-wins; new
#      ids add monotonically.  Verified through the typed registry
#      slot, the same one the engine uses.
#
# The seam is exercised end-to-end through `MockDtlsSession` (a fresh
# `IDtlsSession` implementation defined in this file).  No real
# wolfSSL handshake is required, so the test stays in the
# sub-second range.
# ---------------------------------------------------------------------------
nimrtc_add_engine_test(test_dtls_seam test_dtls_seam.cpp)

# ---------------------------------------------------------------------------
# TPAL-5 (v0.11.0) — usrsctp opt-in regression test.
#
# v0.11.0 cut: `nimrtc::sctp::register_default_plugins()` no longer
# auto-registers `UsrsctpSocketFactory` (id="usrsctp"). The class still
# compiles and links (the usrsctp sources are kept for opt-in callers);
# this test exercises the opt-in path:
#
# Drives two UsrsctpSocket instances (AF_CONN loopback encapsulation)
# to verify:
#   - send_datagram enqueues into usrsctp's send queue (returns kOk)
#   - on_recv callback fires on usrsctp's worker thread, payloads
#     land in B's recv queue under a mutex/condvar
#   - send_partial_reliable(ttl=10ms) call site compiles + returns kOk
#   - factory slot "usrsctp" is NOT registered (v0.11.0 contract)
#   - factory slot "stub" is still registered (Slice 5 regression)
#
# Links nimrtc::sctp, nimrtc::vendor::usrsctp transitively.
# See docs/plan/v0.11-plan.md §2.2 (TPAL-5 deferred to v1.x).
# ---------------------------------------------------------------------------
nimrtc_add_engine_test(test_sctp_usrsctp test_sctp_usrsctp.cpp)

# ---------------------------------------------------------------------------
# P2 PCM tap test (audio3a pre/post 3A tap, §8.7).
# Verifies round-trip: IAudio3A instance, install counting tap, feed 100 frames,
# assert tap invocation count.
# ---------------------------------------------------------------------------
nimrtc_add_engine_test(test_audio3a_tap plugins/test_audio3a_tap.cpp)

# ---------------------------------------------------------------------------
# TPAL-5 (v0.11.0) — DataChannel plugin engine-routing regression test.
#
# Verifies that the engine:
#   - Resolves the datachannel plugin via the typed
#     `core::PluginRegistry::get_datachannel(id)` slot (Slice 8 contract).
#   - Returns nullptr from create_data_channel() for unknown factory ids
#     (no silent fallback, matches the DTLS seam from test_dtls_seam).
#   - Forwards the user-supplied set_on_data_message() /
#     set_on_data_state() callbacks to the IDataChannel before its open().
#   - Honours `cfg.datachannel_name = ""` as an explicit disable.
#
# Uses the same `nimrtc_add_engine_test` wiring as test_dtls_seam and
# test_sctp_usrsctp — links the engine + every module that has a
# `register_default_plugins()` so the Slice 8 typed slots are populated
# end-to-end.  No network/ICE/DTLS interaction (the test stops at
# create_offer() / create_data_channel() / close()).
# ---------------------------------------------------------------------------
nimrtc_add_engine_test(test_datachannel_engine test_datachannel_engine.cpp)
# DataChannel engine test waits on ICE/DTLS handshake (~60s) for the
# `DisabledDatachannelNameReturnsNull` case; the per-test default
# TIMEOUT 30 in `nimrtc_add_engine_test()` is too tight.  Bump it
# specifically for this target.
set_tests_properties(test_datachannel_engine PROPERTIES TIMEOUT 120)

# ---------------------------------------------------------------------------
# ADR-017 — engine-side cross-end timeline wiring.
#
# Covers the layer *above* `nimrtc::rtp::SenderReportTracker` (whose own
# arithmetic is covered by `src/modules/rtp/tests/test_timeline.cpp`):
# the feature gate, the SDP `a=extmap` negotiation, and the
# "no Sender Report ⇒ no capture timestamp" invariant.  Uses the "shell"
# transport strategy so no ICE/DTLS handshake is involved; the same
# wiring as `test_datachannel_engine` above.
# ---------------------------------------------------------------------------
nimrtc_add_engine_test(test_engine_timeline test_engine_timeline.cpp)

# ---------------------------------------------------------------------------
# v1.2.0 M1 (F1 / F6) — the engine's side of the bandwidth loop.
#
# Covers the wiring between the RTCP demux, the net-feedback collector
# and the bandwidth estimator: an inbound Receiver Report reaches the
# collector, the aggregated snapshot reaches `IBwe::on_feedback()`, and
# the bandwidth estimate *moves* under loss.  Before v1.2.0 nothing ever
# fed the estimator, so the target stayed pinned at its initial value.
# Uses the "shell" transport strategy, like `test_engine_timeline` above.
# ---------------------------------------------------------------------------
nimrtc_add_engine_test(test_engine_net_feedback test_engine_net_feedback.cpp)

# ---------------------------------------------------------------------------
# v1.2.0 (session capability plan X1) — the engine's side of the RTCP
# Extended Report path.
#
# Covers the wiring between the RTCP demux and the XR consumer seam: an
# inbound XR packet (PT 207) reaches the consumer, the decoded RFC 3611
# report blocks are readable by the host, and a non-XR RTCP packet never
# reaches it.  Before v1.2.0 the XR sub-packet was recognised by the
# demultiplexer and dropped.
#
# Shell transport strategy, no ICE/DTLS handshake involved.
# ---------------------------------------------------------------------------
nimrtc_add_engine_test(test_engine_xr_report test_engine_xr_report.cpp)

# ---------------------------------------------------------------------------
# v1.2.0 (session capability plan X2 / X4 / X5) — the engine's side of the
# RTCP XR / TMMBR plane.
#
# Covers the wiring the wire-format tests cannot:
#   - the offer advertises `a=rtcp-fb:<pt> ccm tmmbr` and
#     `a=rtcp-xr:rcvr-rtt=all stat-summary=loss,jitt` (and does not, when the
#     features are off);
#   - the answer echoes the negotiated subset of both (RFC 3264 §6.1,
#     RFC 3611 §5.2, RFC 5104 §7.2);
#   - an inbound TMMBR clamps the peer's limit, the tightest limit wins,
#     and a tuple about a foreign media source is ignored;
#   - a malformed TMMBR is dropped, not fatal.
#
# Shell transport strategy, no ICE/DTLS handshake involved.
# ---------------------------------------------------------------------------
nimrtc_add_engine_test(test_engine_tmmbr test_engine_tmmbr.cpp)

# ---------------------------------------------------------------------------
# v1.2.0 (session capability plan X2 / X4 / X5) — the *outbound* RTCP XR /
# TMMBR plane.
#
# Covers the part the wire-format tests cannot: that the engine actually
# puts the packets on the wire and that they carry the right numbers —
#   - the XR report (PT 207) carries the Receiver Reference Time block
#     (BT = 4) and the DLRR block (BT = 5) that answers the peer's own
#     report, and only when the peer named the format (RFC 3611 §5.1);
#   - the Statistics Summary block (BT = 6) reports the inbound audio
#     stream and only the field indicators the peer asked for;
#   - the TMMBR (RTPFB, FMT = 3) carries the live bandwidth estimate and
#     is silent unless "ccm tmmbr" was negotiated (RFC 5104 §7.2);
#   - `disable_tmmbr` vetoes the outbound TMMBR;
#   - an inbound TMMBR is answered with exactly one TMMBN (RTPFB, FMT = 4)
#     whose FCI entry names the owner of the bounding tuple.
#
# Driven through a fake ICE transport (the "fake-ice" pattern), so the
# engine's periodic work runs and the outbound datagrams are observable.
# ---------------------------------------------------------------------------
nimrtc_add_engine_test(test_engine_xr_tmmbr test_engine_xr_tmmbr.cpp)

# ---------------------------------------------------------------------------
# v1.2.0 (audio pipeline, WP-6) — negotiation, payload-type routing and
# SDP completion.
#
# Covers the three things WP-6 changed in the engine:
#   - the offer carries the completed audio m-line (a=ptime / a=maxptime,
#     the Opus a=fmtp, the ssrc-audio-level a=extmap and the per-engine
#     SSRC) — B-14;
#   - a payload type the peer never declared is counted and dropped with
# Video pipeline v2 — C9 / C11: the video payload type is the *negotiated* one.
#
# Covers the two defects the negotiation work closes:
#   - C9: the answer copied the offer's `a=rtpmap:` / `a=fmtp:` lines
#     verbatim, which told the peer we would decode every codec it offered
#     — including the ones NimRTC has no decoder for.  The answer now names
#     exactly the negotiated payload type, and an undecodable codec rejects
#     the media section (port 0) and reports kVideoCodecUnsupported once;
#   - C11: the packet path demultiplexed inbound RTP by a *locally
#     configured* payload type, so a payload type the peer offered but that
#     was never negotiated still reached the video pipeline.  Only the
#     negotiated payload type reaches the receiver now.
#
# The offer side is covered too: the offer names the SENDER's payload type,
# because that is the number the outbound packet path stamps.
#
# Uses the "shell" transport strategy, like `test_engine_audio_state` above.
# ---------------------------------------------------------------------------
if(NIMRTC_MODULE_VIDEO_PIPELINE AND NIMRTC_MODULE_VIDEO_SOURCE
   AND NIMRTC_MODULE_VIDEO_SINK AND NIMRTC_MODULE_H264)
    nimrtc_add_engine_test(test_video_negotiation test_video_negotiation.cpp)
endif()

# ---------------------------------------------------------------------------
#     kAudioPayloadTypeUnknown instead of being decoded — B-4 (defect F-8);
#   - a peer that offers a codec we cannot decode produces
#     kAudioCodecUnsupported exactly once and the audio m-line is rejected
#     — A-9 / spec Part D.
#
# Shell transport strategy, no ICE/DTLS handshake involved.
# ---------------------------------------------------------------------------
nimrtc_add_engine_test(test_engine_audio_state test_engine_audio_state.cpp)

# ---------------------------------------------------------------------------
# v1.2.0 audio pipeline WP-8 — the RTCP receiver report for the inbound
# audio stream and the audio statistics counters (spec B-12 / B-13).
#
# Shell transport strategy, no ICE/DTLS handshake involved.
# ---------------------------------------------------------------------------
nimrtc_add_engine_test(test_engine_audio_stats test_engine_audio_stats.cpp)

# ---------------------------------------------------------------------------
# v1.2.0 audio pipeline WP-9 — the cross-end capture timestamp of an
# inbound audio frame (spec B-17).
#
# Shell transport strategy, no ICE/DTLS handshake involved.
# ---------------------------------------------------------------------------
nimrtc_add_engine_test(test_engine_capture_timestamp test_engine_capture_timestamp.cpp)

# ---------------------------------------------------------------------------
# v1.2.0 audio pipeline WP-11 -- the engine integration tests (spec V-2):
# the media loopback.
#
#   - loss injection: every N-th packet is dropped; the output cadence
#     stays constant and the concealed-frame counter matches the drop
#     count (spec A-3 / A-4);
#   - in-band FEC: the same drops are reconstructed instead of concealed
#     (spec B-8);
#   - tick-rate independence: 30 Hz and 200 Hz produce the same audio
#     (spec A-1 / O-4);
#   - the real loopback: two engines over 127.0.0.1, engine A sends PCM
#     and engine B decodes it (the direction that used to hand the host
#     the Opus bit-stream instead of PCM -- defect F-1).
#
# The loopback case needs a real UDP bind; when the host forbids it the
# case skips instead of failing (a port restriction is not a media-path
# defect).
# ---------------------------------------------------------------------------
nimrtc_add_engine_test(test_engine_audio_loopback test_engine_audio_loopback.cpp)

# ---------------------------------------------------------------------------
# v1.2.0 audio pipeline WP-11 -- the send path's two promises (spec V-1):
#
#   - the framing is the engine's business: `send_audio()` accepts any
#     block size and emits exactly one RTP packet per 20 ms frame with
#     a continuous media timestamp (spec B-6, A-7);
#   - no silent loss: an empty buffer, an invalid frame size or an
#     encoder error increments the statistics counter and fires
#     `on_error` (spec A-8).
#
# The engine is driven through a fake ICE transport and a fake DTLS
# session (both registered as plugins), so the send path is reachable
# without a peer.
# ---------------------------------------------------------------------------
nimrtc_add_engine_test(test_engine_audio_send test_engine_audio_send.cpp)

# ---------------------------------------------------------------------------
# The audio send path's allocation budget (remediation Phase 3 §3.5).
#
# The plan asks for "音频路径 per-packet 堆分配次数 = 0".  The metric is
# per *packet*, not per call: the host may hand the engine an arbitrary
# block size and the engine accumulates samples until it has a whole
# 20 ms frame.
#
# The measurement needs the global operator new replaced, which is only
# safe in a binary that does not link a sanitizer runtime.  The video
# side of the same criterion lives in the `hotpath_zero_allocs` ctest
# case (the benchmark binary owns that probe), so the two probes never
# meet in one process.
# ---------------------------------------------------------------------------
nimrtc_add_engine_test(test_engine_audio_alloc test_engine_audio_alloc.cpp)

# ---------------------------------------------------------------------------
# The BWE-linked audio bitrate adaptation.
#
# The engine lowers the audio encoder's bitrate when the bandwidth
# estimate collapses (never below the configured floor) and raises it
# back towards the configured ceiling once the link recovers.  The test
# drives the engine through a fake BWE whose estimate it controls, plus
# a fake codec that records every bitrate the engine pushes into it.
# ---------------------------------------------------------------------------
nimrtc_add_engine_test(test_engine_audio_bitrate test_engine_audio_bitrate.cpp)

# ---------------------------------------------------------------------------
# v1.1 T1/T2 — engine-side ref_frame <-> capture-ts map.
#
# Covers OUR OWN frames (send_video auto-record + explicit
# record_video_ref_frame), both lookup directions, the T2 age query, and
# the 1024-entry ring eviction. Shell strategy, no ICE/DTLS handshake.
# ---------------------------------------------------------------------------
nimrtc_add_engine_test(test_engine_ref_frame test_engine_ref_frame.cpp)

# ---------------------------------------------------------------------------
# v1.2.0 (session capability plan) — the engine's renegotiation surface.
#
# N1 — the engine resolves a pluggable session model through the
#      registry's session slot and drives it only through
#      `plugins::ISession` (no concrete module include in `src/engine/`).
# N2 — `needs_negotiation()` flips on a mutation and clears on the offer.
# N3 — a host-added track grows the media list; an existing mid never moves.
# N4 — a track's direction lands on the m-line.
# N5 / OD-A — a removed track keeps its m-line, marked `inactive`.
# T2 / T3 — a multi-m-line offer is answered one-to-one, per mid.
#
# Shell transport strategy, no ICE/DTLS handshake involved.
# ---------------------------------------------------------------------------
nimrtc_add_engine_test(test_engine_session test_engine_session.cpp)

# ---------------------------------------------------------------------------
# v1.2.0 (session capability plan) — D3 / D4: the RFC 4733 SDP declaration
# and the unknown-payload-type counting semantics.
#
# D3 — a default engine offers no telephone-event line (OD-B), an engine
#      that was asked for DTMF declares `a=rtpmap:<pt> telephone-event/8000`
#      and `a=fmtp:<pt> 0-16` exactly once, and the answer carries the
#      *negotiated* event codes rather than a blind echo of the offer.
# D4 — a negotiated telephone-event payload is decoded and reported, and is
#      NOT counted as an unknown audio payload type; an unnegotiated one
#      still counts as unknown.
#
# The DTMF codec's own wire format is covered by
# `src/modules/dtmf/tests/test_dtmf_codec.cpp`.
#
# Shell transport strategy, no ICE/DTLS handshake involved.
# ---------------------------------------------------------------------------
nimrtc_add_engine_test(test_engine_dtmf test_engine_dtmf.cpp)

# ---------------------------------------------------------------------------
# Fuzz targets + corpus replay driver (P2 hardening, 2026-11-09).
#
# The three `run_*()` entry points (SDP / RTP / video payload) live in
# `tests/fuzz/`.  They are driven two ways:
#
#   - `test_fuzz_replay` replays the committed seed corpus plus a
#     deterministic mutation set.  It runs on every platform and on
#     every commit, so it is the regression floor under the fuzzing
#     effort: a bug libFuzzer finds becomes a seed, and the seed keeps
#     being replayed.
#   - `fuzz_sdp` / `fuzz_rtp` / `fuzz_video_payload` are libFuzzer
#     harnesses, built only with `-DNIMRTC_FUZZING=ON` and a Clang
#     toolchain (see tests/fuzz/CMakeLists.txt).
#
# The DTLS and STUN parsers are deliberately absent: they belong to
# the vendored wolfSSL / libjuice, which carry their own upstream
# fuzzing.  Re-fuzzing them from here would test the vendored code,
# not ours.
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/CMakeLists.txt")
    add_subdirectory(fuzz)
endif()

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
    nimrtc_apply_options(test_nvenc_probe LENIENT)
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
    nimrtc_apply_options(test_nvenc_abi_probe LENIENT)
    # Not registered with ctest — run manually as: cmake --build --target test_nvenc_abi_probe && build/tests/Debug/test_nvenc_abi_probe.exe
endif()
# ---------------------------------------------------------------------------
# v1.2.0 (video plan F8) - the per-stream bandwidth share rule.
#
# The rule is an engine-private, header-only pure function
# (`src/engine/src/bandwidth_allocation.hpp`).  It is unit tested
# directly - no engine, no transport, no clock - because that is the
# shape the rule has: a pure function of (estimate, demand list).
#
# The test therefore compiles the engine's *private* header directory
# into its include path.  That directory is not installed and is not on
# the public surface (constraint L4); adding it here does not make it
# public, it only lets the unit test see the function.
# ---------------------------------------------------------------------------
add_executable(test_bandwidth_allocation test_bandwidth_allocation.cpp)
target_link_libraries(test_bandwidth_allocation PRIVATE
    nimrtc::core
    GTest::gtest_main)
target_include_directories(test_bandwidth_allocation PRIVATE
    "${CMAKE_SOURCE_DIR}/src/core/include"
    "${CMAKE_SOURCE_DIR}/src/engine/src")
if(WIN32)
    target_compile_definitions(test_bandwidth_allocation PRIVATE _WIN32_WINNT=0x0601)
endif()
nimrtc_apply_options(test_bandwidth_allocation LENIENT)
add_test(NAME test_bandwidth_allocation
    COMMAND $<TARGET_FILE:test_bandwidth_allocation>)
set_tests_properties(test_bandwidth_allocation PROPERTIES TIMEOUT 30)
if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_bandwidth_allocation
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 10)
endif()

# ---------------------------------------------------------------------------
# Architecture-remediation Phase 0.3 — the hot-path performance baseline.
#
# Phase 3 ("消除热路径上的分配与拷贝") needs a number to compare against,
# and §0.3 asks for three of them: `send_video()` at 1080p30 (per-call
# heap allocations + CPU), `tick()` (p99), and the scheduler under load
# (`drain_with()` vs. a concurrent `enqueue()`).
#
# `bench_engine_hotpath` produces those three as a JSON document on
# stdout.  It is deliberately NOT a ctest case: the numbers are
# machine-dependent, so a threshold assertion would be a flaky test
# rather than a measurement.  `tools/benchmark_hotpath.py` is the
# wrapper that captures a run into `build/` and compares two of them.
#
# The link set is the engine one (the same modules
# `nimrtc_add_engine_test` links) plus the scheduler module, because the
# benchmark drives the "strict_priority" scheduler directly through the
# registry.
# ---------------------------------------------------------------------------
add_executable(bench_engine_hotpath bench_engine_hotpath.cpp)
if(WIN32)
    target_link_libraries(bench_engine_hotpath PRIVATE ws2_32)
elseif(APPLE)
    target_link_libraries(bench_engine_hotpath PRIVATE c++ pthread)
else()
    target_link_libraries(bench_engine_hotpath PRIVATE atomic)
endif()
target_link_libraries(bench_engine_hotpath PRIVATE
    nimrtc::engine
    nimrtc::core
    nimrtc::plugins
    nimrtc::ice
    nimrtc::rtp
    nimrtc::sdp
    nimrtc::jb
    nimrtc::audio3a
    nimrtc::dtls_seam
    nimrtc::sctp
    nimrtc::raw_udp
    nimrtc::transport
    nimrtc::datachannel
    nimrtc::net_feedback
    nimrtc::web_transport
    nimrtc::session
    nimrtc::sched
    nimrtc::h264
    nimrtc::video_source
    nimrtc::video_sink
    nimrtc::video_pipeline)
target_include_directories(bench_engine_hotpath PRIVATE
    "${CMAKE_SOURCE_DIR}/src/core/include"
    "${CMAKE_SOURCE_DIR}/src/plugins/include"
    "${CMAKE_SOURCE_DIR}/src/engine/include")
if(WIN32)
    target_compile_definitions(bench_engine_hotpath PRIVATE _WIN32_WINNT=0x0601)
endif()
set_target_properties(bench_engine_hotpath PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)
nimrtc_apply_options(bench_engine_hotpath LENIENT)
# The wolfSSL certs are copied because the engine links the DTLS seam.
nimrtc_copy_wolfssl_certs(bench_engine_hotpath)

# ---------------------------------------------------------------------------
# The zero-allocation regression gate.
#
# The benchmark itself is NOT a ctest case (see the comment above): the
# timing numbers are machine-dependent.  The *allocation* count is not —
# it is exact, and the engine runs no background threads in the "shell"
# profile, so every allocation counted during the measured region belongs
# to the `send_video()` call itself.
#
# Architecture-remediation Phase 3 §3.5 requires `send_video()` at
# 1080p30 to perform **zero** heap allocations per call.  This case turns
# that requirement into a gate: `--assert-zero-allocs` makes the process
# exit non-zero when the count is not zero.
#
# The frame count is small on purpose: 300 frames at 1080p is a couple of
# seconds, and the allocation count does not depend on how many frames are
# measured.
# ---------------------------------------------------------------------------
if(NIMRTC_BUILD_TESTS)
    add_test(NAME hotpath_zero_allocs
             COMMAND bench_engine_hotpath
                     --width 1920 --height 1080
                     --frames 300 --warmup 60
                     --assert-zero-allocs)
    set_tests_properties(hotpath_zero_allocs PROPERTIES
        TIMEOUT 300
        LABELS "performance;hotpath")
endif()

