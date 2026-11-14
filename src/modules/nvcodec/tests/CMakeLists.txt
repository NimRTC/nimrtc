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
        DISCOVERY_TIMEOUT 10)
endif()

# -----------------------------------------------------------------------------
# test_vt_common — the platform-independent VideoToolbox helpers
# -----------------------------------------------------------------------------
# The VideoToolbox encoder / decoder are macOS-only, but the bitstream
# plumbing they depend on (AV1 low-overhead OBU <-> Annex-B, AVCC ->
# Annex-B, the Annex-B scanner, the OS-version gate) is pure byte
# manipulation — so it is testable on every host, including the Windows
# CI box.
# -----------------------------------------------------------------------------
add_executable(test_vt_common test_vt_common.cpp)

set_target_properties(test_vt_common PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_vt_common LENIENT)

target_include_directories(test_vt_common PRIVATE
    "${CMAKE_SOURCE_DIR}/src/modules/nvcodec/src")

target_link_libraries(test_vt_common PRIVATE
    nimrtc::nvcodec
    nimrtc::video_frame
    nimrtc::core
    nimrtc::plugins
    GTest::gtest_main
    GTest::gtest)

add_test(NAME modules_nvcodec_tests_test_vt_common
         COMMAND test_vt_common)
set_tests_properties(modules_nvcodec_tests_test_vt_common PROPERTIES TIMEOUT 30)

if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_vt_common
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 10)
endif()

# -----------------------------------------------------------------------------
# test_hw_composite_codec — the composite adapter's telemetry forwarding
# -----------------------------------------------------------------------------
# A *pure unit* test: the composite is a forwarding adapter, so it is
# tested with codec doubles rather than with a GPU.  That keeps the
# "is the hardware flag reported?" question answerable on a machine with
# no NVIDIA card at all — which matters because the J5 capture/encode
# benchmark reads that flag to tell a hardware encode from a software
# one.
# -----------------------------------------------------------------------------
add_executable(test_hw_composite_codec test_hw_composite_codec.cpp)

set_target_properties(test_hw_composite_codec PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_hw_composite_codec LENIENT)

target_link_libraries(test_hw_composite_codec PRIVATE
    nimrtc::nvcodec
    nimrtc::video_frame
    nimrtc::core
    nimrtc::plugins
    GTest::gtest_main
    GTest::gtest)

add_test(NAME modules_nvcodec_tests_test_hw_composite_codec
         COMMAND test_hw_composite_codec)

if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_hw_composite_codec
        PROPERTIES TIMEOUT 60
        DISCOVERY_TIMEOUT 10)
endif()
