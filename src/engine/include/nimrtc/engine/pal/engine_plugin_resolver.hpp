/**
 * @file nimrtc/engine/pal/engine_plugin_resolver.hpp
 * @brief PAL Slice 1 — inline plugin-resolution forwarders.
 *
 * Bound by ADR-009 (PAL Slice 1) and shipped in v0.10.0. The seam is
 * intentionally a thin alias layer: each function is a one-line inline
 * forward to `core::PluginRegistry::instance().get_*()` so the
 * generated code is byte-identical to the pre-Slice-1 call sites.
 *
 * Slice 2 (self-registration table) and Slice 3 (compile-time ID
 * validation) are deferred to v0.10.x patches.
 *
 * @see docs/adr/ADR-009-pal-slice-1.md
 * @see docs/plan/pal-architecture.md §4
 */
#ifndef NIMRTC_ENGINE_PAL_ENGINE_PLUGIN_RESOLVER_HPP
#define NIMRTC_ENGINE_PAL_ENGINE_PLUGIN_RESOLVER_HPP

#include <string_view>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/plugins/audio3a.hpp>
#include <nimrtc/plugins/codec.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/plugins/datachannel.hpp>

namespace nimrtc::engine::pal {

/** Resolve an `IAudio3AFactory` by plugin id. Returns nullptr if not registered. */
[[nodiscard]] inline const plugins::IAudio3AFactory*
resolve_audio3a(std::string_view id) noexcept {
    return nimrtc::core::PluginRegistry::instance().get_audio3a(id);
}

/** Resolve an `ICodecFactory` by plugin id. Returns nullptr if not registered. */
[[nodiscard]] inline const plugins::ICodecFactory*
resolve_codec(std::string_view id) noexcept {
    return nimrtc::core::PluginRegistry::instance().get_codec(id);
}

/** Resolve an `IVideoCodecFactory` by plugin id. Returns nullptr if not registered. */
[[nodiscard]] inline const plugins::IVideoCodecFactory*
resolve_video_codec(std::string_view id) noexcept {
    return nimrtc::core::PluginRegistry::instance().get_video_codec(id);
}

/** Resolve an `IDataChannelFactory` by plugin id. Returns nullptr if not
 *  registered.  Mirrors the resolve_audio3a / resolve_codec / resolve_video_codec
 *  helpers — used by `NimRTCEngine::create_data_channel()` to resolve the
 *  `EngineConfig::datachannel_name` factory slot populated by
 *  `core::PluginRegistry::register_datachannel(id, factory*)`. */
[[nodiscard]] inline const plugins::IDataChannelFactory*
resolve_datachannel(std::string_view id) noexcept {
    return nimrtc::core::PluginRegistry::instance().get_datachannel(id);
}

} // namespace nimrtc::engine::pal

#endif // NIMRTC_ENGINE_PAL_ENGINE_PLUGIN_RESOLVER_HPP
