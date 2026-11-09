// This file is deliberately pure ASCII so that MSVC cannot emit C4819
// ("file contains a character that cannot be represented in the current
// code page") for any comment, no matter which code page the build runs
// under -- CP936/GBK on Chinese Windows is the case that has bitten this
// tree before.  Keep it that way: do not introduce non-ASCII characters
// here.

/**
 * @file tests/test_api_version.cpp
 * @brief Public API version smoke test.
 *
 * ## What this test verifies
 *
 * 1. `version_macros.are_defined` -- the canonical version triplet in
 *    `nimrtc/core/api_version.hpp` matches the triplet the release
 *    process has pinned in `tests/api_version_expectations.hpp`.
 *
 * 2. `version_macros.encoded_version_is_correct` -- `NIMRTC_API_VERSION`
 *    equals the pinned encoded version (MAJOR*10000 + MINOR*100 + PATCH).
 *
 * 3. `version_string.is_well_formed` -- `NIMRTC_API_VERSION_STRING`
 *    expands to the pinned "MAJOR.MINOR.PATCH" string.
 *
 * 4. `api_version_namespace.constants_match_macros` -- the runtime
 *    `nimrtc::core::api_version::k*` constants are identical to the
 *    corresponding `NIMRTC_API_VERSION_*` macros.
 *
 * 5. `stability_markers.expand_to_something` -- `NIMRTC_API_STABLE`
 *    and `NIMRTC_API_UNSTABLE` expand to valid C++ when applied to a
 *    function declaration (compile-time smoke; no runtime effect,
 *    because both macros are empty).
 *
 * 6. `deprecated.expands_to_valid_attribute` -- `NIMRTC_API_DEPRECATED(msg)`
 *    is syntactically valid C++ when applied to a function declaration.
 *    The test does NOT call the deprecated function so no deprecation
 *    warning is emitted; the macro expansion is the subject under test.
 *
 * 7. `version_check.works_correctly` -- `NIMRTC_API_VERSION_CHECK(major, minor, patch)`
 *    evaluates correctly for the current version, for earlier versions,
 *    and for later versions.
 *
 * ## Why this file has no version number in it
 *
 * The test used to hardcode the version ("1.1.0", 10100, "").  Every
 * release then had to edit this file by hand, and the `v1.1.0` ->
 * `v1.1.1` bump (`059cc29`) forgot to: the resulting red CI showed up
 * as a `static_assert` failure in four platform builds, which says
 * nothing about the real cause.
 *
 * The pinned version now lives in one generated file,
 * `tests/api_version_expectations.hpp`, which `tools/bump_version.py`
 * writes together with the header and `CMakeLists.txt`.  The checks
 * below are written relative to that pin, so a version bump no longer
 * touches this file at all -- and a version changed *without* going
 * through the script still fails here, which is the whole point of the
 * pin.
 *
 * ## Platform coverage
 *
 * Windows MSVC / Linux GCC / macOS Clang / Linux aarch64 GCC.
 * No network access; no external dependencies beyond `nimrtc::core`.
 */

#include <gtest/gtest.h>

#include <cstring>

#include <nimrtc/core/api_version.hpp>

#include "api_version_expectations.hpp"

// -----------------------------------------------------------------------------
// Test 1 -- version macros are defined correctly
// -----------------------------------------------------------------------------

TEST(version_macros, are_defined) {
    // The triplet and the pre-release tag are compared against the pin
    // rather than against a literal, so a bump does not touch this file.
    //
    // We deliberately use runtime EXPECT_* here (not static_assert +
    // std::strcmp) because MSVC's <cstring> does not yet declare
    // std::strcmp as constexpr -- only the non-standard ::strcmp is
    // constexpr.  Switching to a runtime check costs us zero
    // release-build perf (the test runs once) and keeps the assertion
    // portable to MSVC 19.30+, GCC 10+, Clang 12+.
    EXPECT_EQ(NIMRTC_API_VERSION_MAJOR, NIMRTC_EXPECTED_API_VERSION_MAJOR);
    EXPECT_EQ(NIMRTC_API_VERSION_MINOR, NIMRTC_EXPECTED_API_VERSION_MINOR);
    EXPECT_EQ(NIMRTC_API_VERSION_PATCH, NIMRTC_EXPECTED_API_VERSION_PATCH);

    EXPECT_STREQ(NIMRTC_API_VERSION_PRE_RELEASE,
                 NIMRTC_EXPECTED_API_VERSION_PRE_RELEASE);
}

