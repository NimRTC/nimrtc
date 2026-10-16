# modules/dtls/tests/CMakeLists.txt
# =============================================================================
# DTLS module unit tests.
# =============================================================================
# As of the wolfSSL-only migration, all DtlsSession tests live in
# src/modules/dtls/tests/ via the public DtlsSession class (which
# delegates to DtlsSessionWolfSSL).  The pre-migration BCrypt-direct
# tests have been folded into this directory; nothing under
# tests/ at the project root is BCrypt-specific any more — the
# Windows-only sanity-check files (test_dtls_gcm_aead.cpp /
# test_dtls_inproc.cpp / test_dtls_x25519.cpp) are governed by
# tests/CMakeLists.txt at the repo root and gated on _WIN32.
#

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
        DISCOVERY_TIMEOUT 10)
endif()
#