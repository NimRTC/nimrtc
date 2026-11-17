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
#   - Lay the seam ONLY — do NOT link a production SCTP stack.
#   - The stub factory is registered under id "stub" (NOT "usrsctp") so
#     downstream code cannot accidentally believe a production backend is
#     already integrated — see sctp_stub_factory.cpp header rationale.
#
# v0.11.0 scope:
#   - The usrsctp backend (`usrsctp_socket.cpp`, `usrsctp_factory.cpp`) was
#     built into `nimrtc_sctp.lib` but was never published into
#     `core::PluginRegistry` (see `docs/plan/v0.11-plan.md` §2.2). It
#     survived as dead weight after the "sctp" DataChannel backend was
#     retired from the public selector in v1.2.0 (WP-A / DEC-4 = A1).
#
# v1.2.0 (plan E7) — usrsctp removed:
#   - `usrsctp_socket.cpp` / `usrsctp_factory.cpp` and their two
#     `@deprecated` public headers are deleted, along with the vendored
#     `src/third_party/usrsctp/` tree and the `nimrtc_vendor_usrsctp`
#     target. The public selector can no longer reach a usrsctp backend,
#     so keeping the sources served no one.
#   - The seam itself is unchanged: `ISctpSocket` / `ISctpSocketFactory`
#     stay, and the stub factory stays the only registered SCTP id
#     ("stub"). The production data plane is WebTransport over QUIC.
#
# Mirrors `src/modules/ice/CMakeLists.txt` structurally (static lib, one
# include root, enable_testing() + add_subdirectory(tests) when tests dir
# exists).
#
# Dependencies:
#   - core (always; nimrtc_apply_options + log + (future) registry hooks)
#   - plugins (IDataChannel / IPluginFactory — base types)
# ----------------------------------------------------------------------------
add_library(nimrtc_sctp STATIC
    src/sctp_stub_socket.cpp
    src/sctp_stub_factory.cpp
    src/sctp_plugin.cpp
    src/dcep.cpp
    )

nimrtc_apply_options(nimrtc_sctp LENIENT)

target_link_libraries(nimrtc_sctp
    PUBLIC
        nimrtc::core
        nimrtc::plugins       # plugin base types (Status, BufferView, ...)
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
