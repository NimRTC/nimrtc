# modules/rd_session/CMakeLists.txt
# ----------------------------------------------------------------------------
# The remote-desktop session-guard module.
#
# Scope:
#   - the portable contract: "is the interactive desktop session
#     currently capturable?" (three states: usable / locked / unknown),
#   - the operator-facing wording for each state, and
#   - the Windows backend: the input-desktop probe and the
#     console-session recovery.
#
# NOT in scope:
#   - the capture itself (that is the `video_capture` module),
#   - the input injection (that is the `rd_input_inject` module), and
#   - the "is the capture working?" question (that is the capture
#     backend's own `last_error_message()`, added in the same change
#     that brought this module in).
#
# Why a module and not a seam (same argument as `rd_input_inject`):
#   This is a *product* concern driven by the example application, not
#   an engine capability.  The engine never resolves it through the
#   registry and never names it; the device-side example does.
#
# Dependencies:
#   - core (always) -- the error / log vocabulary
# ----------------------------------------------------------------------------
set(NIMRTC_RD_SESSION_SOURCES
    src/session_guard.cpp
    )

# ---------------------------------------------------------------------------
# The platform backend
#
# The Windows backend is compiled on Windows only.  Every other platform
# gets the portable dispatch, which reports "usable" -- the Winlogon
# input-desktop failure mode does not exist there.
# ---------------------------------------------------------------------------
if(WIN32)
    list(APPEND NIMRTC_RD_SESSION_SOURCES
        src/platform/windows/windows_session_guard.cpp)
endif()

add_library(nimrtc_rd_session STATIC
    ${NIMRTC_RD_SESSION_SOURCES}
    )

nimrtc_apply_options(nimrtc_rd_session)
target_link_libraries(nimrtc_rd_session
    PUBLIC
        nimrtc::core
    )

target_include_directories(nimrtc_rd_session
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        # The platform adapter includes its sibling by relative path.
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(nimrtc_rd_session PUBLIC NIMRTC_HAS_RD_SESSION=1)

# The Windows backend drives the terminal-services tools (`tsdiscon.exe`
# / `tscon.exe`) as helper processes, and reads the process list through
# the tool-help API.
if(WIN32)
    target_link_libraries(nimrtc_rd_session PRIVATE
        wtsapi32
        kernel32
        user32
        )
endif()

add_library(nimrtc::rd_session ALIAS nimrtc_rd_session)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
