# modules/rd_input/CMakeLists.txt
# ----------------------------------------------------------------------------
# Remote-desktop input module — plan Track G (G3 / G4 / G7).
#
# Scope:
#   - G3: the compact, versioned binary encoding of an input event
#          (mouse move / button / wheel / key down-up / text)
#   - G4: the pointer coalescer and the input path's own rate limit
#   - G7: the configurable allow / deny safety filter
#
# NOT in scope (other items in Track G, owned by other work):
#   - G1 / G1b / G2: the control transport (the engine owns the wiring)
#   - G5: the input <-> frame binding (needs the engine's ref-frame seam)
#   - G6: the dead-man switch / heartbeat (engine side)
#
# Why a module and not a seam (plan G3):
#   "It is a module, not a seam: the transport is DataChannel, and only the
#    payload format is NimRTC's."  So there is no `plugins::I*` interface
#    and nothing in src/plugins/ — the module is self-contained (L2 / L3).
#
# Dependencies:
#   - core    (always) — the byte / error / log vocabulary
# ----------------------------------------------------------------------------
add_library(nimrtc_rd_input STATIC
    src/coalescer.cpp
    src/event_codec.cpp
    src/safety_filter.cpp
    )

nimrtc_apply_options(nimrtc_rd_input)
target_link_libraries(nimrtc_rd_input
    PUBLIC
        nimrtc::core
    )

target_include_directories(nimrtc_rd_input
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_rd_input PUBLIC NIMRTC_HAS_RD_INPUT=1)

add_library(nimrtc::rd_input ALIAS nimrtc_rd_input)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    # ensure a CTestTestfile.cmake gets generated at this level so ctest can
    # recurse into the tests/ subdir from src/modules/ (CMake 4.0 VS
    # generator quirk — same pattern as modules/rtp).
    enable_testing()
    add_subdirectory(tests)
endif()
