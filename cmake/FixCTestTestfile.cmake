# FixCTestTestfile.cmake
# ----------------------------------------------------------------------------
# Post-configure hook: makes CMake 4.0's CTEST_CONFIGURATION_TYPE conditional
# in CTestTestfile.cmake resolve to a real binary for *every* ctest
# invocation style — `ctest -C <config>` and plain `ctest`.
#
# Background
# ----------
# The VS / Xcode / Ninja Multi-Config generators emit each add_test() in a
# 4-branch conditional in CTestTestfile.cmake:
#
#   if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
#     add_test(... "Debug/foo.exe")
#   elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
#     add_test(... "Release/foo.exe")
#   elseif(... MinSizeRel ...)
#     add_test(... "MinSizeRel/foo.exe")
#   elseif(... RelWithDebInfo ...)
#     add_test(... "RelWithDebInfo/foo.exe")
#   else()
#     add_test(... NOT_AVAILABLE)
#   endif()
#
# Two problems with that, from the caller's point of view:
#
# 1. Plain `ctest` (no `-C <config>`) leaves CTEST_CONFIGURATION_TYPE empty.
#    None of the four regex branches match, the else() branch fires, and the
#    test is registered as `NOT_AVAILABLE` — ctest then reports it as
#    "Not Run" and the suite fails even though every binary is present and
#    green.  gtest_discover_tests is unaffected (it emits its own
#    <target>[N]_tests.cmake with the already-resolved per-config path),
#    but every hand-written `add_test()` — the E2E / DTLS / plugin-loading
#    wrappers — becomes unrunnable.
#
# 2. The earlier revision of this script "solved" (1) by throwing away the
#    Release / MinSizeRel / RelWithDebInfo branches and keeping only the
#    Debug body.  That works for a Debug build (which is what CI does) but
#    breaks a Release build outright: every wrapper test then points at a
#    Debug/ path that does not exist, and ctest reports "Not Run" — the
#    same symptom, but now with the binaries sitting right there on disk.
#
# What this script does now
# --------------------------
# It keeps ALL FOUR branches verbatim, so `ctest -C <config>` resolves
# exactly as CMake intended, and it replaces the else() (no `-C`) body with
# an existence check that picks whichever configuration actually has its
# binaries on disk.  A plain `ctest` therefore finds the tests that were
# really built, and `ctest -C Release` finds the Release ones.
#
# It also stops mangling the `set_tests_properties(... _BACKTRACE_TRIPLES
# "a;b;c")` lines: those contain semicolons, and the previous revision
# split the file on `;` (the CMake list separator), which rewrote the
# backtrace triples into garbage.
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

# The four configuration branches CMake emits, in the order it emits them.
set(_nimrtc_configs Debug Release MinSizeRel RelWithDebInfo)

# Sentinel that cannot occur in a CMake-generated file; used to hide the
# semicolons inside _BACKTRACE_TRIPLES from CMake's list handling.
set(_nimrtc_semi "@NIMRTC_SEMICOLON@")

