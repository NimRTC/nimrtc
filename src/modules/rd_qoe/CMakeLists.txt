# modules/rd_qoe/CMakeLists.txt
# ----------------------------------------------------------------------------
# Remote-desktop QoE module — plan Track J (J3 / J6).
#
# Scope:
#   - J3: the quality-of-experience metrics — freeze count / duration,
#          the sharpness proxy, the end-to-end latency percentiles and
#          the viewer's frame rate
#   - J6: the input-to-photon measurement (the input event -> the frame
#          that reflects it, displayed)
#
# NOT in scope (other items in Track J, owned by other work):
#   - J1: the per-frame CPU-time instrumentation (the engine's own
#          `Stats::video_stage_*`)
#   - J2: the latency breakdown (the engine's own stage timings)
#   - J4 / J5: the benchmark tools (tools/benchmark_remote_desktop.py,
#          tools/benchmark_capture_encode.py)
#
# Why a module and not a seam (plan J3):
#   The collector is *fed* by the host — it never pulls from the engine.
#   That keeps it decoupled and unit-testable, so there is no
#   `plugins::I*` interface and nothing in src/plugins/ — the module is
#   self-contained (L2 / L3).
#
# Dependencies:
#   - core    (always) — the byte / error / log vocabulary
# ----------------------------------------------------------------------------
add_library(nimrtc_rd_qoe STATIC
    src/qoe_metrics.cpp
    )

nimrtc_apply_options(nimrtc_rd_qoe)
target_link_libraries(nimrtc_rd_qoe
    PUBLIC
        nimrtc::core
    )

target_include_directories(nimrtc_rd_qoe
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_rd_qoe PUBLIC NIMRTC_HAS_RD_QOE=1)

add_library(nimrtc::rd_qoe ALIAS nimrtc_rd_qoe)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    # ensure a CTestTestfile.cmake gets generated at this level so ctest can
    # recurse into the tests/ subdir from src/modules/ (CMake 4.0 VS
    # generator quirk — same pattern as modules/rtp).
    enable_testing()
    add_subdirectory(tests)
endif()
