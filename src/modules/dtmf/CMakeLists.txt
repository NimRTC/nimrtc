# modules/dtmf/CMakeLists.txt
# ----------------------------------------------------------------------------
# RFC 4733 (telephone-event) codec module — v1.2.0 (session capability plan
# D1 / D2).
#
# Scope:
#   - Encode / decode the RFC 4733 §2.3.1 four-byte telephone-event payload
#   - Implement the plugins::IDtmfCodec seam
#
# Out of scope:
#   - The repetition / end-of-event timing (RFC 4733 §2.5) — the engine owns
#     the clock and the send path
#   - SDP negotiation of the payload type — the engine's job
#
# Dependencies:
#   - core    (always)
#   - plugins (the seam)
# ----------------------------------------------------------------------------
add_library(nimrtc_dtmf STATIC
    src/dtmf_codec.cpp
    src/register.cpp
    )

nimrtc_apply_options(nimrtc_dtmf)
target_link_libraries(nimrtc_dtmf
    PUBLIC
        nimrtc::core
        nimrtc::plugins       # plugin interface (IDtmfCodec / factory)
    )

target_include_directories(nimrtc_dtmf
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_dtmf PUBLIC NIMRTC_HAS_DTMF=1)

add_library(nimrtc::dtmf ALIAS nimrtc_dtmf)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    # ensure a CTestTestfile.cmake gets generated at this level so ctest
    # can recurse into the tests/ subdir from src/modules/ (CMake 4.0
    # VS generator quirk — same pattern as modules/rtp).
    enable_testing()
    add_subdirectory(tests)
endif()
