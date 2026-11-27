// audio_device/src/audio_device.cpp
// The module's backend lookup.

#include <nimrtc/audio_device/audio_device.hpp>
#include <nimrtc/audio_device/system_audio.hpp>
#include <nimrtc/core/api_version.hpp>
#include <nimrtc/core/registry.hpp>

#include <string_view>

namespace nimrtc::audio_device {

namespace {

// The factories are `const` objects with static storage duration, so
// the pointers the lookup hands out stay valid for the lifetime of the
// process and the caller must not delete them.
//
// They are NOT global *mutable* state: they are immutable objects whose
// address is published through a lookup function (L6).
const PortAudioSourceFactory g_portaudio_source_factory;
const PortAudioSinkFactory   g_portaudio_sink_factory;
const SyntheticSourceFactory g_synthetic_source_factory;
const SyntheticSinkFactory   g_synthetic_sink_factory;

// The system-audio (loopback) factories — remote-desktop plan F6.
// They are *separate* from the microphone's factories, so a host can
// never resolve "the system output" through the microphone's id.
const SystemAudioSourceFactory g_system_audio_source_factory;
const SyntheticSystemAudioSourceFactory g_synthetic_system_audio_source_factory;

} // namespace

namespace detail {
/** The registration shim (defined at the bottom of this file).
 *
 *  Forward-declared here because the deprecated lookup functions below
 *  have to *ensure* the module's factories are in the registry before
 *  they forward to it — see the comment there. */
void do_register_default_plugins() noexcept;
} // namespace detail

// ---------------------------------------------------------------------------
// The backend lookup (deprecated — ADR-028)
//
// These four functions are the module's *historical* entry points: they
// predate the registry and they answered "which backends exist?" from
// the module's own tables.  ADR-028 moved that answer to
// `core::PluginRegistry`, and the two entry points must not disagree —
// otherwise a third party that registered its own microphone would be
// invisible through one of them.
//
// So all four now forward to the registry.  The module's own tables
// (the `g_*_factory` statics above) are the *authority* for what the
// module registers: `register_default_plugins()` puts exactly these
// factories into the registry's audio-source / audio-sink slots, so the
// registry's answer is the module's answer — plus whatever anybody
// else registered.
//
// The definitions below *name* the deprecated functions, so the
// deprecation warning is suppressed around them — the warning is for
// the *callers*, not for the module's own forwarding shims.
// ---------------------------------------------------------------------------

NIMRTC_SUPPRESS_DEPRECATION_BEGIN

std::vector<std::string> available_source_backends() noexcept {
    // The registry's list is the answer.  It contains this module's
    // factories once they are registered — and any third party's.
    //
    // The module's own list is *not* returned as a fallback: it would
    // be a second answer to the same question, which is exactly what
    // ADR-028 forbids.
    detail::do_register_default_plugins();
    return core::PluginRegistry::instance().list_audio_sources();
}

std::vector<std::string> available_sink_backends() noexcept {
    detail::do_register_default_plugins();
    return core::PluginRegistry::instance().list_audio_sinks();
}

const plugins::IAudioSourceFactory* source_factory(
    std::string_view id) noexcept {
    // The registry owns the answer.  A backend that is not registered
    // resolves to nullptr.
    //
    // The registration is ensured first, so the module's own backends
    // are visible through *both* entry points even when the host never
    // opened an engine (which is what runs
    // `register_all_default_plugins()` in the normal case).
    detail::do_register_default_plugins();
    return core::PluginRegistry::instance().get_audio_source(id);
}

const plugins::IAudioSinkFactory* sink_factory(std::string_view id) noexcept {
    detail::do_register_default_plugins();
    return core::PluginRegistry::instance().get_audio_sink(id);
}

NIMRTC_SUPPRESS_DEPRECATION_END

// ---------------------------------------------------------------------------
// Playback progress (remote-desktop plan F3)
// ---------------------------------------------------------------------------

const PlaybackProgressTracker*
playback_progress_tracker(const plugins::IAudioSink* sink) noexcept {
    if (sink == nullptr) {
        return nullptr;
    }
    const auto* progress_sink =
        dynamic_cast<const IPlaybackProgressSink*>(sink);
    if (progress_sink == nullptr) {
        return nullptr;
    }
    return &progress_sink->playback_progress();
}

std::uint64_t played_frames(const plugins::IAudioSink* sink) noexcept {
    const PlaybackProgressTracker* tracker =
        playback_progress_tracker(sink);
    if (tracker == nullptr) {
        return 0;
    }
    return tracker->frames_played();
}

std::size_t queued_frames(const plugins::IAudioSink* sink) noexcept {
    const PlaybackProgressTracker* tracker =
        playback_progress_tracker(sink);
    if (tracker == nullptr) {
        return 0;
    }
    return tracker->frames_pending();
}

// ---------------------------------------------------------------------------
// The AEC delay (remote-desktop plan F4)
// ---------------------------------------------------------------------------

bool attach_aec_delay_monitor(plugins::IAudioSink* sink,
                              AecDelayMonitor* monitor) noexcept {
    if (sink == nullptr) {
        return false;
    }
    auto* delay_sink = dynamic_cast<IAecDelaySink*>(sink);
    if (delay_sink == nullptr) {
        return false;
    }
    delay_sink->set_aec_delay_monitor(monitor);
    return true;
}

bool attach_aec_delay_monitor(plugins::IAudioSource* source,
                              AecDelayMonitor* monitor) noexcept {
    if (source == nullptr) {
        return false;
    }
    auto* delay_source = dynamic_cast<IAecDelaySource*>(source);
    if (delay_source == nullptr) {
        return false;
    }
    delay_source->set_aec_delay_monitor(monitor);
    return true;
}

namespace {

/** The monitor attached to @p sink, or nullptr. */
AecDelayMonitor* monitor_of(const plugins::IAudioSink* sink) noexcept {
    if (sink == nullptr) {
        return nullptr;
    }
    const auto* delay_sink = dynamic_cast<const IAecDelaySink*>(sink);
    if (delay_sink == nullptr) {
        return nullptr;
    }
    return delay_sink->aec_delay_monitor();
}

} // namespace

std::optional<std::int32_t>
aec_delay_ms(const plugins::IAudioSink* sink) noexcept {
    AecDelayMonitor* monitor = monitor_of(sink);
    if (monitor == nullptr) {
        return std::nullopt;
    }
    return monitor->delay_ms();
}

std::optional<std::int32_t>
aec_delay_headroom_ms(const plugins::IAudioSink* sink) noexcept {
    AecDelayMonitor* monitor = monitor_of(sink);
    if (monitor == nullptr) {
        return std::nullopt;
    }
    return monitor->aec_delay_headroom_ms();
}

float aec_delay_confidence(const plugins::IAudioSink* sink) noexcept {
    AecDelayMonitor* monitor = monitor_of(sink);
    if (monitor == nullptr) {
        return 0.0f;
    }
    return monitor->confidence();
}

// ---------------------------------------------------------------------------
// Registration (v1.4 D1, ADR-028)
// ---------------------------------------------------------------------------

namespace detail {

void do_register_default_plugins() noexcept {
    // The `static` local guarantees the body runs exactly once, no
    // matter how many times the entry point is called.  The factories
    // are the module's immutable statics (L6: not global *mutable*
    // state), so re-registering them is a late-binding overwrite of the
    // same pointer.
    static const struct Registrar {
        Registrar() {
            auto& reg = core::PluginRegistry::instance();

            // The capture devices.  `system-audio` and
            // `synthetic-system-audio` are their OWN ids rather than
            // variants of "portaudio": a host that asks for the
            // microphone never gets the system output by accident.
            reg.register_audio_source(
                g_synthetic_source_factory.id(), &g_synthetic_source_factory);
            reg.register_audio_source(
                g_system_audio_source_factory.id(), &g_system_audio_source_factory);
            reg.register_audio_source(
                g_synthetic_system_audio_source_factory.id(),
                &g_synthetic_system_audio_source_factory);
            if (portaudio_available()) {
                reg.register_audio_source(
                    g_portaudio_source_factory.id(), &g_portaudio_source_factory);
            }

            // The playback devices.
            reg.register_audio_sink(
                g_synthetic_sink_factory.id(), &g_synthetic_sink_factory);
            if (portaudio_available()) {
                reg.register_audio_sink(
                    g_portaudio_sink_factory.id(), &g_portaudio_sink_factory);
            }
        }
    } s_registrar;
    (void)s_registrar;
}

} // namespace detail

void register_default_plugins() noexcept {
    detail::do_register_default_plugins();
}

} // namespace nimrtc::audio_device
