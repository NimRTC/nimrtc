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

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.

#include <nimrtc/core/api_version.hpp>
#include <string_view>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/plugins/audio3a.hpp>
#include <nimrtc/plugins/bwe.hpp>
#include <nimrtc/plugins/codec.hpp>
#include <nimrtc/plugins/datachannel.hpp>
#include <nimrtc/plugins/scheduler.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/plugins/video_pipeline.hpp>
#include <nimrtc/plugins/video_sink.hpp>
#include <nimrtc/plugins/video_source.hpp>
#include <nimrtc/web_transport/web_transport_session_factory.hpp>   // IWebTransportSessionFactory (v0.14.0 alpha.2 seam)

namespace nimrtc::engine::pal {

/** Resolve an `IAudio3AFactory` by plugin id. Returns nullptr if not registered. */
[[nodiscard]] NIMRTC_API_STABLE inline const plugins::IAudio3AFactory*
resolve_audio3a(std::string_view id) noexcept {
    return nimrtc::core::PluginRegistry::instance().get_audio3a(id);
}

/** Resolve an `ICodecFactory` by plugin id. Returns nullptr if not registered. */
[[nodiscard]] inline const plugins::ICodecFactory*
resolve_codec(std::string_view id) noexcept {
    return nimrtc::core::PluginRegistry::instance().get_codec(id);
}

/** Resolve an `IBweFactory` by plugin id. Returns nullptr if not registered. */
[[nodiscard]] inline const plugins::IBweFactory*
resolve_bwe(std::string_view id) noexcept {
    return nimrtc::core::PluginRegistry::instance().get_bwe(id);
}

/** Resolve an `ISchedulerFactory` by plugin id. Returns nullptr if not registered. */
[[nodiscard]] inline const plugins::ISchedulerFactory*
resolve_scheduler(std::string_view id) noexcept {
    return nimrtc::core::PluginRegistry::instance().get_scheduler(id);
}

/** Resolve an `IVideoCodecFactory` by plugin id. Returns nullptr if not registered. */
[[nodiscard]] inline const plugins::IVideoCodecFactory*
resolve_video_codec(std::string_view id) noexcept {
    return nimrtc::core::PluginRegistry::instance().get_video_codec(id);
}

/** Resolve an `IVideoSinkFactory` by plugin id. Returns nullptr if not registered. */
[[nodiscard]] inline const plugins::IVideoSinkFactory*
resolve_video_sink(std::string_view id) noexcept {
    return nimrtc::core::PluginRegistry::instance().get_video_sink(id);
}

/** Resolve an `IVideoSourceFactory` by plugin id. Returns nullptr if not registered. */
[[nodiscard]] inline const plugins::IVideoSourceFactory*
resolve_video_source(std::string_view id) noexcept {
    return nimrtc::core::PluginRegistry::instance().get_video_source(id);
}

/** Resolve an `IVideoReceiverFactory` by plugin id. Returns nullptr if not registered. */
[[nodiscard]] inline const plugins::IVideoReceiverFactory*
resolve_video_receiver(std::string_view id) noexcept {
    return nimrtc::core::PluginRegistry::instance().get_video_receiver(id);
}

/** Resolve an `IVideoSenderFactory` by plugin id. Returns nullptr if not registered. */
[[nodiscard]] inline const plugins::IVideoSenderFactory*
resolve_video_sender(std::string_view id) noexcept {
    return nimrtc::core::PluginRegistry::instance().get_video_sender(id);
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

/** Resolve an `IWebTransportSessionFactory` by plugin id. Returns nullptr if not
 *  registered.  Mirrors the resolve_audio3a / resolve_datachannel family —
 *  used by `NimRTCEngine::init_datachannel_registry()` to verify the
 *  `EngineConfig::web_transport_name` factory slot (default `"msquic"`)
 *  when `EngineConfig::datachannel_backend == "web_transport"`.  The slot
 *  is populated by `core::PluginRegistry::register_web_transport(id, factory*)`
 *  or the v0.14.0-beta.1 auto-registrar (id `"msquic"` → msquic-backed
 *  factory).  Landed in v0.14.0-rc.1 alongside `datachannel_backend`. */
[[nodiscard]] inline const web_transport::IWebTransportSessionFactory*
resolve_web_transport(std::string_view id) noexcept {
    return nimrtc::core::PluginRegistry::instance().get_web_transport(id);
}

} // namespace nimrtc::engine::pal

#endif // NIMRTC_ENGINE_PAL_ENGINE_PLUGIN_RESOLVER_HPP
