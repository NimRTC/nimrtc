# Enforce C++20 before project() so all targets inherit it.
# Rationale: std::span (core/bytes.hpp) is used pervasively for media buffer
# views and requires C++20.
#
# PLATFORM NOTE (0.9.0-rc1): this RC has ONLY been built + tested on
# Windows 10 / MSVC 19.30+. The compiler list below documents *intended*
# future support, NOT verified support for this release. See CHANGELOG.md
# "Platform support notice" before reporting build failures on other
# compilers — they are expected and out of scope for this RC.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
cmake_minimum_required(VERSION 3.20)

project(nimrtc
    VERSION 0.10.1
    DESCRIPTION "Native C++ WebRTC engine focused on AI Agent and embedded scenarios"
    LANGUAGES CXX C)

# -----------------------------------------------------------------------------
# Build options
# -----------------------------------------------------------------------------
# Declared BEFORE any include() so cache vars exist when helpers test them.
option(NIMRTC_BUILD_TESTS      "Build unit tests"                  ON)
option(NIMRTC_BUILD_EXAMPLES   "Build examples"                    OFF)
option(NIMRTC_BUILD_DOCS       "Build API documentation (Doxygen)" OFF)
option(NIMRTC_BUILD_BRIDGE_TESTS
       "Build Chrome↔NimRTC interop bridge (tests/wolfssl_dtls/bridge/)"
       OFF)
option(NIMRTC_VENDORED         "Use vendored third-party libraries" ON)
option(NIMRTC_ASAN             "Enable AddressSanitizer"           OFF)
option(NIMRTC_UBSAN            "Enable UndefinedBehaviorSanitizer" OFF)
option(NIMRTC_WARNINGS_AS_ERRORS "Treat warnings as errors"        ON)

# -----------------------------------------------------------------------------
# HW codec plugin switches (default OFF — opt-in per host platform)
# -----------------------------------------------------------------------------
# When a flag is ON, NimRTC builds the corresponding plugin object file and
# wires it into the `nimrtc_h264` library's HW backend table.  When OFF,
# the backend entry is still registered (so the selector compiles cleanly)
# but its `available()` always returns false and `create()` falls back to
# the stub codec.
#
# Per-platform availability (compile-time):
#   - NVENC        : Windows + Linux (NVIDIA drivers)
#   - AMF          : Windows (AMD drivers)
#   - QSV/libvpl   : Windows + Linux (Intel GPU + driver)
#   - DXVA         : Windows only
#   - VA-API       : Linux only
#   - NVDEC        : Windows + Linux (decoupled from NVENC — driver-only)
#
# Each switch is a *runtime* decision (you can flip it on/off without
# touching CMake) — see HW codec plugin docs.  The CMake flag controls
# whether the SDK is even *attempted* to be located on the build host.
# -----------------------------------------------------------------------------
option(NIMRTC_PLUGINS_NVENC  "Build NVIDIA NVENC H.264 encoder + NVDEC decoder"        OFF)
option(NIMRTC_PLUGINS_AMF    "Build AMD AMF H.264 encoder"                             OFF)
option(NIMRTC_PLUGINS_QSV    "Build Intel Quick Sync (libvpl/MediaSDK) H.264 encoder"  OFF)
option(NIMRTC_PLUGINS_DXVA   "Build Microsoft DXVA H.264 decoder"                      OFF)
option(NIMRTC_PLUGINS_VAAPI  "Build VA-API H.264 encoder + decoder (Linux Intel/AMD)"  OFF)
option(NIMRTC_PLUGINS_NVDEC  "Build NVIDIA NVDEC H.264 decoder (without NVENC)"        OFF)
option(NIMRTC_PLUGINS_OPENH264 "Build OpenH264 software H.264 encoder"                 OFF)

# Helper that prints a one-liner summary at the end of CMake configure.
set(_nimrtc_hw_summary "")
foreach(_hw NVENC AMF QSV DXVA VAAPI NVDEC OPENH264)
    if(NIMRTC_PLUGINS_${_hw})
        set(_nimrtc_hw_summary "${_nimrtc_hw_summary} ${_hw}")
    endif()
endforeach()
if(_nimrtc_hw_summary STREQUAL "")
    set(_nimrtc_hw_summary "(none)")
endif()
message(STATUS "  HW plugins enabled:${_nimrtc_hw_summary}")

