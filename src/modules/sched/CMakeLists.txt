# modules/sched/CMakeLists.txt
# ----------------------------------------------------------------------------
# Unified Sending Scheduler — L1 packet prioritisation module.
#
# Responsibilities:
#   - Multiplex RTP (media) and DataChannel (data) into one priority queue.
#   - Strict descending priority: kControl > kAudio > kVideoKeyframe > kVideo >
#     kBestEffort.
#   - BWE-driven congestion response: when on_bwe_update() signals a drop,
#     kVideo / kBestEffort are preferentially dropped — kControl is never
#     dropped by the scheduler.
#
# Dependencies:
#   - nimrtc::core  — BufferView, TimePoint, log helpers
#   - nimrtc::plugins — Addr, OutPacket
# ----------------------------------------------------------------------------
add_library(nimrtc_sched STATIC
    src/scheduler.cpp
    src/scheduler_plugin.cpp
    )

nimrtc_apply_options(nimrtc_sched)
target_link_libraries(nimrtc_sched
    PUBLIC
        nimrtc::core
        nimrtc::plugins   # Addr / OutPacket / BufferView
    PRIVATE
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
