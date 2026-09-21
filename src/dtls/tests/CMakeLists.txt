# modules/dtls-seam/tests/CMakeLists.txt
# ----------------------------------------------------------------------------
# PAL Slice 4 seam tests — minimal GoogleTest suite that verifies the
# factory pattern (create() returns IDtlsSession, id() matches "wolfssl")
# and the register_default_plugins() idempotency latch.
#
# These tests are intentionally NOT linked against the full wolfSSL
# crypto stack so they can run in seconds.  Real handshake behaviour
# (e2e Case D, Chrome interop) is covered by the existing
# tests/test_dtls_handshake_e2e.cpp and the per-module tests in
# src/modules/dtls/tests/.
# ----------------------------------------------------------------------------
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
        "${CMAKE_CURRENT_SOURCE_DIR}/../../core/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../plugins/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../modules/dtls/include")
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

# Slice 4 factory smoke test (4 subtests — see test_dtls_factory.cpp).
nimrtc_add_dtls_seam_test(test_dtls_factory test_dtls_factory.cpp)

# TPAL-4 (v0.11.0) — seam mock-factory test.  Registers a
# MockDtlsFactory under a non-default id and verifies the engine's
# registry lookup, factory.resolve(), session vtable, and
# same-id overwrite semantics all behave correctly.  This is the
# substitution-bench for a future 国密 / OpenSSL / BoringSSL /
# mbedTLS backend; running a real DTLS through the mock and then
# re-pointing the engine at it is the testbed for Slice 6+ /
# national-backend verification.
nimrtc_add_dtls_seam_test(test_dtls_seam_mock_factory
                          test_dtls_seam_mock_factory.cpp)
