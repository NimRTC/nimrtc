// Note: This file intentionally saved as UTF-8 with BOM so that MSVC
// (compiling with a non-UTF-8 code page such as CP936/GBK on Chinese Windows)
// does not emit C4819 for any future non-ASCII characters in comments.

/**
 * @file tests/test_api_classification.cpp
 * @brief v1.0.0-alpha.1 Sec.12.3.1 DoD gate #6 (extension) -- public header
 *        classification coverage check.
 *
 * ## What this test verifies
 *
 * Per ADR-015 (`docs/adr/ADR-015-api-stability-policy.md`) and
 * `docs/plan/v1.0-plan.md` Sec.12.3.1, every public symbol under
 * `src/**​/include/nimrtc/**​/*.hpp` must carry a stability
 * classification.  In v1.0.0-alpha.1 that means every public
 * header file must contain at least one `NIMRTC_API_STABLE` or
 * `NIMRTC_API_UNSTABLE` marker.
 *
 * This test walks the source tree at runtime and asserts:
 *
 * 1. `every_header_has_at_least_one_marker` -- for every `.hpp`
 *    under `<root>/src/**​/include/nimrtc/**` (excluding
 *    `<root>/src/third_party/**`), the file content contains at
 *    least one occurrence of `NIMRTC_API_STABLE` or
 *    `NIMRTC_API_UNSTABLE`.  Failures print the full missing list
 *    so the next PR can apply the missing markers mechanically.
 *
 * 2. `total_header_count_matches_baseline` -- the count of public
 *    headers equals the v1.0.0-alpha.1 baseline of 88.  A drift
 *    here signals that new public headers were added without
 *    updating the classification sweep; bump the baseline in the
 *    same PR that adds the header.
 *
 * ## Why runtime (not compile-time) check
 *
 * `NIMRTC_API_STABLE` and `NIMRTC_API_UNSTABLE` are empty macros
 * in v1.0.0-alpha.1 (visual markers only).  A compile-time check
 * cannot detect "header X has no markers" because the preprocessor
 * erases them.  A runtime filesystem walk is the authoritative
 * way to count marker occurrences per file.
 *
 * ## Configuration
 *
 * `NIMRTC_SOURCE_ROOT` is injected by CMake
 * (`target_compile_definitions(... NIMRTC_SOURCE_ROOT=${CMAKE_SOURCE_DIR})`).
 * The test must not hardcode any path; it only uses the compile-time
 * injection so the same binary runs unmodified from any build
 * directory.
 *
 * ## Platform coverage
 *
 * Windows MSVC / Linux GCC / macOS Clang.  Uses C++17 `<filesystem>`.
 * No network access.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifndef NIMRTC_SOURCE_ROOT
    #error "NIMRTC_SOURCE_ROOT must be defined via CMake target_compile_definitions"
#endif

namespace fs = std::filesystem;

namespace {

// -----------------------------------------------------------------------------
// Filesystem walker
// -----------------------------------------------------------------------------

/// Recursively collect every `.hpp` under `<root>/src/**​/include/nimrtc/**`
/// that is NOT under `<root>/src/third_party/**`.
///
/// Returns paths sorted lexicographically (stable ordering for ctest output).
std::vector<fs::path> collect_public_headers(const fs::path& root) {
    std::vector<fs::path> result;
    fs::path src = root / "src";
    if (!fs::exists(src) || !fs::is_directory(src)) {
        return result;
    }
    for (auto entry = fs::recursive_directory_iterator(
                 src, fs::directory_options::skip_permission_denied);
         entry != fs::recursive_directory_iterator();
         ++entry) {
        if (!entry->is_regular_file()) {
            continue;
        }
        const fs::path& p = entry->path();
        // Normalize path separators to '/' for substring matching so the
        // check works identically on Windows (backslash) and POSIX (slash).
        std::string path_str = p.generic_string();
        // Must live under .../include/nimrtc/...
        if (path_str.find("/include/nimrtc/") == std::string::npos) {
            continue;
        }
        // Must not live under src/third_party/.
        if (path_str.find("/third_party/") != std::string::npos) {
            continue;
        }
        if (p.extension() != ".hpp") {
            continue;
        }
        result.push_back(p);
    }
    std::sort(result.begin(), result.end());
    return result;
}

// -----------------------------------------------------------------------------
// Marker counter (per file)
// -----------------------------------------------------------------------------

struct MarkerCounts {
    int stable     = 0;
    int unstable   = 0;
    int deprecated = 0;
};

/// Count occurrences of NIMRTC_API_STABLE / UNSTABLE / DEPRECATED in `file`.
///
/// `std::regex` is used (not <regex> std::search) so the match is anchored
/// at a word boundary and avoids false positives in unrelated identifiers
/// (e.g. `NIMRTC_API_STABLE_FOO`).
MarkerCounts count_markers(const fs::path& file) {
    std::ifstream f(file);
    if (!f) {
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string content = ss.str();

    auto count = [&content](const char* pat) -> int {
        // The sregex_iterator constructor takes a const std::basic_regex&,
        // not an rvalue, so we must store the regex in a named local.
        // Passing a temporary `std::regex(pat)` directly triggers C2280
        // ("attempting to reference a deleted function") on MSVC.
        const std::regex re(pat);
        const auto begin = std::sregex_iterator(content.begin(), content.end(), re);
        const auto end = std::sregex_iterator();
        return static_cast<int>(std::distance(begin, end));
    };

    MarkerCounts c;
    c.stable     = count(R"(NIMRTC_API_STABLE\b)");
    c.unstable   = count(R"(NIMRTC_API_UNSTABLE\b)");
    c.deprecated = count(R"(NIMRTC_API_DEPRECATED\b)");
    return c;
}

} // namespace

// -----------------------------------------------------------------------------
// Test 1 -- every public header has >=1 stability classification marker
// -----------------------------------------------------------------------------

TEST(public_header_classification, every_header_has_at_least_one_marker) {
    const fs::path root = fs::path(NIMRTC_SOURCE_ROOT);
    const auto headers = collect_public_headers(root);

    ASSERT_FALSE(headers.empty())
        << "No public headers found under " << root
        << "/src/**​/include/nimrtc/**.  Check NIMRTC_SOURCE_ROOT and the "
        << "include-directory layout in the project.";

    std::vector<fs::path> missing;
    int total_stable = 0;
    int total_unstable = 0;
    int total_deprecated = 0;

    for (const auto& h : headers) {
        const MarkerCounts c = count_markers(h);
        total_stable     += c.stable;
        total_unstable   += c.unstable;
        total_deprecated += c.deprecated;
        if (c.stable == 0 && c.unstable == 0) {
            missing.push_back(h);
        }
    }

    // Print a human-readable summary to stdout so ctest -V shows it on
    // both pass and fail runs.  Engineers triaging the sweep can read the
    // exact list without rerunning with -V -V.
    std::cout << "[classification] source_root=" << root.generic_string() << '\n'
              << "[classification] headers=" << headers.size()
              << " stable=" << total_stable
              << " unstable=" << total_unstable
              << " deprecated=" << total_deprecated
              << " missing_markers=" << missing.size() << '\n';

    if (!missing.empty()) {
        std::cout << "[classification] missing files (" << missing.size()
                  << "):\n";
        for (const auto& m : missing) {
            // Strip the source-root prefix for readability.
            std::string rel = m.generic_string();
            const std::string root_prefix =
                std::string(root.generic_string()) + "/";
            if (rel.rfind(root_prefix, 0) == 0) {
                rel = rel.substr(root_prefix.size());
            }
            std::cout << "  " << rel << '\n';
        }
    }

    EXPECT_TRUE(missing.empty())
        << missing.size() << " of " << headers.size()
        << " public headers have no NIMRTC_API_STABLE/UNSTABLE marker. "
        << "See the [classification] section above for the full list. "
        << "Per ADR-015, every public symbol must carry a stability "
        << "classification; apply NIMRTC_API_STABLE to all public "
        << "declarations in the missing files.";
}

// -----------------------------------------------------------------------------
// Test 2 -- total header count matches the v1.0.0-alpha.1 baseline
// -----------------------------------------------------------------------------

TEST(public_header_classification, total_header_count_matches_baseline) {
    const fs::path root = fs::path(NIMRTC_SOURCE_ROOT);
    const auto headers = collect_public_headers(root);

    // v1.0.0-alpha.1 baseline: 89 public headers (verified 2026-10-14 by
    // build/_count_headers.ps1 + tests/test_api_classification.cpp run,
    // see docs/plan/v1.0-plan.md Sec.12.3.1).
    //   - 4 headers have >=1 NIMRTC_API_STABLE marker applied (the
    //     framework file api_version.hpp plus three pilot files:
    //     engine.hpp, engine/pal/make_plugin.hpp, transport_plugin.hpp).
    //   - 85 headers are still missing markers — the bulk sweep is
    //     tracked as a post-alpha.1 follow-up; this test fires
    //     `every_header_has_at_least_one_marker` (above) to surface
    //     the gap every time the test suite runs.
    // Bump this constant in the same PR that intentionally adds/removes
    // a public header.
    constexpr std::size_t kBaselineHeaderCount = 89;

    EXPECT_EQ(headers.size(), kBaselineHeaderCount)
        << "Public header count drifted from the v1.0.0-alpha.1 baseline "
        << "of " << kBaselineHeaderCount << ".  Either new public headers "
        << "were added (update the baseline + run the classification sweep) "
        << "or a public header was removed (update the baseline + ADR-015).";
}
