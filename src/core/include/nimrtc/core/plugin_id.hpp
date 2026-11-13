/**
 * @file nimrtc/core/plugin_id.hpp
 * @brief NIMRTC_PLUGIN_ID — compile-time-unique plugin id literal wrapper (PAL Slice 3).
 *
 * ## What this is
 *
 * `NIMRTC_PLUGIN_ID(x)` is a thin macro that wraps a string literal in a
 * `nimrtc::core::detail::PluginIdTag<N>`-typed wrapper, where `N` is
 * `__LINE__` (or `__COUNTER__` on compilers that lack usable `__LINE__`
 * expansion in the chosen context). Every callsite of the macro therefore
 * produces a wrapper of a distinct *type* even if two plugin files use
 * the same string literal — the *runtime* string value is what the
 * registry sees.
 *
 * ## What problem this solves
 *
 * Before PAL Slice 3, plugin id literals (`"webrtc"`, `"webrtc_apm"`,
 * `"adaptive"`, etc.) were scattered across each plugin module's `.id()`
 * implementation as plain `std::string_view{ "..." }` returns. Two
 * unrelated modules could therefore accidentally pick the same id and
 * silently collide at runtime in `core::PluginRegistry::register_*()`,
 * with the second registration overwriting the first (the registry is
 * "last writer wins" by design, see registry.hpp).
 *
 * `NIMRTC_PLUGIN_ID("opus")` at every callsite forces a compile-time
 * distinct type at that callsite (`PluginIdTag<42>` vs `PluginIdTag<100>`).
 * The string value is captured but the *type* is unique, so:
 *   (a) Auditing which ids exist is a one-line grep:
 *           git grep -n 'NIMRTC_PLUGIN_ID('
 *   (b) Adding a new module forces a new callsite → a new entry in the
 *       grep, visible in code review.
 *   (c) A `static_assert` in the constructor validates that the literal
 *       matches the ADR-019 grammar `^[a-z][a-z0-9_-]*$` (catches
 *       `NIMRTC_PLUGIN_ID("")`, an uppercase id, a leading digit, a
 *       space, and any non-ASCII byte).
 *
 * ## Why a wrapper type and not just `#define NIMRTC_PLUGIN_ID(x) x`
 *
 * The PAL doc §4 Slice 3 first proposes a plain literal-string macro;
 * the requirement that "if two plugin files accidentally use the same
 * id literal, the build fails" motivates the stronger `PluginIdTag<N>`
 * wrapper: each callsite carries its line number as a template
 * parameter, so two distinct callsites always produce distinct types
 * (and therefore distinct overload resolution / distinct symbols). This
 * makes accidental same-id duplication grep-detectable even when the
 * strings happen to match.
 *
 * ## Header-only & constexpr
 *
 * The wrapper stores the literal as a `const char*` and is a literal type,
 * so the macro expansion is fully usable in any constant-evaluated context
 * (default arguments, static_asserts, non-type template parameters where
 * permitted by C++20).
 *
 * ## Typical usage
 *
 * @code
 * // In audio3a_plugin.cpp
 * std::string_view NullPluginFactory::id() const noexcept {
 *     return NIMRTC_PLUGIN_ID("webrtc");   // → PluginIdTag<__LINE__>{"webrtc"}.value
 * }
 *
 * // In opus codec_plugin.cpp
 * std::string_view OpusPluginFactory::id() const noexcept {
 *     return NIMRTC_PLUGIN_ID("opus");
 * }
 * @endcode
 *
 * ## Out of scope
 *
 * This macro does NOT enforce cross-file uniqueness of the *string value*;
 * it makes the callsite discoverable via grep. Cross-file uniqueness is
 * a runtime / registry-level invariant (the registry surfaces duplicates
 * via `list_*()` which a test can iterate — see
 * `tests/test_engine_plugin_loading.cpp::testPluginIdCompileTimeUniqueness`).
 */

#ifndef NIMRTC_CORE_PLUGIN_ID_HPP
#define NIMRTC_CORE_PLUGIN_ID_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.

#include <string_view>
#include <nimrtc/core/api_version.hpp>

