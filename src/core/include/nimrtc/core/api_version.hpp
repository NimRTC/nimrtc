// Note: This file intentionally saved as UTF-8 with BOM so that MSVC
// (compiling with a non-UTF-8 code page such as CP936/GBK on Chinese Windows)
// does not emit C4819 for any future non-ASCII characters in comments.

#ifndef NIMRTC_CORE_API_VERSION_HPP
#define NIMRTC_CORE_API_VERSION_HPP

/**
 * @file nimrtc/core/api_version.hpp
 * @brief NIMRTC_API_VERSION -- version constants + stability classification
 * markers (v1.0.0-alpha.1, PAL API-FREEZE framework).
 *
 * ## What this is
 *
 * `api_version.hpp` exposes two classes of symbols:
 *
 * 1. **Version constants** -- `NIMRTC_API_VERSION_MAJOR/MINOR/PATCH/PRE_RELEASE`
 *    and `NIMRTC_API_VERSION_STRING`.  These are compile-time literals derived
 *    from a single canonical version triplet defined at the top of the file.
 *    The matching `nimrtc::core::api_version::kMajor/kMinor/kPatch/kString/
 *    kPreRelease` namespace constants provide the same values for runtime
 *    inspection.
 *
 * 2. **Stability classification markers** -- `NIMRTC_API_STABLE`,
 *    `NIMRTC_API_UNSTABLE`, and `NIMRTC_API_DEPRECATED(msg)`.  These are
 *    applied to every public symbol in `src/<mod>/include/nimrtc/<mod>/<name>.hpp` as
 *    part of the v1.0.0-alpha.1 public-header classification sweep
 *    (PR #3).  The sweep is mechanical: it tags symbols without changing
 *    compilation semantics.
 *
 * ## v1.0.0-alpha.1 design decision: STABLE / UNSTABLE are empty
 *
 * `NIMRTC_API_STABLE` and `NIMRTC_API_UNSTABLE` expand to nothing
 * (empty macro) in this release.  They are **visual / documentation
 * markers only** -- they carry no compile-time annotation so existing
 * v0.x callers that have not been updated to recognise the new markers
 * continue to compile without modification.
 *
 * The framework is designed to support a future opt-in mode where
 * `NIMRTC_ENABLE_API_STABILITY_ANNOTATIONS=1` causes
 * `NIMRTC_API_UNSTABLE` to emit a `[[deprecated]]` attribute and
 * trigger `-Wdeprecated-declarations` (GCC/Clang) / `C4996` (MSVC) at
 * every callsite of an UNSTABLE symbol.  This is deferred to a
 * post-alpha.1 follow-up; the pre-release split document
 * (`docs/plan/v1.0-plan.md` Sec.12) captures the design rationale.
 *
 * `NIMRTC_API_DEPRECATED(msg)` is **always active**: it expands to the
 * cross-compiler `[[deprecated(msg)]]` attribute regardless of any opt-in
 * flag, and emits the standard compiler deprecation warning on every
 * caller site.  Callers that use a deprecated symbol must be updated
 * before the next MINOR bump.
 *
 * ## Public API freeze commitment
 *
 * Every symbol under `src/<mod>/include/nimrtc/<mod>/<name>.hpp` carries a
 * stability classification.  The policy is documented in
 * `docs/adr/ADR-015-api-stability-policy.md`.
 *
 * ## Sec.12.3.1 v1.0.0-alpha.1 DoD gate #6
 *
 * `tests/test_api_version.cpp` 5/5 PASS verifies the macro + namespace
 * constants are well-formed and that the STABLE/UNSTABLE/DEPRECATED
 * expansion is valid C++.
 *
 * @see docs/plan/v1.0-plan.md Sec.2.1 (API-VERSION P0 item) and Sec.12
 *      (pre-release split).
 * @see docs/adr/ADR-015-api-stability-policy.md (deprecation policy).
 */

// -----------------------------------------------------------------------------
// Version constants
// -----------------------------------------------------------------------------

/// Major version number of the NimRTC public API.
#define NIMRTC_API_VERSION_MAJOR 1

/// Minor version number of the NimRTC public API.
#define NIMRTC_API_VERSION_MINOR 0

/// Patch version number of the NimRTC public API.
#define NIMRTC_API_VERSION_PATCH 0

/// Pre-release identifier for v1.0.0-alpha.1.  Empty string for final releases.
#define NIMRTC_API_VERSION_PRE_RELEASE "alpha.1"

// Two-level indirection required to expand __LINE__ if the caller passes
// a macro rather than a literal string (C99/C++03 Sec.6.10.3).
#define NIMRTC_API_VERSION_STRING_IMPL_(major, minor, patch) \
    #major "." #minor "." #patch
