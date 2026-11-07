/**
 * @file nimrtc/audio_rx/audio_rx.hpp
 * @brief Audio receive pipeline (v1.2.0, audio pipeline).
 *
 * The module implements `plugins::IAudioReceiver`: it owns the jitter buffer
 * instance, the decoder instance, the timeline bookkeeping, concealment and
 * FEC recovery for one inbound audio stream.
 *
 * The jitter buffer and the decoder are resolved through the plugin seams
 * (plugins::IJB, plugins::IAudioDecoder) so the module depends on
 * nimrtc::core and nimrtc::plugins only.
 *
 * The engine reaches the module through the registry's audio-receiver slot
 * (the id is "nimrtc_audio_rx"); it never includes this header, because the
 * engine may not depend on a concrete module (constraint L4).
 */
#ifndef NIMRTC_AUDIO_RX_AUDIO_RX_HPP
#define NIMRTC_AUDIO_RX_AUDIO_RX_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).

#include <nimrtc/core/api_version.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace nimrtc::audio_rx {

/** Opaque handle to a receive-pipeline instance. */
class NIMRTC_API_STABLE Receiver;

/**
 * @brief Register the module's default plugins.
 *
 * Called by `core::register_all_default_plugins()` (through the registrar
 * table in `src/core/src/pal_default_registrars.cpp`).  It registers the
 * receive pipeline under the id "nimrtc_audio_rx" in the registry's
 * audio-receiver category.
 *
 * Idempotent: the registration happens once, on the first call.
 */
void NIMRTC_API_STABLE register_default_plugins() noexcept;

} // namespace nimrtc::audio_rx
#endif // NIMRTC_AUDIO_RX_AUDIO_RX_HPP