// -----------------------------------------------------------------------------
// Test 1b -- encoded version integer is correct
// -----------------------------------------------------------------------------

TEST(version_macros, encoded_version_is_correct) {
    // NIMRTC_API_VERSION encodes MAJOR*10000 + MINOR*100 + PATCH.  The
    // expected value is the pin, which the release process sets.
    static_assert(NIMRTC_API_VERSION == NIMRTC_EXPECTED_API_VERSION_ENCODED,
                  "NIMRTC_API_VERSION must match the pinned API version");
    EXPECT_EQ(NIMRTC_API_VERSION, NIMRTC_EXPECTED_API_VERSION_ENCODED);
}

// -----------------------------------------------------------------------------
// Test 2 -- version string is well-formed
// -----------------------------------------------------------------------------

TEST(version_string, is_well_formed) {
    // NIMRTC_API_VERSION_STRING must expand to the pinned "MAJOR.MINOR.PATCH".
    // Runtime EXPECT_STREQ (not static_assert + std::strcmp) for MSVC
    // <cstring> constexpr compatibility -- see the comment in
    // version_macros above.
    EXPECT_STREQ(NIMRTC_API_VERSION_STRING, NIMRTC_EXPECTED_API_VERSION_STRING);
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

    // And the whole triple matches the pin.
    EXPECT_EQ(kMajor, NIMRTC_EXPECTED_API_VERSION_MAJOR);
    EXPECT_EQ(kMinor, NIMRTC_EXPECTED_API_VERSION_MINOR);
    EXPECT_EQ(kPatch, NIMRTC_EXPECTED_API_VERSION_PATCH);
    EXPECT_STREQ(kString, NIMRTC_EXPECTED_API_VERSION_STRING);
    EXPECT_STREQ(kPreRelease, NIMRTC_EXPECTED_API_VERSION_PRE_RELEASE);
}

// -----------------------------------------------------------------------------
// Test 4 -- stability markers expand to valid C++
//
// NIMRTC_API_STABLE and NIMRTC_API_UNSTABLE are empty.  This test
// verifies they are syntactically well-formed when applied to a
// function declaration (compile-time check; no runtime effect).
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

// -----------------------------------------------------------------------------
// Test 6 -- NIMRTC_API_VERSION_CHECK compile-time version comparison
// -----------------------------------------------------------------------------

TEST(version_check, works_correctly) {
    // NIMRTC_API_VERSION_CHECK(major, minor, patch) is true when the
    // current version is >= the given one.  The comparisons below are
    // written relative to the pinned version so that a bump does not
    // touch this file.

    // Current version exactly.
    static_assert(NIMRTC_API_VERSION_CHECK(NIMRTC_EXPECTED_API_VERSION_MAJOR,
                                           NIMRTC_EXPECTED_API_VERSION_MINOR,
                                           NIMRTC_EXPECTED_API_VERSION_PATCH),
                  "the pinned version must match itself");
    EXPECT_TRUE(NIMRTC_API_VERSION_CHECK(NIMRTC_EXPECTED_API_VERSION_MAJOR,
                                         NIMRTC_EXPECTED_API_VERSION_MINOR,
                                         NIMRTC_EXPECTED_API_VERSION_PATCH));

    // Earlier versions should also pass.
    EXPECT_TRUE(NIMRTC_API_VERSION_CHECK(0, 0, 0));
    EXPECT_TRUE(NIMRTC_API_VERSION_CHECK(NIMRTC_EXPECTED_API_VERSION_MAJOR, 0, 0));

    // Later versions should fail (static_assert triggers, EXPECT_FALSE catches).
    // We cannot use static_assert for the negative case because it would fail
    // at compile time.  Use constexpr bool instead.
    constexpr bool next_major_check =
        NIMRTC_API_VERSION_CHECK(NIMRTC_EXPECTED_API_VERSION_MAJOR + 1, 0, 0);
    constexpr bool next_minor_check =
        NIMRTC_API_VERSION_CHECK(NIMRTC_EXPECTED_API_VERSION_MAJOR,
                                 NIMRTC_EXPECTED_API_VERSION_MINOR + 1, 0);
    constexpr bool next_patch_check =
        NIMRTC_API_VERSION_CHECK(NIMRTC_EXPECTED_API_VERSION_MAJOR,
                                 NIMRTC_EXPECTED_API_VERSION_MINOR,
                                 NIMRTC_EXPECTED_API_VERSION_PATCH + 1);
    EXPECT_FALSE(next_major_check);
    EXPECT_FALSE(next_minor_check);
    EXPECT_FALSE(next_patch_check);
}
