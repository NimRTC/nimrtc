# modules/rtx/CMakeLists.txt
# ----------------------------------------------------------------------------
# The RFC 4588 retransmission payload format.
#
# Per docs/plan/v1.3-plan.md §Q3 (video plan T1): the retransmission is
# a *payload format*, and the format is what this module owns:
#
#   - the two-octet OSN header (RFC 4588 §4.1) — the original sequence
#     number in network byte order, followed by the original payload;
#   - the `apt=` / `rtx-time=` SDP parameters (RFC 4588 §8.1) that bind
#     a retransmission payload type to the payload type it protects.
#
# The module is a *format* module, not a transport: it does not build
# RTP headers, does not decide whether to retransmit, and does not send
# anything.  Both ends of a retransmission use it, so the framing is
# defined in exactly one place.
#
# Dependencies:
#   - nimrtc::core (ByteSpan / MutableByteSpan / Result)
#
# No vendor libraries; pure C++20.
# ----------------------------------------------------------------------------
add_library(nimrtc_rtx STATIC
    src/rtx.cpp
    )

nimrtc_apply_options(nimrtc_rtx)
target_link_libraries(nimrtc_rtx
    PUBLIC
        nimrtc::core
    )

target_include_directories(nimrtc_rtx
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_rtx PUBLIC NIMRTC_HAS_RTX=1)

add_library(nimrtc::rtx ALIAS nimrtc_rtx)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
