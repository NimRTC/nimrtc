# modules/nvcodec/tests/CMakeLists.txt
# =============================================================================
# nvcodec module tests
#
# The round-trip test lives here (not in the h264 module) because the
# NVIDIA driver integration it exercises is owned by this module: the
# same NVENC / NVDEC session code serves H.264, H.265 and AV1, so the
# test drives the codec-generic entry points in nimrtc/nvcodec/.
# =============================================================================

find_package(GTest QUIET)

if(NOT GTEST_FOUND)
    if(NIMRTC_FETCH_GTEST)
        # GTest::gtest_main is normally provided by cmake/NimRTCTest.cmake via
        # add_subdirectory(src/third_party/googletest). Only fall back here if
        # we're being built standalone (e.g. module-only build).
        if(NOT TARGET GTest::gtest_main)
            set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
            set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
            set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
            add_subdirectory(${CMAKE_SOURCE_DIR}/src/third_party/googletest
                            ${CMAKE_BINARY_DIR}/_googletest_module_subbuild)
        endif()
    else()
        message(FATAL_ERROR "GoogleTest not found")
    endif()
endif()

# -----------------------------------------------------------------------------
# test_nvcodec_roundtrip — the hardware round-trip proof (plan item B10a)
# -----------------------------------------------------------------------------
# This is a *host-dependent* test: it needs a real GPU with the matching
# driver.  Every case calls GTEST_SKIP() when the backend is not compiled
# in or not available on the host, so it never fails a machine without
# NVIDIA hardware — it just reports the skip reason.
#
# The covered paths are:
#   - the NVENC -> NVDEC round trip,
#   - the NV12 output path,
#   - the multi-frame / reference-chain path,
#   - the resolution-change path,
#   - the sub-minimum-geometry padding path,
#   - the codec-generic leg (H.265 / AV1), which proves the driver
#     integration really is codec-parameterised and not H.264-only,
#   - and a decoder-only leg that decodes a self-contained H.264 bitstream
#     checked into src/modules/nvcodec/tests/nvdec_test_vector.hpp — so the
#     decode path can be verified on a host whose NVENC session is
#     unusable.
# -----------------------------------------------------------------------------
add_executable(test_nvcodec_roundtrip test_nvcodec_roundtrip.cpp)

set_target_properties(test_nvcodec_roundtrip PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_nvcodec_roundtrip LENIENT)

target_link_libraries(test_nvcodec_roundtrip PRIVATE
    nimrtc::nvcodec
    nimrtc::video_frame
    nimrtc::video_payload
    nimrtc::core
    nimrtc::plugins
    GTest::gtest_main
    GTest::gtest)

add_test(NAME modules_nvcodec_tests_test_nvcodec_roundtrip
         COMMAND test_nvcodec_roundtrip)
# The hardware round trip opens a CUDA context and a decoder session, so
# it needs more headroom than the pure-unit tests.
set_tests_properties(modules_nvcodec_tests_test_nvcodec_roundtrip
    PROPERTIES TIMEOUT 120)

if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_nvcodec_roundtrip
        PROPERTIES TIMEOUT 120
        DISCOVERY_TIMEOUT 60)
endif()

# -----------------------------------------------------------------------------
# test_nvcodec_zero_copy — the B10 zero-copy path, on real hardware
# -----------------------------------------------------------------------------
# The zero-copy path is a claim about a *negative* ("the picture never
# reached the CPU"), so the test has to make that negative observable:
# it paints a D3D11 texture with a compute shader (GPU-side only), hands
# it to the encoder through the public `encode_zero_copy()` seam, and
# checks the *decoded* picture.  A silent fallback to a CPU staging copy
# would produce a different colour and fail the comparison.
#
# Like the round-trip test it is host-dependent: every case skips
# cleanly when NVENC / NVDEC are unavailable.
# -----------------------------------------------------------------------------
add_executable(test_nvcodec_zero_copy test_nvcodec_zero_copy.cpp)

set_target_properties(test_nvcodec_zero_copy PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_nvcodec_zero_copy LENIENT)

target_link_libraries(test_nvcodec_zero_copy PRIVATE
    nimrtc::nvcodec
    nimrtc::video_frame
    nimrtc::core
    nimrtc::plugins
    GTest::gtest_main
    GTest::gtest)

# The shader is compiled at run time by D3DCompile, so the test needs the
# compiler library on the link line.
if(WIN32)
    target_link_libraries(test_nvcodec_zero_copy PRIVATE d3d11 d3dcompiler)
endif()

add_test(NAME modules_nvcodec_tests_test_nvcodec_zero_copy
         COMMAND test_nvcodec_zero_copy)
set_tests_properties(modules_nvcodec_tests_test_nvcodec_zero_copy
    PROPERTIES TIMEOUT 120)

if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_nvcodec_zero_copy
        PROPERTIES TIMEOUT 120
        DISCOVERY_TIMEOUT 60)
endif()
