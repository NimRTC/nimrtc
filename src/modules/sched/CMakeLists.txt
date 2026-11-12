# modules/sched/CMakeLists.txt
# ----------------------------------------------------------------------------
# Unified Sending Scheduler — send-priority scheduling module.
#
# Responsibilities:
#   - Two independent queue families inside one Scheduler instance:
#     P-family (per-packet, plugins::Priority, 5 levels) and S-family
#     (per-stream, sched::StreamPriority, 3 buckets).  They are never
#     compared against each other — see sched.hpp §"Terminology".
#   - Strict descending priority inside the P-family: kControl > kAudio >
#     kVideoKeyframe > kVideo > kBestEffort.
#   - BWE-driven congestion response: when on_bwe_update() signals a drop,
#     kVideo / kBestEffort are preferentially dropped — kControl is never
#     dropped by the scheduler.
#
# Dependencies:
#   - nimrtc::core  — BufferView, TimePoint, log helpers
#   - nimrtc::plugins — Addr, OutPacket
#   - nimrtc::pacing — the per-class token buckets (v1.2.0 M3, video
#     plan P2).  The scheduler owns one bucket per traffic class and
#     shapes the outbound traffic with it.
# ----------------------------------------------------------------------------
add_library(nimrtc_sched STATIC
    src/scheduler.cpp
    src/scheduler_plugin.cpp
    )

# STRICT_CONVERSIONS: the scheduler converts byte counts to BWE credit
# and to pacing tokens in several places, so a narrowing conversion
# here silently changes the congestion response.
nimrtc_apply_options(nimrtc_sched STRICT_CONVERSIONS)
target_link_libraries(nimrtc_sched
    PUBLIC
        nimrtc::core
        nimrtc::plugins   # Addr / OutPacket / BufferView
    PRIVATE
        nimrtc::pacing   # the per-class token buckets
    )

target_include_directories(nimrtc_sched
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

add_library(nimrtc::sched ALIAS nimrtc_sched)

if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
