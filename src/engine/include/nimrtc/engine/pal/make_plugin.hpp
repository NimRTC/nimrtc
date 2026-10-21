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
 * a per-plugin-block drop from ~9-10 lines to ~5 lines, with no loss of
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
 * ## Why two overloads (low-level + resolver-aware)
 *
 * The low-level overload (`make_plugin<IPlugin, Config, Factory>(factory, ...)`)
 * takes the factory pointer as an argument. The resolver-aware overload
 * (`make_plugin<auto Resolver, IPlugin, Config>(id, ...)`) takes a
 * resolver function pointer (e.g. `&pal::resolve_bwe`) and deduces the
 * factory type from `std::invoke_result_t<Resolver, std::string_view>`.
 * The resolver-aware form is the common case at the call site (every
 * engine.cpp call site resolves through `pal::resolve_*` or
 * `reg.get_*`); the low-level form remains for cases where the factory
 * is already in hand (e.g. a test that constructs one in-line).
 *
 * ## Failure modes
 *
 * Each of the two failure paths (factory missing, open failed) is
 * independently configurable via `PluginFailureMode` (passed in the
 * `MakePluginOpts` struct):
 *
 *   - `Error`  → fires the engine's `on_error` callback with the
 *                supplied code (e.g. `kVideoSinkOpenFailed`).
 *   - `Warn`   → logs a warning via `core::log::Logger`.
 *   - `Silent` → does nothing (the caller handles the consequence,
 *                typically by falling back to a concrete implementation).
 *
 * The 9 plugin categories each map to a different combination — see
 * `engine.cpp` for the per-call-site mapping. The helper does not
 * hard-code any of those policies; the caller passes them in via
 * `MakePluginOpts`.
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
using NIMRTC_API_STABLE EngineErrorCallback = std::function<void(std::uint32_t err, std::string_view msg)>;

namespace pal {

/** How `make_plugin` should report a failure. */
enum class NIMRTC_API_STABLE PluginFailureMode : std::uint8_t {
    Error,   ///< Fire the engine's on_error_ callback with the supplied code.
    Warn,    ///< Log a warning via core::log::Logger.
    Silent,  ///< Do nothing; the caller is expected to handle the consequence.
};

/**
 * @brief Named-arguments struct for `make_plugin`.
 *
 * Replaces the four positional `err_open_failed` / `err_missing` /
 * `open_fail_mode` / `missing_mode` / `on_error` arguments that the
 * pre-Slice-1.5 helper took. Designated initializers at the call
 * site make the per-category policy visible at a glance:
 *
 * @code
 *   pal::make_plugin<&pal::resolve_bwe>(
 *       config_.bwe_name, bwe_, config_.bwe_config,
 *       {.err_open_failed = core::kEngineInternal,
 *        .err_missing     = core::kEngineInternal,
 *        .open_fail_mode  = pal::PluginFailureMode::Warn,
 *        .missing_mode    = pal::PluginFailureMode::Warn,
 *        .on_error        = on_error_});
 * @endcode
 *
 * `err_open_failed` and `err_missing` are only consulted when
 * `open_fail_mode == Error` and `missing_mode == Error` respectively.
 * They are dead when the corresponding mode is Warn or Silent; the
 * caller is free to leave them at the default `0` in those cases.
 */
struct NIMRTC_API_STABLE MakePluginOpts {
    /** Status code passed to `on_error` when `slot->open()` fails.
     *  Only consulted when `open_fail_mode == Error`. */
    std::uint32_t err_open_failed = 0;

    /** Status code passed to `on_error` when the factory is missing or
     *  `create()` returned null. Only consulted when
     *  `missing_mode == Error`. */
    std::uint32_t err_missing = 0;

    /** How to report an `open()` failure. */
    PluginFailureMode open_fail_mode = PluginFailureMode::Error;

    /** How to report a factory-missing or create-returned-null failure. */
    PluginFailureMode missing_mode = PluginFailureMode::Error;

