/**
 * @file nimrtc/engine/pal/make_plugin.hpp
 * @brief PAL Slice 1.5 — `pal::make_plugin` template consolidates the
 *        5-step "factory lookup → create → null-check → open → report"
 *        dance repeated for every plugin category in NimRTCEngine.
 *
 * Companion to `engine_plugin_resolver.hpp` (ADR-009, Slice 1). Slice 1
 * collapsed `reg.get_<category>(id)` into `pal::resolve_<category>(id)`;
 * Slice 1.5 collapses the surrounding 4-step boilerplate (create / null
 * check / open / report) into a single template call. The result is
 * a per-plugin-block drop from ~20 lines to ~7 lines, with no loss of
 * behaviour.
 *
 * ## Why a template (not a virtual `IPluginFactory`)
 *
 * Each factory type has a different `create(ConfigT)` signature — there
 * is no shared base that returns `IPlugin*` with a typed Config
 * parameter. A non-template wrapper would force every plugin to expose
 * a uniform Config shape (significant churn across 9+ plugin
 * categories). A template specialisation per category is what the
 * 5-step dance is effectively doing inline; the template just makes
 * the specialisation declarative.
 *
 * ## Failure modes
 *
 * Each of the two failure paths (factory missing, open failed) is
 * independently configurable via `PluginFailureMode`:
 *
 *   - `Error`  → fires the engine's `on_error_` callback with the
 *                supplied code (e.g. `kVideoSinkOpenFailed`).
 *   - `Warn`   → logs a warning via `core::log::Logger`.
 *   - `Silent` → does nothing (the caller handles the consequence,
 *                typically by falling back to a concrete implementation).
 *
 * The 9 plugin categories each map to a different combination — see
 * `engine.cpp` for the per-call-site mapping. The helper does not
 * hard-code any of those policies; the caller passes them in.
 *
 * @see docs/adr/ADR-009-pal-slice-1.md
 */
#ifndef NIMRTC_ENGINE_PAL_MAKE_PLUGIN_HPP
#define NIMRTC_ENGINE_PAL_MAKE_PLUGIN_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <nimrtc/core/api_version.hpp>
#include <nimrtc/core/log.hpp>
#include <nimrtc/plugins/base.hpp>

