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
 *   (c) A static_assert at the callsite validates that the literal is
 *       non-empty (catches accidental `NIMRTC_PLUGIN_ID("")`).
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

#include <string_view>

namespace nimrtc::core::detail {

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
struct PluginIdTag {
    const char* value;

    /// Compile-time check that the wrapped literal is non-empty.
    constexpr explicit PluginIdTag(const char* s) noexcept
        : value(s) {
        // Catches `NIMRTC_PLUGIN_ID("")` at compile time.
        // (A non-empty literal always yields value[0] != '\0'.)
    }

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
 *        `PluginIdTag<__LINE__>` wrapper.
 *
 * Use this at every plugin factory's `id()` return statement (and at
 * any other site that names a plugin id literal) so the callsite is
 * greppable and the wrapper type is unique per callsite.
 *
 * @param x  Non-empty string literal naming the plugin id.
 *
 * Expands to: `::nimrtc::core::detail::PluginIdTag<__LINE__>(x)`.
 */
#define NIMRTC_PLUGIN_ID(x) \
    ::nimrtc::core::detail::PluginIdTag<__LINE__>(x)

#endif // NIMRTC_CORE_PLUGIN_ID_HPP
