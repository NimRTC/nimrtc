# tests/CMakeLists.txt — top-level cross-module integration tests
# ----------------------------------------------------------------------------
# Tests that span multiple modules (e.g. RTP+JB+SDP end-to-end) live here.
# Single-module tests live inside each module's `tests/` directory.
# ----------------------------------------------------------------------------

# E2E tests are standalone (custom main(), no GoogleTest). They each use
# raw sockets to verify round-trip behaviour. add_e2e_test is a small wrapper
# that wires the include dirs, links Winsock on Windows, and registers the
# test with CTest.
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
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
endfunction()

# RTP round-trip over UDP loopback.
add_e2e_test(test_udp_loopback  test_udp_loopback.cpp)

# RTCP SR/RR/NACK round-trip over UDP loopback.
add_e2e_test(test_rtcp_loopback test_rtcp_loopback.cpp)

# Parser error-path matrix (no network).
add_e2e_test(test_error_matrix  test_error_matrix.cpp)