#define NIMRTC_API_VERSION_STRING_IMPL(major, minor, patch) \
    NIMRTC_API_VERSION_STRING_IMPL_(major, minor, patch)

/// Human-readable version string "MAJOR.MINOR.PATCH" (e.g. "1.0.0").
#define NIMRTC_API_VERSION_STRING \
    NIMRTC_API_VERSION_STRING_IMPL( \
        NIMRTC_API_VERSION_MAJOR, \
        NIMRTC_API_VERSION_MINOR, \
        NIMRTC_API_VERSION_PATCH)

// -----------------------------------------------------------------------------
// Stability classification markers
//
// v1.0.0-alpha.1 decision: STABLE and UNSTABLE are empty (visual only).
// NIMRTC_API_DEPRECATED(msg) is always active (cross-compiler attribute).
// Future: NIMRTC_ENABLE_API_STABILITY_ANNOTATIONS=1 will cause
// NIMRTC_API_UNSTABLE to emit [[gnu::deprecated]] warnings.
// -----------------------------------------------------------------------------

/**
 * @def NIMRTC_API_STABLE
 * @brief Marks a public symbol as part of the stable ABI.
 *
 * Applied to every symbol that is guaranteed not to change in a
 * patch release and whose removal from the public surface requires
 * a MINOR or MAJOR version bump per the stability policy.
 *
 * v1.0.0-alpha.1: empty expansion (visual / documentation only).
 * Compile-time annotation is deferred to a post-alpha.1 follow-up
 * that activates when NIMRTC_ENABLE_API_STABILITY_ANNOTATIONS=1.
 */
#define NIMRTC_API_STABLE

/**
 * @def NIMRTC_API_UNSTABLE
 * @brief Marks a public symbol as experimental and subject to change.
 *
 * Applied to symbols that are exposed for early access or evaluation
 * but are not yet committed to the stable ABI.  Such symbols may
 * change or be removed in any release without a version bump
 * announcement.
 *
 * v1.0.0-alpha.1: empty expansion (visual / documentation only).
 * Compile-time annotation is deferred to a post-alpha.1 follow-up
 * that activates when NIMRTC_ENABLE_API_STABILITY_ANNOTATIONS=1.
 */
#define NIMRTC_API_UNSTABLE

/**
 * @def NIMRTC_API_DEPRECATED(msg)
 * @brief Marks a public symbol as deprecated.
 *
 * Emits the standard `[[deprecated(msg)]]` attribute on all
 * compilers (C++14 `[[deprecated]]`, GCC/Clang
 * `__attribute__((deprecated))`, MSVC `__declspec(deprecated)`).
 *
 * The compiler emits a deprecation warning on every caller site.
 * Callers must migrate to the replacement before the next
 * MINOR version bump.  Removal is governed by the stability
 * policy (ADR-015).
 *
 * @param msg  Human-readable migration hint (string literal).
 */
#if defined(__cplusplus) && (__cplusplus >= 201402L)
    // C++14 and later: standard attribute syntax.
    #define NIMRTC_API_DEPRECATED(msg) [[deprecated(msg)]]
#elif defined(__GNUC__) || defined(__clang__)
    // GCC / Clang.
    #define NIMRTC_API_DEPRECATED(msg) __attribute__((deprecated(msg)))
#elif defined(_MSC_VER)
    // MSVC.
    #define NIMRTC_API_DEPRECATED(msg) __declspec(deprecated(msg))
#else
    // Fallback: no-op.  All modern compilers are covered above.
    #define NIMRTC_API_DEPRECATED(msg)
#endif

// -----------------------------------------------------------------------------
// Runtime-accessible version constants (mirrors the macros above)
// -----------------------------------------------------------------------------

namespace nimrtc::core::api_version {

/// Major version number.
inline constexpr int kMajor = NIMRTC_API_VERSION_MAJOR;

/// Minor version number.
inline constexpr int kMinor = NIMRTC_API_VERSION_MINOR;

/// Patch version number.
inline constexpr int kPatch = NIMRTC_API_VERSION_PATCH;

/// Human-readable version string "MAJOR.MINOR.PATCH".
inline constexpr const char* kString = NIMRTC_API_VERSION_STRING;

/// Pre-release identifier (e.g. "alpha.1").  Empty string for final releases.
inline constexpr const char* kPreRelease = NIMRTC_API_VERSION_PRE_RELEASE;

} // namespace nimrtc::core::api_version

#endif // NIMRTC_CORE_API_VERSION_HPP
