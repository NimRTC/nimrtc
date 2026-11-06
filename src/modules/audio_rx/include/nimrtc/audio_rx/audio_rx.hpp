/**
 * @file nimrtc/audio_rx/audio_rx.hpp
 * @brief Audio receive pipeline (v1.2.0, audio pipeline).
 *
 * The module implements plugins::IAudioReceiver: it owns the jitter buffer
 * instance, the decoder instance, the timeline bookkeeping, concealment and
 * FEC recovery for one inbound audio stream.
 *
 * The jitter buffer and the decoder are resolved through the plugin seams
 * (plugins::IJB, plugins::IAudioDecoder) so the module depends on
 * nimrtc::core and nimrtc::plugins only.
 *
 * @note Skeleton -- the implementation lands with the v1.2.0 audio work.
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

} // namespace nimrtc::audio_rx
#endif // NIMRTC_AUDIO_RX_AUDIO_RX_HPP