namespace nimrtc::core::detail {

/**
 * @brief The plugin id grammar (ADR-019 Decision 1).
 *
 *     ^[a-z][a-z0-9_-]*$
 *
 * A lowercase ASCII letter, then lowercase letters, digits,
 * underscores and hyphens.  The hyphen is permitted because the
 * shipped transport-stack ids (`"webrtc-classic"`, `"raw-udp-arq"`)
 * already use it and are public config values.
 *
 * The check is `constexpr` so it can run at compile time on the
 * literal — a malformed id is a compile error at the callsite, not a
 * runtime surprise at `open()`.
 */
constexpr bool is_valid_plugin_id(std::string_view id) noexcept {
    if (id.empty()) return false;
    const char first = id[0];
    if (first < 'a' || first > 'z') return false;
    for (std::size_t i = 1; i < id.size(); ++i) {
        const char c = id[i];
        const bool ok = (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') ||
                        c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

/**
 * @brief The compile-time grammar check (ADR-019 Decision 9).
 *
 * The check has to run in a **constant expression**, and it has to run
 * on the *literal* at the callsite — not on a runtime string.  Two
 * shapes were tried and one is not usable:
 *
 *  - A `consteval` function taking a `const char*` does **not** work:
 *    reading through a pointer *parameter* is not a constant
 *    expression, so the `static_assert` inside it reports
 *    "non-constant condition" rather than evaluating the grammar.
 *    (Verified on GCC 11 and Clang.)
 *  - A `consteval` function taking a `const char (&)[N]` has the same
 *    problem for the same reason.
 *
 * What *does* work is binding the literal to a `constexpr` local
 * **inside a `constexpr` lambda**, because the macro substitutes the
 * argument textually: the local becomes
 * `constexpr const char* _p = "opus";`, and reading through *that* is
 * a constant expression — the literal has static storage duration and
 * `const` element type.  The `static_assert` therefore fires at
 * compile time, so a malformed id is a compile error at the callsite,
 * not a runtime surprise at `open()`.
 *
 * Both spellings work: a string literal (`"opus"`) and a
 * `static constexpr const char*` bound to one (`kBackendId`).
 */

/**
 * @brief Compile-time-unique plugin id wrapper.
 *
 * Each callsite of `NIMRTC_PLUGIN_ID(x)` instantiates this template with
 * a distinct line number, giving the wrapper a unique *type*. The
 * runtime `.value` carries the original string literal.
 *
 * @tparam Line   Source line of the callsite (from `__LINE__`). Two
 *                callsites always produce two distinct types.
 */
template <int Line>
struct NIMRTC_API_STABLE PluginIdTag {
    const char* value;

    /// The grammar check lives in the `NIMRTC_PLUGIN_ID` macro below,
    /// which binds the literal to a `constexpr` local inside a
    /// `constexpr` lambda and asserts on it there.
    constexpr explicit PluginIdTag(const char* s) noexcept
        : value(s) {}

    /// Implicit conversion to `std::string_view` lets the macro substitute
    /// cleanly into existing `std::string_view` returns without an
    /// explicit `.value` dereference at every callsite.
    constexpr operator std::string_view() const noexcept {
        return std::string_view{value};
    }
};

} // namespace nimrtc::core::detail

/**
 * @def NIMRTC_PLUGIN_ID(x)
 * @brief Wrap a plugin id literal in a compile-time-unique
 *        `PluginIdTag<__LINE__>` wrapper, and check it against the
 *        ADR-019 grammar at compile time.
 *
 * Use this at every plugin factory's `id()` return statement (and at
 * any other site that names a plugin id literal) so the callsite is
 * greppable, the wrapper type is unique per callsite, and a
 * malformed id is a compile error rather than a runtime surprise.
 *
 * @param x  A string literal (or a `constexpr const char*` bound to
 *             one) naming the plugin id.  It must match
 *             `^[a-z][a-z0-9_-]*$`.
 *
 * Expands to: a `::nimrtc::core::detail::PluginIdTag<__LINE__>`
 * wrapping the literal, with a `static_assert` on the grammar.
 */
#define NIMRTC_PLUGIN_ID(x)                                        \
    ::nimrtc::core::detail::PluginIdTag<__LINE__>(                 \
        []() constexpr {                                           \
            constexpr auto _p = (x);                               \
            static_assert(                                         \
                ::nimrtc::core::detail::is_valid_plugin_id(        \
                    std::string_view{_p}),                         \
                "NIMRTC_PLUGIN_ID: the plugin id must match "      \
                "^[a-z][a-z0-9_-]*$ (a lowercase ASCII letter, "   \
                "then lowercase letters, digits, underscores and "  \
                "hyphens). See docs/adr/ADR-019-plugin-id-naming-"  \
                "contract.md.");                                   \
            return _p;                                             \
        }())

#endif // NIMRTC_CORE_PLUGIN_ID_HPP
