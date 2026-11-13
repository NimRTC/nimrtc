# modules/dtls/tests/CMakeLists.txt
# =============================================================================
# DTLS module unit tests.
#
# After the v1.2 directory unification (architecture remediation plan
# §4.1 / D3) this directory owns BOTH test suites that used to be split
# across two trees:
#
#   src/modules/dtls/tests/  — the handshake-reassembly shim test
#                               (was: src/modules/dtls/tests/)
#   src/dtls/tests/          — the seam factory tests
#                               (was: src/dtls/tests/)
#
# They are merged here so the DTLS module has exactly ONE test root.
#
# As of the wolfSSL-only migration, all DtlsSession tests live here via
# the public DtlsSession class (which delegates to DtlsSessionWolfSSL).
# The pre-migration BCrypt-direct tests have been folded into this
# directory; nothing under tests/ at the project root is BCrypt-specific
# any more — the Windows-only sanity-check files (test_dtls_gcm_aead.cpp /
# test_dtls_inproc.cpp / test_dtls_x25519.cpp) are governed by
# tests/CMakeLists.txt at the repo root and gated on _WIN32.
# =============================================================================

# -----------------------------------------------------------------------------
# PAL Slice 4 seam tests — verify the factory pattern
# (create() returns IDtlsSession, id() matches "wolfssl") and the
# register_default_plugins() idempotency latch.
#
# These tests are intentionally NOT linked against the full wolfSSL
# crypto stack so they can run in seconds.  Real handshake behaviour
# (e2e Case D, Chrome interop) is covered by the existing
# tests/test_dtls_handshake_e2e.cpp and the per-module tests in
# src/modules/dtls/tests/.
# -----------------------------------------------------------------------------
function(nimrtc_add_dtls_seam_test name source)
    add_executable(${name} ${source})
    if(WIN32)
        target_link_libraries(${name} PRIVATE ws2_32)
    endif()
    target_link_libraries(${name} PRIVATE
        nimrtc::dtls_seam
        nimrtc::dtls
        nimrtc::core
        nimrtc::plugins
        GTest::gtest_main)
    target_include_directories(${name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../include"
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${CMAKE_SOURCE_DIR}/src/plugins/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    nimrtc_apply_options(${name})
    add_test(NAME ${name}
        COMMAND $<TARGET_FILE:${name}>)
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
    if(COMMAND gtest_discover_tests)
        gtest_discover_tests(${name}
            PROPERTIES TIMEOUT 30
            DISCOVERY_TIMEOUT 10)
    endif()
endfunction()

# Slice 4 factory smoke test (4 subtests — see test_dtls_factory.cpp).
nimrtc_add_dtls_seam_test(test_dtls_factory test_dtls_factory.cpp)

# TPAL-4 mock-factory test (5 subtests — see test_dtls_seam_mock_factory.cpp).
#
# This one used to live in src/dtls/tests/ but was NOT registered with
# ctest (the old src/dtls/tests/CMakeLists.txt only wired
# test_dtls_factory).  The merge is the moment to fix that: the test is
# the executable proof that a THIRD-PARTY DTLS backend can be injected
# through the registry — exactly the "user can replace any module"
# criterion the remediation plan is judged by.
nimrtc_add_dtls_seam_test(test_dtls_seam_mock_factory
                          test_dtls_seam_mock_factory.cpp)

# -----------------------------------------------------------------------------
# DtlsHandshakeReassembler unit tests
#
# Direct GoogleTest coverage of the fragment-reassembly shim
# (see src/modules/dtls/include/nimrtc/dtls/dtls_handshake_reassembler.hpp).
# The shim is a small, pure C++ class with no wolfSSL dependencies, so it
# can be tested in isolation — much cheaper than driving a full
# DtlsSessionWolfSSL round-trip.
# -----------------------------------------------------------------------------
add_executable(test_dtls_handshake_reassembler
    test_dtls_handshake_reassembler.cpp)
if(WIN32)
    target_link_libraries(test_dtls_handshake_reassembler PRIVATE ws2_32)
endif()
target_link_libraries(test_dtls_handshake_reassembler PRIVATE
    nimrtc::dtls
    GTest::gtest_main)
target_include_directories(test_dtls_handshake_reassembler PRIVATE
    "${CMAKE_SOURCE_DIR}/src/core/include"
    "${CMAKE_SOURCE_DIR}/src/plugins/include"
    "${CMAKE_SOURCE_DIR}/src/modules/dtls/include")
if(WIN32)
    target_compile_definitions(test_dtls_handshake_reassembler PRIVATE
        _WIN32_WINNT=0x0601)
endif()
nimrtc_apply_options(test_dtls_handshake_reassembler LENIENT)
add_test(NAME test_dtls_handshake_reassembler
    COMMAND $<TARGET_FILE:test_dtls_handshake_reassembler>)
set_tests_properties(test_dtls_handshake_reassembler PROPERTIES TIMEOUT 30)
if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_dtls_handshake_reassembler
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 60)
endif()
