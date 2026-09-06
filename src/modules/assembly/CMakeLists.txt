# modules/assembly/CMakeLists.txt
# ----------------------------------------------------------------------------
# Assembly module — scenario assembly and profile registry.
#
# Provides:
#   - Profile struct: per-scenario configuration bundle
#   - ProfileRegistry: global singleton that registers all built-in profiles
#   - Builder: fluent API to load a named profile and override individual fields
#   - profiles/ directory: declarative JSON profiles for call/live/transport/agent
#
# Design (NimRTC-V2 技术文档 §2.6, §5):
#   - Assembly is the P1 skeleton that binds together all plugin seams.
#   - ProfileRegistry is the single source of truth for profile definitions.
#   - Builder is the API surface used by NimRTCEngine at construction time.
#
# Dependencies:
#   - nimrtc::core     — always (error types, registry patterns)
#   - nimrtc::plugins  — always (interface lookups driven by profile names)
#   - nlohmann_json    — for JSON profile loading (FetchContent from root CMakeLists.txt)
# ----------------------------------------------------------------------------

# --- Library target ---------------------------------------------------------
add_library(nimrtc_assembly STATIC
    src/assembly.cpp)

nimrtc_apply_options(nimrtc_assembly)

target_link_libraries(nimrtc_assembly
    PUBLIC
        nimrtc::core
        nimrtc::plugins
    PRIVATE
        nlohmann_json::nlohmann_json
    )

target_include_directories(nimrtc_assembly
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

# Copy JSON profile files into the build directory so they are loadable at
# runtime via relative paths (e.g. from the source tree or an installed copy).
# Ensure target directory exists — configure_file's COPYONLY mode doesn't
# create missing parent directories on fresh builds.
# Source layout: src/modules/assembly/CMakeLists.txt → profiles/ is at the
# repo root, i.e. ../../../profiles/ from this file.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/profiles")
file(GLOB profile_jsons "${CMAKE_CURRENT_SOURCE_DIR}/../../../profiles/*.json")
foreach(json_file IN LISTS profile_jsons)
    get_filename_component(json_name "${json_file}" NAME)
    configure_file(
        "${json_file}"
        "${CMAKE_BINARY_DIR}/profiles/${json_name}"
        COPYONLY)
endforeach()

add_library(nimrtc::assembly ALIAS nimrtc_assembly)

# --- Tests ------------------------------------------------------------------
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