# Vendored libwebrtc-audio-processing-2.a (audio3a WebRTC APM wrapper) is
# pre-built with MinGW/gcc in Release /MD mode.  When MSVC links it into a
# Debug /MDd consumer it trips LNK2038 mismatch errors.  Override the
# MSVC runtime that all test targets use to /MD so the link succeeds
# regardless of the host build type.  Set to "DEFAULT" to use CMake's
# per-config default (/MDd in Debug, /MD in Release).
# MSVC_RUNTIME_LIBRARY values are logical names (MultiThreadedDLL), not
# compiler flags (like /MD). Translate the user-friendly alias here.
if(DEFINED NIMRTC_TEST_RUNTIME_LIB AND NOT "${NIMRTC_TEST_RUNTIME_LIB}" STREQUAL "DEFAULT")
    if("${NIMRTC_TEST_RUNTIME_LIB}" STREQUAL "/MD")
        set(NIMRTC_TEST_RUNTIME_LIB_NAME "MultiThreadedDLL")
    elseif("${NIMRTC_TEST_RUNTIME_LIB}" STREQUAL "/MDd")
        set(NIMRTC_TEST_RUNTIME_LIB_NAME "MultiThreadedDebugDLL")
    elseif("${NIMRTC_TEST_RUNTIME_LIB}" STREQUAL "/MT")
        set(NIMRTC_TEST_RUNTIME_LIB_NAME "MultiThreaded")
    elseif("${NIMRTC_TEST_RUNTIME_LIB}" STREQUAL "/MTd")
        set(NIMRTC_TEST_RUNTIME_LIB_NAME "MultiThreadedDebug")
    else()
        set(NIMRTC_TEST_RUNTIME_LIB_NAME "${NIMRTC_TEST_RUNTIME_LIB}")
    endif()
else()
    set(NIMRTC_TEST_RUNTIME_LIB_NAME "")
endif()

# -----------------------------------------------------------------------------
# Project-wide compile settings (compiler-independent base)
# -----------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Force the MSVC runtime library to MultiThreadedDLL (/MD) for every
# configuration. The vendored audio3a webrtc_apm archive is built with
# MinGW/gcc in /MD mode; consumers that link it must use the same
# runtime to avoid LNK2038 mismatch errors. /MD is used regardless of
# build type (Debug/Release). Set NIMRTC_TEST_RUNTIME_LIB=DEFAULT to
# revert to CMake's per-config default.
if(MSVC AND NOT (DEFINED NIMRTC_TEST_RUNTIME_LIB AND
                 "${NIMRTC_TEST_RUNTIME_LIB}" STREQUAL "DEFAULT"))
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

# Initialise the GTest fetch switch BEFORE NimRTCTest runs find_package, so
# the FetchContent branch is reachable on a clean configure.
if(NOT DEFINED NIMRTC_FETCH_GTEST)
    set(NIMRTC_FETCH_GTEST ON CACHE BOOL
        "Auto-fetch GoogleTest via FetchContent if not installed")
endif()

include(NimRTCOptions)        # sets C++ standard + warnings + visibility
include(NimRTCVendored)       # declares vendor library targets (no-op if unused)
include(NimRTCTest)            # declares nimrtc_add_test() (no-op if tests OFF)

# HW codec plugin SDK detection (NVENC/AMF/QSV/DXVA/VA-API/NVDEC).
# Silent when the relevant NIMRTC_PLUGINS_<NAME>=OFF; otherwise probes
# standard install paths and reports "not found" without failing the build.
include(NimRTHwPlugins)
nimrtc_locate_hw_plugins()

# Vendored third-party sources — only added when NIMRTC_VENDORED is ON.
# When OFF the targets are still declared by NimRTCVendored.cmake as stubs.
if(NIMRTC_VENDORED)
    add_subdirectory(src/third_party)
    # Wire the per-library acquire aliases (vendored default → system
    # fallback) now that the vendored targets exist.  When any
    # NIMRTC_VENDORED_<NAME>=OFF, this calls find_package() instead of
    # aliasing the (non-existent) vendored target.
    nimrtc_acquire_all_vendored()
endif()

# -----------------------------------------------------------------------------
# WebRTC APM guidance
# -----------------------------------------------------------------------------
# The webrtc_audio_processing source (meson.build) may already be cloned via
# git submodule, but its pre-built static library requires an extra build step
# (Meson → static .a).  We detect this state and emit actionable guidance.
set(_webrtc_apm_src "${CMAKE_SOURCE_DIR}/src/third_party/webrtc_audio_processing/src/meson.build")
set(_webrtc_apm_cmake "${CMAKE_SOURCE_DIR}/src/third_party/webrtc_audio_processing/CMakeLists.txt")
if(NIMRTC_VENDORED_WEBRTC_APM AND EXISTS "${_webrtc_apm_src}" AND NOT EXISTS "${_webrtc_apm_cmake}")
    message(WARNING
        "\n"
        "  WebRTC Audio Processing (3A: AEC/ANS/AGC) source is present\n"
        "  but the pre-built static library has not been compiled yet.\n"
        "\n"
        "  Without it, audio3a uses a stub implementation (no 3A processing).\n"
        "\n"
        "  To enable full 3A support, run:\n"
        "\n"
        "    Windows:  python tools\\fetch_webrtc_apm.py\n"
        "    Linux:    python3 tools/fetch_webrtc_apm.py\n"
        "    macOS:    python3 tools/fetch_webrtc_apm.py\n"
        "\n"
        "  To skip 3A entirely (no action needed):\n"
        "\n"
        "    cmake -DNIMRTC_VENDORED_WEBRTC_APM=OFF ...\n"
        "\n"
    )