namespace nimrtc::engine {

// Forward declaration — defined in <nimrtc/engine/engine.hpp>. We use a
// forward declaration here so the helper does not drag in the full
// engine.hpp (which pulls concrete module headers transitively). The
// forward declaration matches the engine's public callback shape.
using EngineErrorCallback = std::function<void(std::uint32_t err, std::string_view msg)>;

namespace pal {

/** How `make_plugin` should report a failure. */
enum class NIMRTC_API_STABLE PluginFailureMode : std::uint8_t {
    Error,   ///< Fire the engine's on_error_ callback with the supplied code.
    Warn,    ///< Log a warning via core::log::Logger.
    Silent,  ///< Do nothing; the caller is expected to handle the consequence.
};

/**
 * @brief Create and open a plugin from a (already-resolved) factory pointer.
 *
 * The 5-step dance:
 *   1. factory == nullptr  →  report via `missing_mode`  →  return false.
 *   2. slot.reset( factory->create( std::forward<Config>(config) ) ).
 *   3. slot == nullptr     →  report via `missing_mode`  →  return false.
 *   4. post_create(*slot)  (only if the post_create hook is non-null).
 *   5. slot->open() != kOk →  report via `open_fail_mode` + slot.reset()
 *                            →  return false.
 *
 * On success returns `true` and leaves the plugin in `slot` (open()ed).
 * On any failure returns `false` and resets `slot` to `nullptr`.
 *
 * @tparam IPlugin     The plugin interface. Must derive from
 *                     `plugins::IPlugin` (for the `open()` call).
 * @tparam Config      The factory's config type. Forwarded to
 *                     `factory->create()` by perfect forwarding.
 * @tparam Factory     The factory type. Must have a
 *                     `IPlugin* create(ConfigT)` member.
 * @tparam PostCreate  Optional hook type, default `std::nullptr_t`.
 *                     If non-null, the callable is invoked as
 *                     `post_create(*slot)` between create() and open().
 *                     Used by audio3a (set_callbacks must precede open).
 *
 * @param factory           Resolved factory pointer (typically the
 *                          return of `pal::resolve_*()` or
 *                          `reg.get_*()`).
 * @param plugin_id         Plugin id (for diagnostic messages).
 * @param slot              Engine-owned `std::unique_ptr<IPlugin>`.
 *                          Set on success; cleared on failure.
 * @param config            Config forwarded to `factory->create()`.
 * @param err_open_failed   Code to pass to `on_error` if open() fails
 *                          (only used when `open_fail_mode == Error`).
 * @param err_missing       Code to pass to `on_error` if factory is
 *                          null or `create` returned null (only used
 *                          when `missing_mode == Error`).
 * @param open_fail_mode    How to report an open() failure.
 * @param missing_mode      How to report a factory-missing failure.
 * @param on_error          Engine error callback. May be empty; only
 *                          consulted when a failure mode == Error.
 * @param post_create       Optional callable invoked between create()
 *                          and open(). Default = no-op.
 *
 * @return true iff a plugin was created AND opened successfully.
 */
template <typename IPlugin, typename Config, typename Factory,
          typename PostCreate = std::nullptr_t>
[[nodiscard]] NIMRTC_API_STABLE bool make_plugin(
    const Factory* factory,
    std::string_view plugin_id,
    std::unique_ptr<IPlugin>& slot,
    Config&& config,
    std::uint32_t err_open_failed,
    std::uint32_t err_missing,
    PluginFailureMode open_fail_mode,
    PluginFailureMode missing_mode,
    EngineErrorCallback on_error = nullptr,
    PostCreate&& post_create = nullptr) noexcept {

    // Helper: emit a "missing" diagnostic per the chosen mode.
    auto report_missing = [&]() noexcept {
        const std::string msg = std::string{"engine: '"}
            + std::string{plugin_id}
            + "' plugin factory not registered or returned null";
        switch (missing_mode) {
            case PluginFailureMode::Error:
                if (on_error) on_error(err_missing, msg);
                break;
            case PluginFailureMode::Warn:
                nimrtc::core::log::Logger::instance().warn(msg);
                break;
            case PluginFailureMode::Silent:
                break;
        }
    };

    auto report_open_failed = [&]() noexcept {
        const std::string msg = std::string{"engine: '"}
            + std::string{plugin_id}
            + "' plugin open failed";
        switch (open_fail_mode) {
            case PluginFailureMode::Error:
                if (on_error) on_error(err_open_failed, msg);
                break;
            case PluginFailureMode::Warn:
                nimrtc::core::log::Logger::instance().warn(msg);
                break;
            case PluginFailureMode::Silent:
                break;
        }
    };

    // Step 1: factory missing?
    if (!factory) {
        report_missing();
        slot.reset();
        return false;
    }

    // Step 2: create.  Branch at compile time so plugins whose factory
    // takes no config (e.g. `IAudio3AFactory::create()`) can pass
    // `nullptr` as the config argument and still compile.
    if constexpr (std::is_same_v<std::remove_cv_t<std::remove_reference_t<Config>>,
                                 std::nullptr_t>) {
        slot.reset(factory->create());
    } else {
        slot.reset(factory->create(std::forward<Config>(config)));
    }

    // Step 3: create returned null?
    if (!slot) {
        report_missing();
        return false;
    }

    // Step 4: post-create hook (e.g. set_callbacks for audio3a).
    // Note: we call the hook unconditionally inside the constexpr branch
    // — the `if (post_create)` truthiness check is intentionally
    // absent because captured lambdas (the audio3a use case:
    // `[this](plugins::IAudio3A& p) { p.set_callbacks(...); }`) do not
    // have an implicit `operator bool()` or a function-pointer
    // conversion (captures disable the latter), so `if (lambda)` is
    // ill-formed.  The `if constexpr` branch is only entered when
    // `PostCreate != std::nullptr_t`, which means the caller was
    // required to supply a non-null callable argument; the default
    // parameter value `nullptr` only applies when
    // `PostCreate = std::nullptr_t` (and that case is discarded).
    if constexpr (!std::is_same_v<PostCreate, std::nullptr_t>) {
        std::forward<PostCreate>(post_create)(*slot);
    }

    // Step 5: open.
    if (slot->open() != nimrtc::plugins::kOk) {
        report_open_failed();
        slot.reset();
        return false;
    }

    return true;
}

}  // namespace pal
}  // namespace nimrtc::engine

#endif  // NIMRTC_ENGINE_PAL_MAKE_PLUGIN_HPP
