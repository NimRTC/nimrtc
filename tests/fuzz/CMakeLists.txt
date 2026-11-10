# tests/fuzz/CMakeLists.txt — fuzz targets + corpus replay driver
# ----------------------------------------------------------------------------
# Two things live here:
#
#   1. `nimrtc_fuzz_targets` — a static library holding the three
#      `run_*()` entry points (SDP / RTP / video payload).  The entry
#      points are plain functions, so both the libFuzzer harnesses and
#      the replay driver can call them.
#
#   2. `test_fuzz_replay` — an ordinary test executable that replays
#      the seed corpus (plus a deterministic mutation set) through
#      those entry points.  It runs on every platform, on every
#      commit, and it needs no special toolchain.
#
# The seeds are checked in as *text* (`.hex`, and the `.sdp` seeds):
# binary blobs do not belong in this repository.  The raw byte corpus
# libFuzzer wants is generated into the build tree by
# `tools/make_corpus.py --materialize <dir>`.
#
# Optionally (NIMRTC_FUZZING=ON, Clang only) three libFuzzer
# executables are built: `fuzz_sdp`, `fuzz_rtp`, `fuzz_video_payload`.
# Those need `-fsanitize=fuzzer`, which only Clang provides.
#
# Why the replay driver exists at all: a project that only fuzzes
# occasionally still needs the found crashes to stay fixed.  The
# replay driver turns every libFuzzer finding into a committed seed
# that is re-run on every commit, so a fixed bug cannot silently
# come back.
# ----------------------------------------------------------------------------

set(_fuzz_sources
    fuzz_sdp.cpp
    fuzz_rtp.cpp
    fuzz_video_payload.cpp)

# ----------------------------------------------------------------------------
# The shared target library
# ----------------------------------------------------------------------------
add_library(nimrtc_fuzz_targets STATIC ${_fuzz_sources})

nimrtc_apply_options(nimrtc_fuzz_targets)

target_include_directories(nimrtc_fuzz_targets
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${CMAKE_SOURCE_DIR}/src/plugins/include"
        "${CMAKE_SOURCE_DIR}/src/modules/rtp/include"
        "${CMAKE_SOURCE_DIR}/src/modules/sdp/include"
        "${CMAKE_SOURCE_DIR}/src/modules/video_payload/include"
        "${CMAKE_SOURCE_DIR}/src/modules/video_jb/include"
        "${CMAKE_SOURCE_DIR}/src/modules/video_frame/include"
        "${CMAKE_BINARY_DIR}/include")

target_link_libraries(nimrtc_fuzz_targets
    PUBLIC
        nimrtc::core
        nimrtc::plugins
        nimrtc::rtp
        nimrtc::sdp
        nimrtc::video_payload
        nimrtc::video_jb)

# The entry points are only exported for the test / fuzzer binaries;
# the library itself is internal to the test tree.
set_target_properties(nimrtc_fuzz_targets PROPERTIES
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF)

# ----------------------------------------------------------------------------
# The corpus replay driver (runs everywhere)
# ----------------------------------------------------------------------------
add_executable(test_fuzz_replay
    fuzz_replay.cpp)

nimrtc_apply_options(test_fuzz_replay)

target_link_libraries(test_fuzz_replay
    PRIVATE
        nimrtc_fuzz_targets)

# The corpus lives in the source tree; the driver reads it at runtime.
# The seeds are *text* (`.hex`, plus the `.sdp` seeds) — binary blobs
# are not checked in.  `tools/make_corpus.py --materialize <dir>`
# regenerates the raw byte corpus that libFuzzer wants.
target_compile_definitions(test_fuzz_replay PRIVATE
    NIMRTC_FUZZ_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")

# The test has to be registered before its properties are set: CMake
# resolves `set_tests_properties` against the tests that exist at that
# point, and fails the configure step otherwise.
add_test(NAME test_fuzz_replay COMMAND $<TARGET_FILE:test_fuzz_replay>)
set_tests_properties(test_fuzz_replay PROPERTIES TIMEOUT 120)
# ----------------------------------------------------------------------------
# libFuzzer harnesses (optional, Clang only)
# ----------------------------------------------------------------------------
if(NIMRTC_FUZZING)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "NIMRTC_FUZZING=ON requires Clang (-fsanitize=fuzzer is a "
            "Clang-only flag).  The compiler in use is "
            "'${CMAKE_CXX_COMPILER_ID}'.")
    endif()

    # One executable per target: libFuzzer's entry point is a single
    # symbol with a fixed name, so each target needs its own binary.
    foreach(_target sdp rtp video_payload)
        add_executable(fuzz_${_target} fuzz_entry_${_target}.cpp)
        nimrtc_apply_options(fuzz_${_target})
        target_compile_definitions(fuzz_${_target} PRIVATE NIMRTC_FUZZING=1)
        target_link_libraries(fuzz_${_target}
            PRIVATE
                nimrtc_fuzz_targets
                nimrtc::core
                nimrtc::plugins
                nimrtc::rtp
                nimrtc::sdp
                nimrtc::video_payload
                nimrtc::video_jb)
        target_compile_options(fuzz_${_target} PRIVATE
            -fsanitize=fuzzer
            -fno-omit-frame-pointer)
        target_link_options(fuzz_${_target} PRIVATE
            -fsanitize=fuzzer)
    endforeach()
endif()
