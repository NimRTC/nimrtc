# modules/rd_input_inject/CMakeLists.txt
# ----------------------------------------------------------------------------
# The OS input-injection module — remote-desktop plan Track G (the device
# side of the control plane).
#
# Scope:
#   - the platform-neutral key layout and its mapping from the
#     rd_input vocabulary's opaque key codes,
#   - the injection *state* (which keys / buttons are down), so the
#     dead-man switch (G6) can release everything, and
#   - the three platform backends:
#       Windows  -> SendInput   (user32)
#       macOS    -> CGEvent     (ApplicationServices)
#       Linux    -> XTest (libXtst) or uinput
#
# NOT in scope:
#   - the event *vocabulary* and the wire format (that is the rd_input
#     module -- this module consumes it),
#   - the safety filter (also rd_input -- G7), and
#   - the transport (the engine's control channel -- G1 / G2).
#
# Why a module and not a seam:
#   "It is a module, not a seam: the transport is DataChannel, and only
#   the payload format is NimRTC's" (plan G3).  The same reasoning
#   applies here: the injector is a *product* concern driven by the
#   example application, not an engine capability, so there is nothing
#   in src/plugins/ (AGENTS.md L2) and the engine never resolves it
#   through the registry.
#
# Dependencies:
#   - core     (always)  -- the error / log vocabulary
#   - rd_input (PUBLIC)  -- the event vocabulary this module injects
# ----------------------------------------------------------------------------
set(NIMRTC_RD_INPUT_INJECT_SOURCES
    src/input_injector.cpp
    src/injector_base.cpp
    src/key_map.cpp
    )

# ---------------------------------------------------------------------------
# The platform backend
#
# Each backend is compiled on exactly one platform family.  A platform
# with no backend still links the module -- the factory returns the
# portable stub (see src/input_injector.cpp).
# ---------------------------------------------------------------------------
if(WIN32)
    list(APPEND NIMRTC_RD_INPUT_INJECT_SOURCES
        src/platform/windows/windows_input.cpp)
elseif(APPLE)
    list(APPEND NIMRTC_RD_INPUT_INJECT_SOURCES
        src/platform/macos/macos_input.cpp)
    # The macOS backend uses Objective-C (the `NSString` transcoding in
    # `do_inject_text()`), so the translation unit is compiled as
    # Objective-C++ with ARC.  Without the dialect switch the compiler
    # treats the file as plain C++ and rejects the `#import` directives
    # ("#import is a language extension") and the Objective-C message
    # sends ("unknown type name 'NSString'").
    set_source_files_properties(src/platform/macos/macos_input.cpp PROPERTIES
        COMPILE_OPTIONS "-x;objective-c++;-fobjc-arc")
elseif(UNIX)
    list(APPEND NIMRTC_RD_INPUT_INJECT_SOURCES
        src/platform/linux/linux_input.cpp)
endif()

add_library(nimrtc_rd_input_inject STATIC
    ${NIMRTC_RD_INPUT_INJECT_SOURCES}
    )

nimrtc_apply_options(nimrtc_rd_input_inject)
target_link_libraries(nimrtc_rd_input_inject
    PUBLIC
        nimrtc::core
        nimrtc::rd_input
    )

target_include_directories(nimrtc_rd_input_inject
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        # The platform adapters include their siblings by relative path.
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(nimrtc_rd_input_inject
    PUBLIC NIMRTC_HAS_RD_INPUT_INJECT=1)

# ---------------------------------------------------------------------------
# The platform link libraries
#
# Both the XTest and the uinput paths are *optional* on Linux: a host
# with neither still builds the module (the stub is used), and the
# build must never fail for a missing optional dependency.
# ---------------------------------------------------------------------------
if(UNIX AND NOT APPLE)
    find_package(X11 QUIET)
    if(X11_FOUND)
        find_library(XTST_LIBRARY NAMES Xtst)
        if(XTST_LIBRARY)
            target_link_libraries(nimrtc_rd_input_inject PRIVATE ${XTST_LIBRARY})
            target_compile_definitions(nimrtc_rd_input_inject
                PRIVATE NIMRTC_HAS_XTEST=1)
            message(STATUS
                "rd_input_inject: the XTest injection backend is enabled")
        else()
            message(STATUS
                "rd_input_inject: the XTest injection backend is disabled "
                "(libXtst was not found)")
        endif()
    endif()

    # The uinput path needs the kernel's uapi headers, which every
    # Linux build host has.  It is enabled whenever the header is
    # present.
    include(CheckIncludeFile)
    check_include_file("linux/uinput.h" NIMRTC_HAS_UINPUT_HEADER)
    if(NIMRTC_HAS_UINPUT_HEADER)
        target_compile_definitions(nimrtc_rd_input_inject
            PRIVATE NIMRTC_HAS_UINPUT=1)
        message(STATUS
            "rd_input_inject: the uinput injection backend is enabled")
    else()
        message(STATUS
            "rd_input_inject: the uinput injection backend is disabled "
            "(linux/uinput.h was not found)")
    endif()
endif()

if(APPLE)
    target_link_libraries(nimrtc_rd_input_inject PRIVATE
        "-framework ApplicationServices"
        "-framework CoreGraphics"
        "-framework Foundation"
    )
endif()

add_library(nimrtc::rd_input_inject ALIAS nimrtc_rd_input_inject)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