    /** Engine error callback. May be empty; only consulted when a
     *  failure mode == Error. */
    EngineErrorCallback on_error = nullptr;
};

/**
 * @brief Create and open a plugin from a (already-resolved) factory pointer.
 *
 * The 5-step dance:
 *   1. factory == nullptr  →  report via `opts.missing_mode`  →  return false.
 *   2. slot.reset( factory->create( std::forward<Config>(config) ) ).
 *   3. slot == nullptr     →  report via `opts.missing_mode`  →  return false.
 *   4. post_create(*slot)  (only if the post_create hook is non-null).
 *   5. slot->open() != kOk →  report via `opts.open_fail_mode` + slot.reset()
 *                            →  return false.
 *
 * On success returns `true` and leaves the plugin in `slot` (open()ed).
 * On any failure returns `false` and resets `slot` to `nullptr`.
 *
 * @tparam IPlugin     The plugin interface. Must derive from
 *                     `plugins::IPlugin` (for the `open()` call).
 * @tparam Config      The factory's config type. Forwarded to
 *                     `factory->create()` by perfect forwarding. Pass
 *                     `nullptr` (decuced as `std::nullptr_t`) for
 *                     factories that take no config
 *                     (e.g. `IAudio3AFactory::create()`).
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
 * @param opts              Named-arguments struct: error codes,
 *                          failure modes, on_error callback.
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
    const MakePluginOpts& opts,
    PostCreate&& post_create = nullptr) noexcept {

    // Helper: emit a "missing" diagnostic per the chosen mode.
    auto report_missing = [&]() noexcept {
        const std::string msg = std::string{"engine: '"}
            + std::string{plugin_id}
            + "' plugin factory not registered or returned null";
        switch (opts.missing_mode) {
            case PluginFailureMode::Error:
                if (opts.on_error) opts.on_error(opts.err_missing, msg);
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
        switch (opts.open_fail_mode) {
            case PluginFailureMode::Error:
                if (opts.on_error) opts.on_error(opts.err_open_failed, msg);
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

/**
 * @brief Resolver-aware overload of `make_plugin`.
 *
 * Takes a resolver function pointer (e.g. `&pal::resolve_bwe`) and
 * deduces the factory type from its return. The factory lookup happens
 * inside the helper, removing the `reg.get_X(id)` / `pal::resolve_X(id)`
 * boilerplate from the call site.
 *
 * @tparam Resolver  Function pointer of shape
 *                   `const Factory* (*)(std::string_view) noexcept`.
 *                   Usually `&pal::resolve_<category>`.
 * @tparam IPlugin   The plugin interface (the slot's element type).
 * @tparam Config    Factory config type. Pass `nullptr` for
 *                   config-less factories.
 * @tparam PostCreate  Optional post-create hook (audio3a use case).
 *
 * @param plugin_id   Plugin id (also the resolver key).
 * @param slot        Engine-owned `std::unique_ptr<IPlugin>`.
 * @param config      Config forwarded to `factory->create()`.
 * @param opts        Named-arguments struct.
 * @param post_create Optional hook between create() and open().
 *
 * @return true iff a plugin was created AND opened successfully.
 */
template <auto Resolver, typename IPlugin, typename Config,
          typename PostCreate = std::nullptr_t>
[[nodiscard]] NIMRTC_API_STABLE bool make_plugin(
    std::string_view plugin_id,
    std::unique_ptr<IPlugin>& slot,
    Config&& config,
    const MakePluginOpts& opts,
    PostCreate&& post_create = nullptr) noexcept {
    static_assert(std::is_pointer_v<decltype(Resolver)>,
                  "Resolver must be a function pointer "
                  "(e.g. &pal::resolve_bwe).");
    using FactoryPtr =
        std::remove_pointer_t<std::invoke_result_t<decltype(Resolver), std::string_view>>;
    using Factory = std::remove_cv_t<FactoryPtr>;
    const Factory* factory = Resolver(plugin_id);
    return make_plugin<IPlugin, Config, Factory, PostCreate>(
        factory, plugin_id, slot, std::forward<Config>(config),
        opts, std::forward<PostCreate>(post_create));
}

}  // namespace pal
}  // namespace nimrtc::engine

#endif  // NIMRTC_ENGINE_PAL_MAKE_PLUGIN_HPP
