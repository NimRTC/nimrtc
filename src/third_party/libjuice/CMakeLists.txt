# third_party/libjuice/CMakeLists.txt
# =============================================================================
# libjuice — UDP Interactive Connectivity Establishment (ICE) library.
#
# Populated by:
#   git clone --depth 1 https://github.com/paullouisageneau/libjuice.git \
#       third_party/libjuice/src
#
# Upstream version: libjuice 1.6.0 (vendored commit: see src/.git).
#
# Target produced: nimrtc_vendor_libjuice (INTERFACE alias for upstream `juice`).
# Static linkage is enforced so libjuice is compiled into nimrtc and never
# dynamically linked. Consumers see include dir + compile definitions only.
# =============================================================================

set(_upstream_dir "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Upstream uses BUILD_SHARED_LIBS=ON by default; force a static build so
# libjuice symbols are pulled into nimrtc_ice (and ultimately into consumers)
# without producing a DLL we would then need to ship.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(NO_SERVER   ON  CACHE BOOL "" FORCE)   # we only need the client agent
set(NO_TESTS    ON  CACHE BOOL "" FORCE)   # upstream test programs not built
set(FUZZER      OFF CACHE BOOL "" FORCE)
set(CLANG_TIDY  OFF CACHE BOOL "" FORCE)
set(WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)

# Delegate to upstream CMakeLists.txt, which produces the `juice` STATIC target.
add_subdirectory(${_upstream_dir} ${CMAKE_BINARY_DIR}/_vendor_libjuice_build)

# Wrap the upstream target in an INTERFACE library so consumers only see the
# public NimRTC conventions (nimrtc_vendor_libjuice + nimrtc::vendor::libjuice).
add_library(nimrtc_vendor_libjuice INTERFACE)
add_library(nimrtc::vendor::libjuice ALIAS nimrtc_vendor_libjuice)

target_link_libraries(nimrtc_vendor_libjuice INTERFACE juice)
target_include_directories(nimrtc_vendor_libjuice
    INTERFACE
        $<BUILD_INTERFACE:${_upstream_dir}/include>
)

target_compile_definitions(nimrtc_vendor_libjuice
    INTERFACE
        NIMRTC_USE_LIBJUICE=1
        JUICE_STATIC=1
)
