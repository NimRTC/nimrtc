# nlohmann_json 3.11.3 (single-header, vendored)
# ----------------------------------------------------------------------------
# Vendored copy of the upstream single_include header so that
# NIMRTC_MODULE_ASSEMBLY can build offline (no FetchContent / network).
#
# Upstream tarball: https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
# We only ship single_include/nlohmann/json.hpp (the umbrella header) so
# consumers can write `#include <nlohmann/json.hpp>` exactly as if they
# linked against the upstream build.
#
# We expose the same target names as upstream's CMake package so existing
# link lines (nlohmann_json::nlohmann_json or nlohmann::nlohmann_json) keep
# working unchanged.
# ----------------------------------------------------------------------------

add_library(nlohmann_json INTERFACE)
add_library(nlohmann::nlohmann_json ALIAS nlohmann_json)
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)

target_include_directories(nlohmann_json INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Header-only library; require C++20 (project-wide) so consumers can use
# std::span, concepts, etc. declared in the header.
target_compile_features(nlohmann_json INTERFACE cxx_std_20)

# v3.11.3 defaults are sufficient for the assembly module (no legacy
# discarded-value-comparison toggle needed). Add INTERFACE compile
# definitions here if a future consumer requires them.