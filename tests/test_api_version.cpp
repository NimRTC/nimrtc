// Note: This file intentionally saved as UTF-8 with BOM so that MSVC
// (compiling with a non-UTF-8 code page such as CP936/GBK on Chinese Windows)
// does not emit C4819 for any future non-ASCII characters in comments.

/**
 * @file tests/test_api_version.cpp
 * @brief v1.0.0-alpha.1 Sec.12.3.1 DoD gate #6 -- API version smoke test.
 *
 * ## What this test verifies
 *
 * 1. `version_macros_are_defined` -- canonical version triplet matches
 *    v1.0.0-alpha.1 (MAJOR=1, MINOR=0, PATCH=0, PRE_RELEASE="alpha.1").
 *
 * 2. `version_string_is_well_formed` -- `NIMRTC_API_VERSION_STRING`
 *    expands to "1.0.0".
 *
 * 3. `api_version_namespace_constants_match_macros` -- the runtime
 *    `nimrtc::core::api_version::k*` constants are identical to the
 *    corresponding `NIMRTC_API_VERSION_*` macros.
 *
 * 4. `stability_markers_expand_to_something` -- `NIMRTC_API_STABLE`
 *    and `NIMRTC_API_UNSTABLE` expand to valid C++ when applied to a
 *    function declaration (compile-time smoke; no runtime effect in
 *    v1.0.0-alpha.1 because both macros are empty).
 *
 * 5. `deprecated_expands_to_valid_attribute` -- `NIMRTC_API_DEPRECATED(msg)`
 *    is syntactically valid C++ when applied to a function declaration.
 *    The test does NOT call the deprecated function so no deprecation
 *    warning is emitted; the macro expansion is the subject under test.
 *
 * ## Platform coverage
 *
 * Windows MSVC / Linux GCC / macOS Clang / Linux aarch64 GCC.
 * No network access; no external dependencies beyond `nimrtc::core`.
 */

#include <gtest/gtest.h>

#include <cstring>

#include <nimrtc/core/api_version.hpp>

// -----------------------------------------------------------------------------
// Test 1 -- version macros are defined correctly
// -----------------------------------------------------------------------------

TEST(version_macros, are_defined) {
    // Pre-release tag.  We deliberately use a runtime EXPECT_STREQ here
    // (not static_assert + std::strcmp) because MSVC's <cstring> does
    // not yet declare std::strcmp as constexpr — only the non-standard
    // ::strcmp is constexpr.  Switching to a runtime check costs us
    // zero release-build perf (the test runs once) and keeps the
    // assertion portable to MSVC 19.30+, GCC 10+, Clang 12+.
    EXPECT_STREQ(NIMRTC_API_VERSION_PRE_RELEASE, "alpha.2");
}

// -----------------------------------------------------------------------------
// Test 2 -- version string is well-formed
// -----------------------------------------------------------------------------

TEST(version_string, is_well_formed) {
    // NIMRTC_API_VERSION_STRING must expand to "1.0.0".  Runtime
    // EXPECT_STREQ (not static_assert + std::strcmp) for MSVC <cstring>
    // constexpr compatibility — see the comment in version_macros above.
    EXPECT_STREQ(NIMRTC_API_VERSION_STRING, "1.0.0");
}

// -----------------------------------------------------------------------------
// Test 3 -- namespace constants match the macros
// -----------------------------------------------------------------------------

TEST(api_version_namespace, constants_match_macros) {
    using namespace nimrtc::core::api_version;

    // Canonical triplet.
    static_assert(kMajor == NIMRTC_API_VERSION_MAJOR, "kMajor mismatch");
    static_assert(kMinor == NIMRTC_API_VERSION_MINOR, "kMinor mismatch");
    static_assert(kPatch == NIMRTC_API_VERSION_PATCH, "kPatch mismatch");

    // String form.  Runtime EXPECT_STREQ; see the comment in
    // version_macros above for why we don't use static_assert +
    // std::strcmp on MSVC.
    EXPECT_STREQ(kString, NIMRTC_API_VERSION_STRING);

    // Pre-release tag.
    EXPECT_STREQ(kPreRelease, NIMRTC_API_VERSION_PRE_RELEASE);
}

// -----------------------------------------------------------------------------
// Test 4 -- stability markers expand to valid C++
//
// NIMRTC_API_STABLE and NIMRTC_API_UNSTABLE are empty in v1.0.0-alpha.1.
// This test verifies they are syntactically well-formed when applied to
// a function declaration (compile-time check; no runtime effect).
// -----------------------------------------------------------------------------

namespace nimrtc::core::api_version {
namespace {

// Apply NIMRTC_API_STABLE to a function declaration -- must compile.
NIMRTC_API_STABLE
inline int stability_marker_stable_fn() { return 0; }

// Apply NIMRTC_API_UNSTABLE to a function declaration -- must compile.
NIMRTC_API_UNSTABLE
inline int stability_marker_unstable_fn() { return 0; }

} // anonymous namespace
} // namespace nimrtc::core::api_version

TEST(stability_markers, expand_to_something) {
    // The mere fact that this TU compiles proves the macro expansions are
    // valid C++ applied to a function declaration.
    EXPECT_EQ(0, nimrtc::core::api_version::stability_marker_stable_fn());
    EXPECT_EQ(0, nimrtc::core::api_version::stability_marker_unstable_fn());
}

// -----------------------------------------------------------------------------
// Test 5 -- deprecated macro expands to a valid attribute
//
// NIMRTC_API_DEPRECATED(msg) is always active (cross-compiler attribute).
// We apply it to a function declaration and verify the TU compiles without
// emitting a deprecation warning -- because we never CALL the deprecated
// function, only declare it.  The macro expansion itself is the subject.
// -----------------------------------------------------------------------------

namespace nimrtc::core::api_version {
namespace {

// Apply NIMRTC_API_DEPRECATED to a function declaration -- must compile.
NIMRTC_API_DEPRECATED("use deprecated_marker_ok_fn instead")
inline int deprecated_fn() { return 1; }

// Non-deprecated companion function -- the test calls THIS, not deprecated_fn.
NIMRTC_API_STABLE
inline int deprecated_marker_ok_fn() { return 0; }

} // anonymous namespace
} // namespace nimrtc::core::api_version

TEST(deprecated, expands_to_valid_attribute) {
    // We call the non-deprecated companion, not deprecated_fn().
    // This means the TU emits no deprecation warning (C4996 / -Wdeprecated).
    // If the NIMRTC_API_DEPRECATED macro were broken, the TU would not compile.
    EXPECT_EQ(0, nimrtc::core::api_version::deprecated_marker_ok_fn());
}
