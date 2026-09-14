# docs/api/CMakeLists.txt
# =============================================================================
# Doxygen API documentation target.
#
# Activated by setting `-DNIMRTC_BUILD_DOCS=ON` at configure time.  The
# project's top-level CMakeLists already does `find_package(Doxygen)`
# and only adds this subdirectory when the tool is found, so it is safe
# to assume Doxygen is available here.
#
# We do NOT commit a Doxyfile (the canonical configuration is part of
# the developer's environment / CI). To generate the API reference:
#
#     doxygen Doxyfile    # writes HTML into this directory
#
# The `nimrtc::docs` target below lets CI track whether docs generated
# cleanly via a CMake build dependency (optional).
# =============================================================================

# Generate the doxygen configuration template once via configure_file().
# We use configure_file so the project SOURCE_DIR / BUILD_DIR are
# substituted at configure-time, keeping the config reproducible across
# platforms.
if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in")
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in"
            "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            @ONLY)
    endif()
endif()

# Only add the target if we successfully produced a Doxyfile.
if(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile")
    add_custom_target(nimrtc_docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Building Doxygen API documentation"
        VERBATIM)
else()
    message(STATUS
        "docs/api: no Doxyfile.in found; skipping target. "
        "Run 'doxygen Doxyfile' manually to populate ${CMAKE_CURRENT_SOURCE_DIR}/")
endif()
