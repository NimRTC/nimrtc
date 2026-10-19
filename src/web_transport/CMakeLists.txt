# src/web_transport/CMakeLists.txt
# ----------------------------------------------------------------------------
# WebTransport / QUIC seam module (v0.14.0-beta.1, ADR-014).
#
# Scope (v0.14.0-beta.1):
#   - WebTransportMsQuicSession (IWebTransportSession impl backed by msquic)
#   - WebTransportMsQuicSessionFactory (IWebTransportSessionFactory, id="msquic")
#   - register_default_plugins() entry point (Meyers-singleton latch)
#
# Dependencies:
#   - core  (always; log + registry)
#   - plugins (header-only; plugins::Status used in seam return types)
#   - nimrtc::vendor::msquic  (QUIC C library — git submodule)
#
# Architectural role (per docs/plan/v0.14-plan.md §2.1.1 WT-SEAM):
#   This module stands up the v0.14.0-alpha.2 seam (`IWebTransportSession`
#   + companion headers) with a production-quality msquic-backed
#   implementation.  The seam is the v1.0.0 data-plane re-design
#   (docs/plan/v0.11-plan.md §2.2 + §10): SCTP-over-DTLS is deprecated
#   by the Chrome team in favour of WebTransport / QUIC; v0.14.0 cuts
#   the seam here and the msquic-backed implementation in beta.1.
# ----------------------------------------------------------------------------

set(_wt_srcs
    src/web_transport_msquic_session.cpp
    src/web_transport_data_channel.cpp
    src/web_transport_plugin.cpp
    )

add_library(nimrtc_web_transport STATIC
    ${_wt_srcs}
    )

nimrtc_apply_options(nimrtc_web_transport LENIENT)

target_link_libraries(nimrtc_web_transport
    PUBLIC
        nimrtc::core
        nimrtc::plugins       # plugins::Status
    PRIVATE
        # Link against the msquic vendor library.  The
        # vendored CMake target is exposed as nimrtc::vendor::msquic
        # by src/third_party/msquic/CMakeLists.txt.  FindMsQuic.cmake
        # (cmake/FindMsQuic.cmake) handles the system-install path
        # for out-of-tree consumers.
        nimrtc::vendor::msquic
    )

target_include_directories(nimrtc_web_transport
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

# msquic API is C, so we need to ensure extern "C" is in scope.
# The msquic header handles this internally, but we also need
# to set the C++ standard to at least C++17 for std::span etc.
target_compile_features(nimrtc_web_transport PRIVATE cxx_std_17)

add_library(nimrtc::web_transport ALIAS nimrtc_web_transport)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
