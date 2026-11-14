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
#include <nimrtc/plugins/audio_decoder.hpp>
#include <nimrtc/plugins/audio_receiver.hpp>
#include <nimrtc/plugins/bwe.hpp>
#include <nimrtc/plugins/codec.hpp>
#include <nimrtc/plugins/datachannel.hpp>
#include <nimrtc/plugins/dtmf.hpp>
#include <nimrtc/plugins/net_feedback.hpp>
#include <nimrtc/plugins/scheduler.hpp>
#include <nimrtc/plugins/srtp_backend.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/plugins/video_pipeline.hpp>
// Remote-desktop plan A1: the capture-*device* seam.  Kept separate from
// the source seam: a capture device enumerates and negotiates, while a
// source is just a frame stream.
#include <nimrtc/plugins/video_capture.hpp>
#include <nimrtc/plugins/video_sink.hpp>
#include <nimrtc/plugins/video_source.hpp>
// v1.2.0 (session capability plan X1): the RTCP Extended Report seam.
#include <nimrtc/plugins/xr_report.hpp>
// v0.14.0 alpha.2 seam (WebTransport / QUIC session factory).
//
// P1 hardening (2026-11-09): this was an #include of
// <nimrtc/web_transport/web_transport_session_factory.hpp>.  The
// resolver only hands out a POINTER to the factory, so a forward
// declaration is enough -- and it keeps the engine's PUBLIC header from
// depending on an optional module's header (the web_transport include
// path is not propagated to the engine's consumers).
namespace nimrtc::web_transport {
class NIMRTC_API_UNSTABLE IWebTransportSessionFactory;
} // namespace nimrtc::web_transport

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

/** Resolve an `IAudioDecoderFactory` by plugin id. Returns nullptr if not
 *  registered.  Used by the engine to satisfy `EngineConfig::audio_decoder_name`
 *  (default "opus") -- the audio pipeline's codec-side seam. */
[[nodiscard]] inline const plugins::IAudioDecoderFactory*
resolve_audio_decoder(std::string_view id) noexcept {
    return nimrtc::core::PluginRegistry::instance().get_audio_decoder(id);
}

/** Resolve an `IAudioReceiverFactory` by plugin id. Returns nullptr if not
 *  registered.  Used by the engine to satisfy `EngineConfig::audio_receiver_name`
 *  (default "nimrtc_audio_rx") -- the audio pipeline's pipeline-side seam. */
[[nodiscard]] inline const plugins::IAudioReceiverFactory*
resolve_audio_receiver(std::string_view id) noexcept {
    return nimrtc::core::PluginRegistry::instance().get_audio_receiver(id);
}

/** Resolve an `IDtmfCodecFactory` by plugin id. Returns nullptr if not
 *  registered.  Used by the engine to satisfy `EngineConfig::dtmf_codec_name`
 *  (default "dtmf") -- the RFC 4733 telephone-event seam (v1.2.0,
 *  session capability plan D1 / D2). */
[[nodiscard]] inline const plugins::IDtmfCodecFactory*
resolve_dtmf(std::string_view id) noexcept {
    return nimrtc::core::PluginRegistry::instance().get_dtmf(id);
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

/** Resolve an `INetFeedbackFactory` by plugin id. Returns nullptr if not
 *  registered.  Used by the engine to satisfy `EngineConfig::net_feedback_name`
 *  (default "nimrtc_net_feedback") -- the receiver-side network-feedback
 *  collector that sits between the wire and the bandwidth estimator
 *  (v1.2.0 M1, F2). */
[[nodiscard]] inline const plugins::INetFeedbackFactory*
resolve_net_feedback(std::string_view id) noexcept {
    return nimrtc::core::PluginRegistry::instance().get_net_feedback(id);
}

/** Resolve an `IXrReportFactory` by plugin id. Returns nullptr if not
 *  registered.  Used by the engine to satisfy
 *  `EngineConfig::xr_report_name` (default "nimrtc_xr") -- the RTCP
 *  Extended Report consumer that decodes RFC 3611 report blocks
 *  (v1.2.0, session capability plan X1). */
[[nodiscard]] inline const plugins::IXrReportFactory*
resolve_xr_report(std::string_view id) noexcept {
    return nimrtc::core::PluginRegistry::instance().get_xr_report(id);
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

/** Resolve an `IVideoCaptureFactory` by plugin id. Returns nullptr if not
 *  registered.
 *
 *  Used by the engine to satisfy `EngineConfig::video_capture_name`
 *  (empty = no capture device) — the capture-*device* seam of the
 *  remote-desktop plan (A1).  It is distinct from the source seam: a
 *  capture device enumerates devices, negotiates a format and reports
 *  dirty-rect metadata, while a source is a plain frame stream. */
[[nodiscard]] inline const plugins::IVideoCaptureFactory*
resolve_video_capture(std::string_view id) noexcept {
    return nimrtc::core::PluginRegistry::instance().get_video_capture(id);
}

/** Resolve an `IVideoBitrateControllerFactory` by plugin id. Returns
 *  nullptr if not registered.
 *
 *  Used by the engine to satisfy
 *  `EngineConfig::video_bitrate_controller_name` (empty = no
 *  controller) — the resolution / frame-rate / bitrate ladder of the
 *  remote-desktop plan D1 / D2 / D3 / D4. */
[[nodiscard]] inline const plugins::IVideoBitrateControllerFactory*
resolve_video_bitrate_controller(std::string_view id) noexcept {
    return nimrtc::core::PluginRegistry::instance()
        .get_video_bitrate_controller(id);
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

/** Resolve an `ISrtpBackendFactory` by plugin id. Returns nullptr if not
 *  registered.
 *
 *  Used by the engine to satisfy `EngineConfig::srtp_backend_name`
 *  (default "libsrtp") — the pluggable SRTP crypto-backend seam
 *  (v1.2.0, architecture remediation plan B7 / Q3).  An unknown id is a
 *  hard failure at `open()` time; there is no fallback to libsrtp. */
[[nodiscard]] inline const plugins::ISrtpBackendFactory*
resolve_srtp_backend(std::string_view id) noexcept {
    return nimrtc::core::PluginRegistry::instance().get_srtp_backend(id);
}

} // namespace nimrtc::engine::pal

#endif // NIMRTC_ENGINE_PAL_ENGINE_PLUGIN_RESOLVER_HPP
