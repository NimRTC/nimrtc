# src/sctp/CMakeLists.txt
# ----------------------------------------------------------------------------
# SCTP seam module (Transport PAL Slice 5 — SEAM ONLY).
#
# Per `docs/plan/transport-selection.md` §5.2 / §6.2:
#   - ISctpSocket / ISctpSocketFactory interface (the SEAM).
#   - SctpStubSocket / SctpStubFactory default impl (id="stub"), returning
#     plugins::kErrNotReady from every send method.
#   - register_default_plugins() entry point (Meyers-singleton latch).
#
# Slice 5 scope:
#   - Lay the seam ONLY — do NOT link usrsctp yet (the production default
#     impl lands in v0.11.0 per §7).
#   - The stub factory is registered under id "stub" (NOT "usrsctp") so
#     downstream code cannot accidentally believe the v0.11.0 backend is
#     already integrated — see sctp_stub_factory.cpp header rationale.
#
# v0.11.0 scope (NOT in this file):
#   - Add `UsrsctpSocket` / `UsrsctpSocketFactory` (id="usrsctp").
#   - Link `nimrtc_vendor_usrsctp` (vendored third_party).
#   - Extend SctpConfig with usrsctp-specific knobs (max_num_streams,
#     local_port, DTLS-SRTP key material hook, ...).
#
# Mirrors `src/modules/ice/CMakeLists.txt` structurally (static lib, one
# include root, enable_testing() + add_subdirectory(tests) when tests dir
# exists). The path is `src/sctp/` (NOT `src/modules/sctp/`) per the
# Slice 5 task brief and §6.2 of the plan — Slice 5 is a forward-looking
# module that will sit alongside the existing module tree.
#
# Dependencies:
#   - core (always; nimrtc_apply_options + log + (future) registry hooks)
#   - plugins (IDataChannel / IPluginFactory — base types)
# ----------------------------------------------------------------------------
add_library(nimrtc_sctp STATIC
    src/sctp_stub_socket.cpp
    src/sctp_stub_factory.cpp
    src/sctp_plugin.cpp
    # TPAL-5 (v0.11.0) — production usrsctp backend (id="usrsctp").
    # Sibling to the Slice 5 stub (id="stub"). The stub stays in place
    # so v0.10.x callers continue to work — TPAL-5 is additive.
    src/usrsctp_socket.cpp
    src/usrsctp_factory.cpp
    )

nimrtc_apply_options(nimrtc_sctp LENIENT)

target_link_libraries(nimrtc_sctp
    PUBLIC
        nimrtc::core
        nimrtc::plugins       # plugin base types (Status, BufferView, ...)
    PRIVATE
        # TPAL-5: usrsctp backend is only linked into the production
        # library, not re-exported (consumers must go through the
        # ISctpSocket seam — the factory pointer in the registry).
        nimrtc::vendor::usrsctp
    )

target_include_directories(nimrtc_sctp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_sctp PUBLIC NIMRTC_HAS_SCTP_SEAM=1)

add_library(nimrtc::sctp ALIAS nimrtc_sctp)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
