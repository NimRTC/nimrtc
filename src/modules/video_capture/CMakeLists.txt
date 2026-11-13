# modules/video_capture/CMakeLists.txt
# ----------------------------------------------------------------------------
# The capture-device layer (remote-desktop plan A1 / A2 / A2b / A3).
#
# This module owns the *portable* half of screen capture:
#
#   - the bounded frame queue with its explicit drop policy,
#   - the dirty-rectangle model,
#   - the format-negotiation contract,
#   - and the synthetic device that exercises all of it on every CI leg.
#
# The platform backends are thin adapters over this mechanism; they live
# in src/modules/video_capture/src/platform/ and are added here as they
# land (M1-c):
#
#   - Windows: DXGI Desktop Duplication + Windows Graphics Capture
#     (src/platform/windows/) -- built only on WIN32.
#
# Per Layout Invariant 3 (AGENTS.md): the module is self-contained --
# own CMakeLists.txt, include/, src/, tests/ -- and depends only on
# nimrtc::core and nimrtc::plugins (the seams it implements).
# ----------------------------------------------------------------------------
set(NIMRTC_VIDEO_CAPTURE_SOURCES
    src/synthetic_capture.cpp
    )

# ---------------------------------------------------------------------------
# Windows capture backends (remote-desktop plan A3, milestone M1-c)
#
# DXGI Desktop Duplication needs d3d11 + dxgi; Windows Graphics Capture
# additionally needs the WinRT projection headers (cppwinrt) and
# windowsapp.lib for the activation factory.
# ---------------------------------------------------------------------------
if(WIN32)
    list(APPEND NIMRTC_VIDEO_CAPTURE_SOURCES
        src/platform/windows/d3d11_support.cpp
        src/platform/windows/dxgi_desktop_duplication.cpp
        src/platform/windows/wgc_capture.cpp
        src/platform/windows/windows_capture.cpp
        )
endif()

add_library(nimrtc_video_capture STATIC
    ${NIMRTC_VIDEO_CAPTURE_SOURCES}
    )

nimrtc_apply_options(nimrtc_video_capture)
target_link_libraries(nimrtc_video_capture
    PUBLIC
        nimrtc::core
        nimrtc::plugins
    )

target_include_directories(nimrtc_video_capture
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_video_capture
    PUBLIC NIMRTC_HAS_VIDEO_CAPTURE=1)

if(WIN32)
    # d3d11 / dxgi  -- the Desktop Duplication + D3D11 surface path.
    # windowsapp    -- the WinRT activation factory used by WGC.
    # d3d11.lib's DXGI surface access (IDirect3DDxgiInterfaceAccess) is
    # declared in the SDK header <windows.graphics.directx.direct3d11.interop.h>
    # and resolves against d3d11.lib.
    target_link_libraries(nimrtc_video_capture
        PRIVATE
            d3d11
            dxgi
            windowsapp
        )

    # The C++/WinRT projection headers ship with the Windows SDK but are
    # NOT on the default include path -- MSBuild only adds the UM /
    # shared / winrt directories.  They live in the SDK's `cppwinrt`
    # directory; find it through the SDK root.
    #
    # Three sources are tried, in order:
    #   1. the `WindowsSdkDir` environment variable (set inside a Visual
    #      Studio developer prompt),
    #   2. the registry key the Windows SDK installer writes
    #      (`HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots` ->
    #      `KitsRoot10`),
    #   3. the default install locations.
    if(DEFINED ENV{WindowsSdkDir} AND IS_DIRECTORY "$ENV{WindowsSdkDir}")
        set(_nimrtc_sdk_root "$ENV{WindowsSdkDir}")
    else()
        set(_nimrtc_sdk_root "")
        find_program(_nimrtc_reg_exe reg)
        if(_nimrtc_reg_exe)
            execute_process(
                COMMAND "${_nimrtc_reg_exe}" query
                        "HKLM\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots"
                        /v KitsRoot10
                OUTPUT_VARIABLE _nimrtc_reg_output
                ERROR_QUIET
                RESULT_VARIABLE _nimrtc_reg_result
                OUTPUT_STRIP_TRAILING_WHITESPACE
                )
            if(_nimrtc_reg_result EQUAL 0)
                # The answer is on the last line:
                #     KitsRoot10    REG_SZ    D:\Windows Kits\10\
                # Collapse the output to one line first (a CMake regex
                # does not match across a newline), then take the value.
                string(REPLACE "\n" " " _nimrtc_reg_flat "${_nimrtc_reg_output}")
                string(REGEX REPLACE ".*KitsRoot10[ \t]+REG_SZ[ \t]+"
                       "([^ \t\r\n]+).*" "\\1"
                       _nimrtc_sdk_root "${_nimrtc_reg_flat}")
            endif()
        endif()
        if(NOT _nimrtc_sdk_root)
            foreach(_candidate
                    "C:/Program Files (x86)/Windows Kits/10"
                    "C:/Program Files/Windows Kits/10"
                    "D:/Windows Kits/10")
                if(IS_DIRECTORY "${_candidate}/Include")
                    set(_nimrtc_sdk_root "${_candidate}")
                    break()
                endif()
            endforeach()
        endif()
    endif()

    if(_nimrtc_sdk_root)
        file(GLOB _nimrtc_cppwinrt_dirs
             "${_nimrtc_sdk_root}/Include/*/cppwinrt")
        if(_nimrtc_cppwinrt_dirs)
            list(GET _nimrtc_cppwinrt_dirs 0 _nimrtc_cppwinrt_dir)
            target_include_directories(nimrtc_video_capture
                PRIVATE "${_nimrtc_cppwinrt_dir}")
            message(STATUS
                "video_capture: using the C++/WinRT headers in "
                "${_nimrtc_cppwinrt_dir}")
        else()
            message(WARNING
                "video_capture: no C++/WinRT headers found under "
                "${_nimrtc_sdk_root}/Include -- the WGC backend will not "
                "compile.")
        endif()
    else()
        message(WARNING
            "video_capture: the Windows SDK root could not be located -- "
            "the WGC backend will not compile.")
    endif()
endif()

add_library(nimrtc::video_capture ALIAS nimrtc_video_capture)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
