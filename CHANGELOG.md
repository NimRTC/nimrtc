# Changelog

All notable changes to NimRTC are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/)
and uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- `src/` directory as canonical source layout
- `.clang-format`, `.editorconfig`, `.gitattributes` for consistent formatting
- `CMakePresets.json` for `cmake --preset` workflows
- `CONTRIBUTING.md` with DCO signing and Conventional Commits requirements
- `SECURITY.md` with private disclosure process
- `examples/`, `interop/`, `tools/`, `docs/adr/`, `docs/api/` placeholder directories
- `.github/` directory with CI workflow and issue/PR templates
- Module-level CMake options: `NIMRTC_MODULE_RTP`, `NIMRTC_MODULE_SDP`, `NIMRTC_MODULE_JB`
- `cmake/NimRTCOptions.cmake`: standardised C++20 flags and MSVC/GCC/Clang warnings
- `cmake/NimRTCVendored.cmake`: convenience wrappers for linking vendor libraries
- `cmake/NimRTCTest.cmake`: `nimrtc_add_test()` macro with GoogleTest integration

### Fixed
- CMake: `nimrtc_add_test` unknown command (moved `include(NimRTCTest)` before `add_subdirectory(modules)`)
- CMake: duplicate `DEPS` keyword in `cmake_parse_arguments`
- `.gitignore`: added `cmake-configure.log`, `.vs/`, `CMakeUserPresets.json`

### Changed
- `NIMRTC_FETCH_GTEST` is now initialised before `include(NimRTCTest)` to ensure
  GoogleTest is fetched on first configure without a stale cache
- `nimrtc_target_include_directories()` dead function removed from `NimRTCOptions.cmake`
- Vendor library target names standardised to `nimrtc::vendor::<name>` namespace

## [0.1.0] — P0 scaffold

### Added
- Project skeleton: `core/`, `modules/rtp/`, `modules/sdp/`, `modules/jb/`, `third_party/`
- Header-only `nimrtc::core` library: `time.hpp`, `error.hpp`, `bytes.hpp`, `log.hpp`
- `nimrtc::rtp::PacketView`, `Parser`, `PacketBuilder`, RTCP enums (declarations only)
- `nimrtc::sdp::SessionDescription`, `Parser`, `Munger` (declarations only)
- `nimrtc::jb::JitterBuffer`, `Frame`, `Config`, `Stats` (declarations only)
- Unit test scaffolding for RTP, SDP, and JB modules (GoogleTest, via FetchContent)
- Vendor stub targets: `nimrtc_vendor_libsrtp`, `nimrtc_vendor_libopus`, `nimrtc_vendor_mbedtls`

---

## Versioning scheme

NimRTC uses a three-part version number `MAJOR.MINOR.PATCH`:

- **MAJOR**: breaking changes to the public API (including ABI)
- **MINOR**: new backwards-compatible features or module additions
- **PATCH**: backwards-compatible bug fixes

Until v1.0.0, **MINOR** bumps indicate phase completions (P0, P1, P2…),
and **PATCH** bumps indicate internal fixes within a phase.
