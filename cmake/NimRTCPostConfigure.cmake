# NimRTCPostConfigure.cmake
# ----------------------------------------------------------------------------
# Post-configure hook dispatcher.
#
# Registers a custom target `fix-ctest-testfile` that strips CMake 4.0's
# broken CTEST_CONFIGURATION_TYPE conditional from CTestTestfile.cmake
# files. Without this, plain `ctest` (without `-C <config>`) marks every
# non-GTest wrapper test as NOT_AVAILABLE.
#
# Background
# ----------
# CMake 4.0's multi-config generator (VS, Xcode, Ninja Multi-Config) wraps
# every hand-written `add_test()` call in CTestTestfile.cmake with:
#
#   if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
#     add_test(... "Debug/foo.exe")
#   elseif(... Release ...)
#     ...
#   else()
#     add_test(... NOT_AVAILABLE)
#   endif()
#
# When ctest is invoked without `-C <config>` (e.g. plain `ctest -j4`),
# CTEST_CONFIGURATION_TYPE is empty inside the cmake -P context, none of
# the four regex branches match, and the test is registered as
# NOT_AVAILABLE. gtest_discover_tests is unaffected (it uses a POST_BUILD
# discovery command that doesn't depend on the variable).
#
# The script-mode invocation of cmake -P does not load CMakeCache.txt, so
# setting CTEST_CONFIGURATION_TYPE as a cache variable doesn't help — the
# conditional still sees an empty variable.
#
# Implementation
# --------------
# - The fixup is registered as a custom target `fix-ctest-testfile` that
#   is part of ALL_BUILD, so `cmake --build .` runs it automatically.
# - The script is idempotent (skips files already without the broken
#   token), so repeated builds are cheap (typically a no-op after the
#   first run).
# - We use cmake_language(DEFER ...) here only to surface a one-time
#   status message pointing the user at the target name; the actual
#   rewrite happens during `cmake --build .`.
# ----------------------------------------------------------------------------

# Capture paths at include time so they're stable when the build runs.
set(_nimrtc_post_configure_dir "${CMAKE_CURRENT_LIST_DIR}")

# Add the fixup as a global build step. ALL makes it run with the default
# `cmake --build .` invocation; the script is idempotent so the cost is
# negligible after the first run.
add_custom_target(fix-ctest-testfile ALL
    COMMAND "${CMAKE_COMMAND}" -P
        "${_nimrtc_post_configure_dir}/FixCTestTestfile.cmake"
        "${CMAKE_BINARY_DIR}"
    COMMENT "Stripping CMake 4.0 CTEST_CONFIGURATION_TYPE conditional from CTestTestfile.cmake"
    VERBATIM)

# One-shot info message at configure time, telling the user that the hook
# is in place.  cmake_language(DEFER) runs this at the end of the top-level
# CMakeLists.txt processing — early enough to print alongside the configure
# summary, late enough not to clutter early output.
function(nimrtc_post_configure_info)
    message(STATUS
        "Post-configure hook: target 'fix-ctest-testfile' registered (runs on every build to enable plain 'ctest')")
endfunction()
cmake_language(DEFER CALL nimrtc_post_configure_info)