foreach(testfile IN LISTS testfiles)
    math(EXPR scanned "${scanned} + 1")

    file(READ "${testfile}" content)
    # Normalise line endings so the line-by-line walk is consistent.
    string(REPLACE "\r\n" "\n" content "${content}")
    # Hide semicolons so the line split below does not break the
    # _BACKTRACE_TRIPLES property apart.
    string(REPLACE ";" "${_nimrtc_semi}" content "${content}")

    # Quick reject: file has no CTEST_CONFIGURATION_TYPE conditional.
    string(FIND "${content}" "CTEST_CONFIGURATION_TYPE MATCHES" hit)
    if(hit EQUAL -1)
        math(EXPR unchanged "${unchanged} + 1")
        continue()
    endif()

    # Split into lines, preserving blank lines.  CMake list separators are
    # semicolons; we replace each newline with a sentinel then split.
    string(REPLACE "\n" "@NIMRTC_EOL@;" lines "${content}")

    set(out "")
    set(in_block FALSE)
    set(cur_branch "")

    foreach(line IN LISTS lines)
        if(in_block)
            # --- inside a conditional block -----------------------------
            if(line MATCHES "^elseif\\(CTEST_CONFIGURATION_TYPE MATCHES")
                # A new branch: stash the previous branch's header + body.
                set("b_hdr_${cur_branch}" "${b_hdr_tmp}")
                set("b_body_${cur_branch}" "${b_body_tmp}")
                set(b_hdr_tmp "")
                set(b_body_tmp "")
                # Which configuration is this branch?  CMake spells the
                # regex with character classes, so match on those.
                set(cur_branch "")
                if(line MATCHES "\\[Dd\\]\\[Ee\\]\\[Bb\\]\\[Uu\\]\\[Gg\\]")
                    set(cur_branch "Debug")
                elseif(line MATCHES "\\[Rr\\]\\[Ee\\]\\[Ll\\]\\[Ee\\]\\[Aa\\]\\[Ss\\]\\[Ee\\]")
                    set(cur_branch "Release")
                elseif(line MATCHES "\\[Mm\\]\\[Ii\\]\\[Nn\\]\\[Ss\\]\\[Ii\\]\\[Zz\\]\\[Ee\\]\\[Rr\\]\\[Ee\\]\\[Ll\\]")
                    set(cur_branch "MinSizeRel")
                elseif(line MATCHES "\\[Rr\\]\\[Ee\\]\\[Ll\\]\\[Ww\\]\\[Ii\\]\\[Tt\\]\\[Hh\\]\\[Dd\\]\\[Ee\\]\\[Bb\\]\\[Ii\\]\\[Nn\\]\\[Ff\\]\\[Oo\\]")
                    set(cur_branch "RelWithDebInfo")
                endif()
                string(APPEND b_hdr_tmp "${line}@NIMRTC_EOL@")
                continue()
            endif()

            if(line MATCHES "^else\\(\\)")
                # The catch-all branch: stash the previous branch first.
                set("b_hdr_${cur_branch}" "${b_hdr_tmp}")
                set("b_body_${cur_branch}" "${b_body_tmp}")
                set(b_hdr_tmp "")
                set(b_body_tmp "")
                set(cur_branch "Else")
                string(APPEND b_hdr_tmp "${line}@NIMRTC_EOL@")
                continue()
            endif()

            if(line MATCHES "^endif\\(\\)")
                # Close the block: emit the rewritten conditional.
                set("b_hdr_${cur_branch}" "${b_hdr_tmp}")
                set("b_body_${cur_branch}" "${b_body_tmp}")

                # The test name comes from the first branch (all four name
                # the same test).
                set(test_name "${b_name_Debug}")

                string(APPEND out "@NIMRTC_EOL@")
                foreach(cfg IN LISTS _nimrtc_configs)
                    if("${b_hdr_${cfg}}" STREQUAL "")
                        continue()
                    endif()
                    string(APPEND out "${b_hdr_${cfg}}")
                    string(APPEND out "${b_body_${cfg}}")
                endforeach()

                # Replace the NOT_AVAILABLE catch-all with an existence
                # probe over the four configurations, so a plain `ctest`
                # (no -C) resolves to the binaries that are on disk.
                string(APPEND out "else()@NIMRTC_EOL@")
                string(APPEND out "  # Plain `ctest` (no -C) leaves CTEST_CONFIGURATION_TYPE empty,@NIMRTC_EOL@")
                string(APPEND out "  # so none of the four branches above match.  Pick the@NIMRTC_EOL@")
                string(APPEND out "  # configuration whose binaries actually exist on disk.@NIMRTC_EOL@")
                set(first_cfg TRUE)
                foreach(cfg IN LISTS _nimrtc_configs)
                    set(cfg_path "${b_path_${cfg}}")
                    if(cfg_path STREQUAL "")
                        continue()
                    endif()
                    if(first_cfg)
                        string(APPEND out "  if(EXISTS \"${cfg_path}\")@NIMRTC_EOL@")
                        set(first_cfg FALSE)
                    else()
                        string(APPEND out "  elseif(EXISTS \"${cfg_path}\")@NIMRTC_EOL@")
                    endif()
                    string(APPEND out "    add_test([=[${test_name}]=] \"${cfg_path}\")@NIMRTC_EOL@")
                    set(props "${b_props_${cfg}}")
                    if(NOT props STREQUAL "")
                        string(APPEND out "    ${props}@NIMRTC_EOL@")
                    endif()
                endforeach()
                if(NOT first_cfg)
                    string(APPEND out "  endif()@NIMRTC_EOL@")
                endif()
                string(APPEND out "endif()@NIMRTC_EOL@")

                set(in_block FALSE)
                set(cur_branch "")
                set(b_hdr_tmp "")
                set(b_body_tmp "")
                continue()
            endif()

            # A body line: add_test(...) or set_tests_properties(...).
            # CMake indents the body of the conditional by two spaces.
            # The path is extracted by quoting rather than by matching the
            # whole add_test() shape — the test name may itself contain
            # brackets, which makes a single regex brittle.
            string(FIND "${line}" "add_test(" at_pos)
            if(NOT at_pos EQUAL -1)
                string(REGEX MATCH "\"([^\"]+)\"" _m "${line}")
                if(NOT _m STREQUAL "")
                    # The first quoted string after add_test( is the path.
                    string(REGEX REPLACE "^\"([^\"]+)\"$" "\\1" cfg_path "${_m}")
                    # The test name sits in [=[...]=]; take the first one.
                    string(REGEX MATCH "\\[=\\[([^]]*)\\]=\\]" _n "${line}")
                    if(NOT _n STREQUAL "")
                        string(REGEX REPLACE "^\\[=\\[([^]]*)\\]=\\]$" "\\1" cfg_name "${_n}")
                        set("b_name_${cur_branch}" "${cfg_name}")
                    endif()
                    set("b_path_${cur_branch}" "${cfg_path}")
                endif()
            endif()
            string(FIND "${line}" "set_tests_properties(" at_pos2)
            if(NOT at_pos2 EQUAL -1)
                set("b_props_${cur_branch}" "${line}")
            endif()
            string(APPEND b_body_tmp "${line}@NIMRTC_EOL@")
            continue()
        endif()

        # --- outside a conditional block -------------------------------
        if(line MATCHES "^if\\(CTEST_CONFIGURATION_TYPE MATCHES")
            set(in_block TRUE)
            set(cur_branch "Debug")
            set(b_hdr_tmp "")
            set(b_body_tmp "")
            string(APPEND b_hdr_tmp "${line}@NIMRTC_EOL@")
            continue()
        endif()

        string(APPEND out "${line}@NIMRTC_EOL@")
    endforeach()

    # A block that was never closed (should not happen) — keep the tail.
    if(in_block)
        string(APPEND out "${b_hdr_tmp}${b_body_tmp}")
    endif()

    # Restore the protected semicolons and the newlines.
    string(REPLACE "@NIMRTC_EOL@" "\n" out "${out}")
    string(REPLACE "${_nimrtc_semi}" ";" out "${out}")
    if(NOT out MATCHES "\n$")
        string(APPEND out "\n")
    endif()

    file(WRITE "${testfile}" "${out}")
    math(EXPR rewritten "${rewritten} + 1")
endforeach()

message(STATUS
    "FixCTestTestfile: scanned=${scanned} rewritten=${rewritten} unchanged=${unchanged}")
