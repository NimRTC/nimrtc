# modules/dtls-seam/CMakeLists.txt
# ----------------------------------------------------------------------------
# DTLS seam module (PAL Slice 4) — `IDtlsSession` + `IDtlsSessionFactory`
# plus the default `WolfsslDtlsFactory` (id = "wolfssl") and a
# `register_default_plugins()` entry point.
#
# Scope (v0.11.0 PAL Slice 4):
#   - IDtlsSession interface (pure virtual)
#   - IDtlsSessionFactory interface (pure virtual)
#   - nimrtc::dtls::register_default_plugins() idempotent entry point
#   - Default backend: WolfsslDtlsFactory (delegates to the existing
#     DtlsSessionWolfSSL in src/modules/dtls/ which has been refactored
#     in place to inherit IDtlsSession)
#
# Architectural role (per docs/plan/transport-selection.md §5.2 / §6.1):
#   The seam is the first slice of transport-layer plugin-replaceability
#   for NimRTC.  Slice 5 (SCTP), Slice 6 (raw UDP bypass), Slice 7 (Stack
#   + Selector), and Slice 8 (engine integration) build on top of this
#   DTLS seam.  Slice 4 itself does not change core::PluginRegistry or
#   engine.cpp — registration is seam-local until Slice 7 introduces the
#   registry slot.
#
# Dependencies:
#   - core (always)
#   - plugins (header-only; plugins::Status used in seam return types)
#   - nimrtc::dtls (the existing concrete module — for DtlsSessionWolfSSL)
#   - nimrtc::vendor::wolfssl (transitive via nimrtc::dtls; declared here
#     so the seam's tests link the same crypto backend)
# ----------------------------------------------------------------------------
add_library(nimrtc_dtls_seam STATIC
    src/dtls_wolfssl_factory.cpp
    src/dtls_plugin.cpp
    )

nimrtc_apply_options(nimrtc_dtls_seam)

target_link_libraries(nimrtc_dtls_seam
    PUBLIC
        nimrtc::core
        nimrtc::plugins       # plugins::Status
        nimrtc::dtls          # DtlsSessionWolfSSL (refactored to inherit IDtlsSession)
    )

# Same compile definitions as the concrete dtls module — keeps wolfSSL
# config in lockstep so a future build of either target sees the same
# cipher list / EMS / KEEP_PEER_CERT flags.  These definitions are
# PUBLIC on nimrtc_dtls already; we still mirror them locally so a
# consumer that links only the seam (rare) gets a consistent build.
target_compile_definitions(nimrtc_dtls_seam
    PUBLIC
        NIMRTC_HAS_DTLS=1
        NIMRTC_USE_WOLFSSL_DTLS=1
    )

target_include_directories(nimrtc_dtls_seam
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

add_library(nimrtc::dtls_seam ALIAS nimrtc_dtls_seam)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
