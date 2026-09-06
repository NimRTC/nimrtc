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
    VERSION 1.0.0
    DESCRIPTION "Native C++ WebRTC engine focused on AI Agent and embedded scenarios"
    LANGUAGES CXX C)

# -----------------------------------------------------------------------------
# Build options
# -----------------------------------------------------------------------------
# Declared BEFORE any include() so cache vars exist when helpers test them.
option(NIMRTC_BUILD_TESTS      "Build unit tests"                  ON)
option(NIMRTC_BUILD_EXAMPLES   "Build examples"                    OFF)
option(NIMRTC_BUILD_DOCS       "Build API documentation (Doxygen)" OFF)
option(NIMRTC_VENDORED         "Use vendored third-party libraries" ON)
option(NIMRTC_ASAN             "Enable AddressSanitizer"           OFF)
option(NIMRTC_UBSAN            "Enable UndefinedBehaviorSanitizer" OFF)
option(NIMRTC_WARNINGS_AS_ERRORS "Treat warnings as errors"        ON)

# -----------------------------------------------------------------------------
# Project-wide compile settings (compiler-independent base)
# -----------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Initialise the GTest fetch switch BEFORE NimRTCTest runs find_package, so
# the FetchContent branch is reachable on a clean configure.
if(NOT DEFINED NIMRTC_FETCH_GTEST)
    set(NIMRTC_FETCH_GTEST ON CACHE BOOL
        "Auto-fetch GoogleTest via FetchContent if not installed")
endif()

include(NimRTCOptions)        # sets C++ standard + warnings + visibility
include(NimRTCVendored)       # declares vendor library targets (no-op if unused)
include(NimRTCTest)            # declares nimrtc_add_test() (no-op if tests OFF)

# Vendored third-party sources — only added when NIMRTC_VENDORED is ON.
# When OFF the targets are still declared by NimRTCVendored.cmake as stubs.
if(NIMRTC_VENDORED)
    add_subdirectory(src/third_party)
endif()

# -----------------------------------------------------------------------------
# Targets (in dependency order)
# -----------------------------------------------------------------------------
add_subdirectory(src/log)      # Logger implementation (nimrtc_log) — before core
add_subdirectory(src/core)     # Header-only: bytes / error / time / log header
add_subdirectory(src/plugins)  # Interface definitions (header-only)
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
message(STATUS "  ASan / UBSan     : ${NIMRTC_ASAN} / ${NIMRTC_UBSAN}")
message(STATUS "  Warnings→errors  : ${NIMRTC_WARNINGS_AS_ERRORS}")
message(STATUS "==================================================")