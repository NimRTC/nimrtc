# modules/jb_strategy/CMakeLists.txt
# ----------------------------------------------------------------------------
# Shared adaptive jitter-buffer strategy.
#
# v1.1 track Q2 ("视频侧自适应 JB 统一"): the RFC 3550 §6.4.1 interarrival
# jitter estimator and the playout-delay derivation live here exactly once.
# The audio front-end (modules/jb) and the video front-end (modules/video_jb)
# both link this module and keep only their own buffering structures.
#
# Dependencies:
#   - core (always)
# ----------------------------------------------------------------------------
add_library(nimrtc_jb_strategy STATIC
    src/adaptive_delay.cpp
    )

nimrtc_apply_options(nimrtc_jb_strategy)
target_link_libraries(nimrtc_jb_strategy
    PUBLIC
        nimrtc::core
    )

target_include_directories(nimrtc_jb_strategy
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_jb_strategy PUBLIC NIMRTC_HAS_JB_STRATEGY=1)

add_library(nimrtc::jb_strategy ALIAS nimrtc_jb_strategy)

if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    # ensure a CTestTestfile.cmake gets generated at this level so ctest
    # can recurse into the tests/ subdir from src/modules/ (CMake 4.0
    # VS generator quirk -- without enable_testing() here the file is
    # silently skipped and the module's tests never register).
    enable_testing()
    add_subdirectory(tests)
endif()
