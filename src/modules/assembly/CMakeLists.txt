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
#   - nimrtc::engine   — ONLY through the `nimrtc::assembly_engine` INTERFACE
#       target below.  The Profile → EngineConfig bridge lives in
#       `include/nimrtc/assembly/engine_bridge.hpp`, and the engine is the
#       runtime the Profile configures — so the dependency arrow points
#       assembly -> engine (configuration above runtime), never the other
#       way round.  The engine never includes `assembly.hpp`, so Layout
#       Invariant 4 / ADR-021 is untouched.
#
# NOTE: `nimrtc::transport` is deliberately NOT a dependency of this
# target.  `include/nimrtc/assembly/transport_bridge.hpp` needs the
# transport module's *include path*, but the adapter it provides is
# entirely header-only (`strategy_by_name()` and
# `requirements_from_profile_input()` are `inline`), so no transport
# *symbol* is required.  Linking the transport module here would drag
# the DTLS / SCTP / raw-UDP seams (and wolfSSL) into every consumer of
# `nimrtc::assembly` — including the embedded hosts that only want to
# *read* a Profile.  The include path is supplied by
# `nimrtc::assembly_engine` instead.
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

# --- The engine-facing bridge ------------------------------------------------
#
# `include/nimrtc/assembly/engine_bridge.hpp` maps a `Profile` onto the
# engine's `EngineConfig`.  It is header-only, so it needs no compiled
# sources — but it DOES need the engine's include path, and a host that
# calls `to_engine_config()` needs the engine's symbols at link time.
#
# Why a separate INTERFACE target instead of folding `nimrtc::engine` into
# `nimrtc_assembly`:
#
#   - `nimrtc::assembly` stays light.  An embedded host that only wants to
#     *read* a Profile (print it, diff it, ship it to a remote agent) must
#     not be forced to link the ICE / DTLS / SRTP stack.
#   - The dependency direction stays honest: the engine is the runtime the
#     Profile configures, so the arrow points assembly -> engine.  The
#     engine never includes `assembly.hpp` (Layout Invariant 4 / ADR-021).
#
# Note on CMake ordering: `src/modules/` is added BEFORE `src/engine/` in
# the root CMakeLists.txt, so `nimrtc::engine` is a forward reference here.
# CMake resolves target names at generate time, so this works — the same
# pattern the engine itself uses for `nimrtc::transport` and friends.
add_library(nimrtc_assembly_engine INTERFACE)

target_include_directories(nimrtc_assembly_engine
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(nimrtc_assembly_engine
    INTERFACE
        nimrtc::assembly
        nimrtc::engine
        # The transport module's *include path* is needed by
        # `include/nimrtc/assembly/transport_bridge.hpp` (the
        # TransportSection -> ProfileTransportInput adapter).  The
        # adapter itself is header-only, so this link exists purely to
        # propagate the include path — it pulls no extra symbol into the
        # consumer.
        nimrtc::transport)

add_library(nimrtc::assembly_engine ALIAS nimrtc_assembly_engine)

# --- Tests ------------------------------------------------------------------
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
