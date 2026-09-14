# FixCTestTestfile.cmake
# ----------------------------------------------------------------------------
# Post-configure hook: strips CMake 4.0's broken CTEST_CONFIGURATION_TYPE
# conditional from CTestTestfile.cmake files so plain `ctest` (without
# `-C <config>`) works for non-GTest wrapper tests.
#
# Background
# ----------
# The VS / Xcode / Ninja Multi-Config generators emit each add_test() in a
# 4-branch conditional in CTestTestfile.cmake:
#
#   if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
#     add_test(... "Debug/foo.exe")
#   elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
#     add_test(... "Debug/foo.exe")
#   elseif(... MinSizeRel ...)
#     add_test(... "Debug/foo.exe")
#   elseif(... RelWithDebInfo ...)
#     add_test(... "Debug/foo.exe")
#   else()
#     add_test(... NOT_AVAILABLE)
#   endif()
#
# When ctest is invoked without `-C <config>` (e.g. `ctest -j4`), the
# CTEST_CONFIGURATION_TYPE variable is empty. None of the four regex
# branches match, the else() branch fires, and the test is registered
# as `NOT_AVAILABLE`. gtest_discover_tests is unaffected (it uses
# POST_BUILD discovery and emits a separate `POST_BUILD` command that
# does not depend on the variable), but every hand-written `add_test()`
# call — exactly the non-GTest E2E / DTLS / plugin-loading wrappers —
# becomes unrunnable.
#
# This script runs after CMake configure, scans every CTestTestfile.cmake
# under the build tree, and rewrites each wrapper block to keep only the
# body of the Debug branch (which is the configuration whose binaries
# are sitting on disk for this build).
#
# Invocation
# ----------
#     cmake -P cmake/FixCTestTestfile.cmake <build_dir>
#
# Idempotent: files already rewritten are skipped (no `CTEST_CONFIGURATION_TYPE
# MATCHES` token to find).
# ----------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.20)

# cmake -P forwards the build dir as the first trailing arg.
# CMake passes script-mode args via CMAKE_ARGV3 / CMAKE_ARGV4 ...
# (index 0 = script path, 1.. = positional args).
if(CMAKE_ARGV3 STREQUAL "")
    message(FATAL_ERROR
        "Usage: cmake -P ${CMAKE_CURRENT_LIST_FILE} <build_dir>")
endif()
set(build_dir "${CMAKE_ARGV3}")
if(NOT EXISTS "${build_dir}")
    message(FATAL_ERROR "Build directory does not exist: ${build_dir}")
endif()

# Resolve to absolute path so the glob works regardless of cwd.
get_filename_component(build_dir "${build_dir}" ABSOLUTE)

file(GLOB_RECURSE testfiles "${build_dir}/CTestTestfile.cmake")

set(scanned 0)
set(rewritten 0)
set(unchanged 0)

foreach(testfile IN LISTS testfiles)
    math(EXPR scanned "${scanned} + 1")

    file(READ "${testfile}" content)
    # Normalise line endings so the line-by-line walk is consistent.
    string(REPLACE "\r\n" "\n" content "${content}")

    # Quick reject: file has no CTEST_CONFIGURATION_TYPE conditional.
    string(FIND "${content}" "CTEST_CONFIGURATION_TYPE MATCHES" hit)
    if(hit EQUAL -1)
        math(EXPR unchanged "${unchanged} + 1")
        continue()
    endif()

    # Split into lines, preserving blank lines. CMake list separators are
    # semicolons; we replace each newline with a sentinel then split.
    string(REPLACE "\n" "@NIMRTC_EOL@;" lines "${content}")

    set(out "")
    set(in_wrapper FALSE)
    set(in_debug_body FALSE)

    foreach(line IN LISTS lines)
        if(in_wrapper)
            # else-if / else branch ends the Debug body — drop everything
            # until the matching endif().
            if(line MATCHES "^elseif\\(CTEST_CONFIGURATION_TYPE MATCHES"
                    OR line MATCHES "^else\\(\\)")
                set(in_debug_body FALSE)
                continue()
            endif()

            if(line MATCHES "^endif\\(\\)")
                set(in_wrapper FALSE)
                set(in_debug_body FALSE)
                continue()
            endif()

            if(in_debug_body)
                string(APPEND out "${line}@NIMRTC_EOL@")
            endif()
        else()
            # Opening of the conditional. Anchored to the Debug regex so
            # we don't accidentally trigger on a line that just mentions
            # the variable in a comment.
            if(line MATCHES "^if\\(CTEST_CONFIGURATION_TYPE MATCHES \"\\^\\(\\[Dd\\]\\[Ee\\]\\[Bb\\]\\[Uu\\]\\[Gg\\]\\)\\$\"\\)")
                set(in_wrapper TRUE)
                set(in_debug_body TRUE)
                continue()
            endif()

            string(APPEND out "${line}@NIMRTC_EOL@")
        endif()
    endforeach()

    # Restore newlines and write back. Append a trailing newline because
    # we stripped the last one when walking the list.
    string(REPLACE "@NIMRTC_EOL@" "\n" out "${out}")
    if(NOT out MATCHES "\n$")
        string(APPEND out "\n")
    endif()

    file(WRITE "${testfile}" "${out}")
    math(EXPR rewritten "${rewritten} + 1")
endforeach()

message(STATUS
    "FixCTestTestfile: scanned=${scanned} rewritten=${rewritten} unchanged=${unchanged}")