endif()

# -----------------------------------------------------------------------------
# Targets (in dependency order)
# -----------------------------------------------------------------------------
add_subdirectory(src/log)      # Logger implementation (nimrtc_log) — before core
add_subdirectory(src/core)     # Header-only: bytes / error / time / log header
add_subdirectory(src/plugins)  # Interface definitions (header-only)
add_subdirectory(src/sctp)     # SCTP seam (Transport PAL Slice 5; SEAM ONLY in v0.10)
add_subdirectory(src/dtls)     # DTLS seam (Transport PAL Slice 4; v0.11.0)
add_subdirectory(src/raw_udp)  # Raw-UDP bypass seam (Transport PAL Slice 6; v0.11.0)
add_subdirectory(src/transport) # Transport stack + Selector (Transport PAL Slice 7; v0.11.0)
add_subdirectory(src/modules)  # Concrete module implementations

# nlohmann_json 3.11.3 (used by the assembly module for JSON profile loading).
# Vendored as a single-header INTERFACE library under src/third_party/nlohmann_json/
# so that NIMRTC_MODULE_ASSEMBLY=ON builds work fully offline (no FetchContent,
# no network access). The subdirectory exposes the same target names
# (`nlohmann_json::nlohmann_json` and `nlohmann::nlohmann_json`) that
# upstream's nlohmann_jsonConfig.cmake provides, so consumer link lines
# in src/modules/assembly/CMakeLists.txt are unchanged.
if(NIMRTC_MODULE_ASSEMBLY)
    add_subdirectory(src/third_party/nlohmann_json)
endif()  # NIMRTC_MODULE_ASSEMBLY

add_subdirectory(src/engine)   # Composition layer (wires plugins together)

# Tests
if(NIMRTC_BUILD_TESTS)
    include(CTest)              # wires up enable_testing() + ctest discovery
    add_subdirectory(tests)

    # Post-configure hook: rewrite every CTestTestfile.cmake so plain
    # `ctest` (without `-C <config>`) can run non-GTest wrapper tests.
    # See cmake/NimRTCPostConfigure.cmake for rationale.
    include(NimRTCPostConfigure)
endif()

# CMake 4.0's multi-config CTestTestfile.cmake generation wraps every
# add_test() in `if(CTEST_CONFIGURATION_TYPE MATCHES "Debug")` — which
# means plain `ctest` (without -C Debug) marks every wrapper test as
# NOT_AVAILABLE.  We work around this three ways:
#   1. Force CTEST_CONFIGURATION_TYPE=Debug in the cache so ctest picks it
#      up via its implicit discovery path (no -C flag needed).
#   2. Generate a DartConfiguration.tcl that downstream tooling can also read.
#   3. Have gtest_discover_tests create per-case add_test() entries that
#      bypass the wrapper-level conditional (see module test CMakeLists).
if(NOT DEFINED CTEST_CONFIGURATION_TYPE)
    set(CTEST_CONFIGURATION_TYPE "Debug" CACHE STRING
        "Build configuration passed to ctest (Debug/Release/RelWithDebInfo)")
endif()
# Force the cache value to a regular variable so ctest's default discovery
# (which reads CTEST_CONFIGURATION_TYPE from environment / DartConfiguration
# / cache) resolves to Debug on a fresh build.
set(CTEST_CONFIGURATION_TYPE "${CTEST_CONFIGURATION_TYPE}" CACHE STRING
    "Default ctest build configuration (forces matching CTEST_CONFIGURATION_TYPE)"
    FORCE)
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/DartConfiguration.tcl.in"
    "${CMAKE_BINARY_DIR}/DartConfiguration.tcl"
    @ONLY)

# Examples / spike demos
if(NIMRTC_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Docs
if(NIMRTC_BUILD_DOCS)
    find_package(Doxygen)
    if(DOXYGEN_FOUND)
        add_subdirectory(docs/api)
    else()
        message(WARNING "Doxygen not found; skipping API docs")
    endif()
endif()

# -----------------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------------
message(STATUS "")
message(STATUS "============= NimRTC ${PROJECT_VERSION} =============")
message(STATUS "  C++ standard     : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build type       : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Vendor libs      : ${NIMRTC_VENDORED}")
message(STATUS "  Tests            : ${NIMRTC_BUILD_TESTS}")
message(STATUS "  Examples         : ${NIMRTC_BUILD_EXAMPLES}")
message(STATUS "  API docs         : ${NIMRTC_BUILD_DOCS}")
message(STATUS "  Bridge tests     : ${NIMRTC_BUILD_BRIDGE_TESTS}")
message(STATUS "  ASan / UBSan     : ${NIMRTC_ASAN} / ${NIMRTC_UBSAN}")
message(STATUS "  Warnings→errors  : ${NIMRTC_WARNINGS_AS_ERRORS}")
message(STATUS "==================================================")