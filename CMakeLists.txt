# Enforce C++20 before project() so all targets inherit it.
# Rationale: std::span (core/bytes.hpp) is used pervasively for media buffer
# views and requires C++20. The technical doc §13 originally locked C++17 for
# ARM GCC compatibility; that decision is re-opened here because:
#   - As of 2026, every compiler we target (MSVC 19.30+, GCC 10+, Clang 12+,
#     Apple Clang 14+) has full C++20 support per cmake/NimRTCOptions.cmake.
#   - Removing std::span to honour the original C++17 lock would be a large
#     refactor for diminishing returns on platforms we already support.
# See docs/zh/NimRTC-V2-技术文档.md §13 (decision: C++17 → C++20, 2026-09-03).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
cmake_minimum_required(VERSION 3.20)

project(nimrtc
    VERSION 0.1.0
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
add_subdirectory(src/engine)   # Composition layer (wires plugins together)

# Tests
if(NIMRTC_BUILD_TESTS)
    include(CTest)              # wires up enable_testing() + ctest discovery
    add_subdirectory(tests)
endif()

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