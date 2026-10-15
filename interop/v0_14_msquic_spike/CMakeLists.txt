# ADR-014 spike CMakeLists.txt
#
# Builds the 5 spike programs that exercise the msquic C API
# surface against the planned IWebTransportSession methods.
# Links against src/third_party/msquic/ (the vendored submodule;
# SHA-pinned via vendor.json).
#
# This is a SPIKE, not a test. The 5 programs are minimum-viable
# code that verifies the 5 evaluation criteria in
# docs/adr/ADR-014-web-transport-vendor.md. They are NOT part
# of the ctest regression surface; they are deleted at the
# beta.1 tag cut (or migrated to tests/test_web_transport_msquic.cpp
# as proper regression tests — that migration is a separate PR).

cmake_minimum_required(VERSION 3.20)
project(nimrtc_msquic_spike CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Criterion 1 assertion: MIT license ----------------------------
#
# Read the LICENSE header from src/third_party/msquic/ and assert
# the MIT identifier is present. This is a build-time assertion;
# if the LICENSE is not MIT, the configure step fails.
set(MSQUIC_LICENSE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../src/third_party/msquic/LICENSE")
if(EXISTS "${MSQUIC_LICENSE_PATH}")
    file(READ "${MSQUIC_LICENSE_PATH}" MSQUIC_LICENSE_TEXT)
    string(FIND "${MSQUIC_LICENSE_TEXT}" "MIT License" MSQUIC_MIT_FOUND)
    if(MSQUIC_MIT_FOUND EQUAL -1)
        string(FIND "${MSQUIC_LICENSE_TEXT}" "Permission is hereby granted, free of charge"
                     MSQUIC_MIT_FOUND_ALT)
        if(MSQUIC_MIT_FOUND_ALT EQUAL -1)
            message(FATAL_ERROR
                "ADR-014 Criterion 1 FAIL: src/third_party/msquic/LICENSE "
                "does not contain the MIT identifier. ADR-014 reopens; "
                "fallback path is quiche.")
        endif()
    endif()
    message(STATUS "ADR-014 Criterion 1 (License): MIT identifier found")
else()
    message(WARNING
        "ADR-014 Criterion 1 SKIPPED: src/third_party/msquic/LICENSE not found. "
        "Run `cmake -DNIMRTC_FETCH_VENDORED=ON` to fetch the submodule.")
endif()

# ---- Criterion 2 assertion: no Rust / no CFFI ----------------------
#
# Walk the msquic tree and count .rs files. If > 0, ADR-014
# reopens (Rust CFFI violates the src/plugins/ header-only
# invariant per v1.0-plan.md §10.2).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../src/third_party/msquic")
    file(GLOB_RECURSE MSQUIC_RUST_FILES
         "${CMAKE_CURRENT_SOURCE_DIR}/../../src/third_party/msquic/*.rs")
    list(LENGTH MSQUIC_RUST_FILES MSQUIC_RUST_COUNT)
    if(MSQUIC_RUST_COUNT GREATER 0)
        message(FATAL_ERROR
            "ADR-014 Criterion 2 FAIL: msquic tree contains ${MSQUIC_RUST_COUNT} "
            ".rs files. Rust CFFI violates the src/plugins/ header-only invariant. "
            "ADR-014 reopens; fallback path is quiche.")
    endif()
    message(STATUS "ADR-014 Criterion 2 (No Rust / no CFFI): "
                   "${MSQUIC_RUST_COUNT} .rs files (target: 0)")
else()
    message(WARNING
        "ADR-014 Criterion 2 SKIPPED: src/third_party/msquic not found. "
        "Run `cmake -DNIMRTC_FETCH_VENDORED=ON` to fetch the submodule.")
endif()

# ---- Criterion 5 data: submodule age -------------------------------
#
# Read the latest commit timestamp from the msquic submodule
# and compute the age in days. The spike-run script asserts
# the age is < 180 days (6 months) for criterion 5 PASS.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../src/third_party/msquic/.git")
    execute_process(
        COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}/../../src/third_party/msquic"
                log -1 --format=%H
        OUTPUT_VARIABLE MSQUIC_LATEST_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(
        COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}/../../src/third_party/msquic"
                log -1 --format=%ci
        OUTPUT_VARIABLE MSQUIC_LATEST_DATE
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    message(STATUS "ADR-014 Criterion 5 (Active maintenance) data: "
                   "SHA=${MSQUIC_LATEST_SHA} date=${MSQUIC_LATEST_DATE}")
    # Compute age in days (cross-platform via Python — portable
    # across the 4 CI platforms).
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -c "
import datetime, sys
latest = datetime.datetime.strptime('${MSQUIC_LATEST_DATE}'.strip().split(' +')[0].split(' -')[0], '%Y-%m-%d %H:%M:%S')
now    = datetime.datetime.utcnow()
age    = (now - latest).days
print(f'{age}')"
        OUTPUT_VARIABLE MSQUIC_AGE_DAYS
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    message(STATUS "ADR-014 Criterion 5 (Active maintenance): "
                   "submodule age = ${MSQUIC_AGE_DAYS} days (target: < 180)")
    if(MSQUIC_AGE_DAYS GREATER 180)
        message(WARNING
            "ADR-014 Criterion 5 WARN: submodule is ${MSQUIC_AGE_DAYS} days old "
            "(target: < 180). The spike-run script will mark this as FAIL; "
            "ADR-014 reopens and the fallback is quiche.")
    endif()
else()
    message(WARNING
        "ADR-014 Criterion 5 SKIPPED: src/third_party/msquic/.git not found. "
        "Run `cmake -DNIMRTC_FETCH_VENDORED=ON` to fetch the submodule.")
endif()

# ---- msquic library discovery --------------------------------------
#
# When the submodule is present, link against the msquic static
# library. The spike programs use the C API directly (no C++
# wrapper); the headers live in src/third_party/msquic/src/inc/.
set(MSQUIC_INC_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../src/third_party/msquic/src/inc")
if(EXISTS "${MSQUIC_INC_PATH}")
    target_include_directories(spike INTERFACE "${MSQUIC_INC_PATH}")
endif()

# ---- 5 spike programs ----------------------------------------------

add_executable(spike_echo_client       src/echo_client.cpp)
add_executable(spike_echo_server       src/echo_server.cpp)
add_executable(spike_stream_reset      src/stream_reset.cpp)
add_executable(spike_datagram_roundtrip src/datagram_roundtrip.cpp)
add_executable(spike_concurrent_streams src/concurrent_streams.cpp)

foreach(spike_target IN ITEMS spike_echo_client
                                  spike_echo_server
                                  spike_stream_reset
                                  spike_datagram_roundtrip
                                  spike_concurrent_streams)
    target_include_directories(${spike_target} PRIVATE src)
    if(EXISTS "${MSQUIC_INC_PATH}")
        target_include_directories(${spike_target} PRIVATE "${MSQUIC_INC_PATH}")
    endif()
    if(MSVC)
        target_compile_options(${spike_target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${spike_target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()
