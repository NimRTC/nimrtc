/**
 * @file src/engine/src/engine_stats_state.hpp
 * @brief PRIVATE engine collaborator: the statistics registry state
 *        (plan Track S / S1, design plan DS-1).
 *
 * Architecture remediation plan §4.3 (D1 — engine split): the
 * statistics state used to be nothing but the flat `Engine::Stats`
 * struct, filled on every `stats()` call from a dozen collaborators.
 * This header adds the *registry* half: the named, snapshot-able
 * view of the same numbers, with a timestamp and a sequence number.
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * ## The one authority
 *
 * The flat `Engine::Stats` fields stay the **authority**: they are
 * computed from the live engine state exactly as they always were.
 * The registry is a *projection* of that struct, taken at reading
 * time.  The projection therefore cannot drift from the flat fields —
 * there is no second copy of the bookkeeping that could disagree.
 *
 * ## The counter table
 *
 * The table below is the single list of "which flat field maps to
 * which registry entry, and is it a counter or a gauge".  It is an
 * X-macro list so that the projection cannot fall out of step with
 * the struct: the field is looked up by name at compile time, so a
 * rename breaks the build rather than silently dropping the field
 * from the registry.
 *
 * ## Semantics
 *
 * `kCumulative` — a monotonically increasing counter.  The delta of
 * two readings is the increment that happened in between, so a host
 * can turn it into a rate once it knows the interval's length.
 *
 * `kGauge` — an instantaneous value (a bitrate, a jitter, a queue
 * depth, an EMA).  The delta of two readings is *meaningless*, so the
 * delta API refuses to produce one.
 *
 * Getting this classification wrong is the classic statistics bug, so
 * the classification lives next to the field it describes rather than
 * in a document far away.
 *
 * ## The sequence number
 *
 * The sequence number advances **once per published reading**.  Every
 * public entry point (`stats()`, `stats_snapshot()`,
 * `stats_delta_since()`) publishes the current flat statistics into
 * the registry and takes the *same* sequence number for its own
 * result, so the three views of one instant agree with each other.
 *
 * ## Threading
 *
 * The engine serialises its work on the caller's thread, but a host
 * may poll `stats()` from another thread while the engine thread is
 * running.  The registry is therefore guarded by its own mutex: the
 * projection and the reading are taken together, so a reading is
 * never a torn mixture of two instants.
 *
 * The mutex is taken once per call, not once per counter: the
 * declaration indices are resolved once and cached, so the hot path
 * is an array walk rather than a hundred hash lookups.
 */

#pragma once

#include <cstdint>
#include <mutex>
#include <string_view>
#include <vector>

#include <nimrtc/core/stats_registry.hpp>

#include "nimrtc/engine/engine.hpp"

namespace nimrtc::engine::detail {

// ---------------------------------------------------------------------------
// The counter table
// ---------------------------------------------------------------------------
//
// X(name, semantics) — one line per numeric field of `Engine::Stats`.
//
// The name is the flat field's name, so the mapping between the
// public struct and the registry entry is a lookup, not a guess.
//
// Only the *numeric* fields are listed.  The boolean flags
// (`video_capture_gpu_wired`, `video_codec_hardware`) and the string
// fields (`video_codec_backend`, `video_format_canonical`) are not
// counters and have no increment; they stay reachable through the
// flat struct.

#define NIMRTC_ENGINE_STATS_COUNTERS(X)                                              \
    /* ---- SRTP ---- */                                                            \
    X(srtp_drops, core::StatSemantics::kCumulative)                                  \
    /* ---- Video send / receive ---- */                                            \
    X(video_frames_sent, core::StatSemantics::kCumulative)                           \
    X(video_packets_sent, core::StatSemantics::kCumulative)                          \
    X(video_bytes_sent, core::StatSemantics::kCumulative)                            \
    X(video_frames_received, core::StatSemantics::kCumulative)                       \
    X(video_nack_seqs_queued, core::StatSemantics::kCumulative)                      \
    X(video_keyframes_q, core::StatSemantics::kCumulative)                           \
    X(video_feedback_applied, core::StatSemantics::kCumulative)                      \
    X(video_keyframes_applied, core::StatSemantics::kCumulative)                     \
    /* ---- RTCP feedback, by type ---- */                                          \
    X(video_feedback_rx_pli, core::StatSemantics::kCumulative)                       \
    X(video_feedback_rx_fir, core::StatSemantics::kCumulative)                       \
    X(video_feedback_rx_nack, core::StatSemantics::kCumulative)                      \
    X(video_feedback_rx_remb, core::StatSemantics::kCumulative)                      \
    X(video_feedback_rx_transport_cc, core::StatSemantics::kCumulative)              \
    X(video_feedback_tx_pli, core::StatSemantics::kCumulative)                       \
    X(video_feedback_tx_nack, core::StatSemantics::kCumulative)                      \
    /* ---- The receive-side feedback bound (v1.3 Q4 / video plan T2) ---- */        \
    X(video_feedback_pli_requests, core::StatSemantics::kCumulative)                 \
    X(video_feedback_pli_allowed, core::StatSemantics::kCumulative)                  \
    X(video_feedback_pli_suppressed, core::StatSemantics::kCumulative)               \
    X(video_feedback_pli_from_loss, core::StatSemantics::kCumulative)                \
    X(video_feedback_pli_from_decode_failure, core::StatSemantics::kCumulative)      \
    X(video_feedback_nack_seqs_dropped, core::StatSemantics::kCumulative)            \
    X(video_feedback_reports_expired, core::StatSemantics::kCumulative)              \
    /* ---- The key-frame storm limiter ---- */                                      \
    X(video_keyframes_limiter_requests, core::StatSemantics::kCumulative)            \
    X(video_keyframes_granted, core::StatSemantics::kCumulative)                     \
    X(video_keyframes_deferred, core::StatSemantics::kCumulative)                    \
    X(video_keyframes_rejected, core::StatSemantics::kCumulative)                    \
    X(video_keyframes_forced, core::StatSemantics::kCumulative)                      \
    /* ---- The incremental (dirty-rect) encode ---- */                              \
    X(video_incremental_frames_seen, core::StatSemantics::kCumulative)               \
    X(video_incremental_unchanged, core::StatSemantics::kCumulative)                 \
    X(video_incremental_encoded, core::StatSemantics::kCumulative)                   \
    X(video_incremental_full, core::StatSemantics::kCumulative)                      \
    X(video_incremental_fallbacks, core::StatSemantics::kCumulative)                 \
    X(video_incremental_skipped, core::StatSemantics::kCumulative)                   \
    X(video_region_refreshes_sent, core::StatSemantics::kCumulative)                 \
    /* ---- The zero-copy path ---- */                                              \
    X(video_zero_copy_frames, core::StatSemantics::kCumulative)                      \
    X(video_zero_copy_fallbacks, core::StatSemantics::kCumulative)                   \
    X(video_rx_gpu_frames, core::StatSemantics::kCumulative)                         \
    X(video_rx_cpu_frames, core::StatSemantics::kCumulative)                         \
    X(video_zero_copy_rejected, core::StatSemantics::kCumulative)                    \
    /* ---- The capture device ---- */                                               \
    X(video_capture_frames, core::StatSemantics::kCumulative)                        \
    X(video_capture_frames_pumped, core::StatSemantics::kCumulative)                 \
    X(video_capture_dropped, core::StatSemantics::kCumulative)                       \
    X(video_capture_device_errors, core::StatSemantics::kCumulative)                  \
    X(video_capture_restarts, core::StatSemantics::kCumulative)                      \
    X(video_capture_format_changes, core::StatSemantics::kCumulative)                \
    X(video_capture_bytes_copied, core::StatSemantics::kCumulative)                  \
    /* ---- The codec ---- */                                                        \
    X(video_codec_bitrate_bps, core::StatSemantics::kGauge)                          \
    X(video_codec_actual_bps, core::StatSemantics::kGauge)                           \
    X(video_codec_encode_latency_us, core::StatSemantics::kGauge)                    \
    X(video_codec_frames_encoded, core::StatSemantics::kCumulative)                  \
    X(video_codec_bytes_encoded, core::StatSemantics::kCumulative)                   \
    X(video_codec_encode_errors, core::StatSemantics::kCumulative)                   \
    X(video_codec_zero_copy_frames, core::StatSemantics::kCumulative)                \
    X(video_codec_staged_frames, core::StatSemantics::kCumulative)                   \
    /* ---- The video-quality sink ---- */                                           \
    X(video_freeze_count, core::StatSemantics::kCumulative)                          \
    X(video_freeze_total_ms, core::StatSemantics::kCumulative)                       \
    X(video_last_freeze_ms, core::StatSemantics::kGauge)                             \
    X(video_keyframe_interval_avg_ms, core::StatSemantics::kGauge)                   \
    X(video_qp_sum, core::StatSemantics::kCumulative)                                 \
    X(video_qp_count, core::StatSemantics::kCumulative)                               \
    X(video_frames_dropped_jb, core::StatSemantics::kCumulative)                     \
    X(video_frames_dropped_decoder, core::StatSemantics::kCumulative)                \
    X(video_frames_dropped_dependency, core::StatSemantics::kCumulative)             \
    X(video_current_bitrate_bps, core::StatSemantics::kGauge)                       \
    X(video_avg_bitrate_bps, core::StatSemantics::kGauge)                            \
    X(video_bytes_received, core::StatSemantics::kCumulative)                        \
    X(video_frames_decoded, core::StatSemantics::kCumulative)                        \
    X(video_frames_rendered, core::StatSemantics::kCumulative)                       \
    X(video_tx_frames_encoded, core::StatSemantics::kCumulative)                     \
    X(video_tx_bytes_encoded, core::StatSemantics::kCumulative)                      \
    X(video_tx_frames_dropped_dependency, core::StatSemantics::kCumulative)          \
    /* ---- Audio ---- */                                                           \
    X(audio_frames_sent, core::StatSemantics::kCumulative)                            \
    X(audio_bytes_sent, core::StatSemantics::kCumulative)                             \
    X(audio_encode_errors, core::StatSemantics::kCumulative)                          \
    X(audio_samples_in, core::StatSemantics::kCumulative)                             \
    X(audio_samples_pending, core::StatSemantics::kGauge)                            \
    X(audio_packets_sent, core::StatSemantics::kCumulative)                          \
    X(audio_frames_received, core::StatSemantics::kCumulative)                       \
    X(audio_samples_received, core::StatSemantics::kCumulative)                      \
    X(audio_concealed_frames, core::StatSemantics::kCumulative)                      \
    X(audio_fec_recovered_frames, core::StatSemantics::kCumulative)                  \
    X(audio_packets_lost, core::StatSemantics::kCumulative)                          \
    X(audio_decode_errors, core::StatSemantics::kCumulative)                         \
    X(audio_packets_dropped, core::StatSemantics::kCumulative)                        \
    X(audio_jitter_ms, core::StatSemantics::kGauge)                                  \
    X(audio_playout_delay_ms, core::StatSemantics::kGauge)                           \
    X(audio_encoder_bitrate_bps, core::StatSemantics::kGauge)                        \
    /* ---- The network-feedback / BWE loop ---- */                                  \
    X(net_feedback_blocks, core::StatSemantics::kCumulative)                         \
    X(bwe_feedback_samples, core::StatSemantics::kCumulative)                        \
    X(bandwidth_estimate_bps, core::StatSemantics::kGauge)                           \
    /* ---- DTMF ---- */                                                             \
    X(dtmf_packets_sent, core::StatSemantics::kCumulative)                           \
    X(dtmf_packets_received, core::StatSemantics::kCumulative)                       \
    X(dtmf_packets_malformed, core::StatSemantics::kCumulative)                      \
    X(dtmf_events_delivered, core::StatSemantics::kCumulative)                       \
    X(dtmf_payload_type, core::StatSemantics::kGauge)                                \
    /* ---- RTCP XR ---- */                                                         \
    X(xr_report_blocks, core::StatSemantics::kCumulative)                            \
    /* ---- The capture device's own counters ---- */                                \
    X(video_capture_device_frames_captured, core::StatSemantics::kCumulative)        \
    X(video_capture_device_frames_dropped, core::StatSemantics::kCumulative)         \
    X(video_capture_device_error_count, core::StatSemantics::kCumulative)            \
    X(video_capture_device_restarts, core::StatSemantics::kCumulative)                \
    X(video_capture_device_format_changes, core::StatSemantics::kCumulative)         \
    X(video_capture_device_bytes_copied, core::StatSemantics::kCumulative)           \
    X(video_capture_device_frames_zero_copy, core::StatSemantics::kCumulative)       \
    X(video_capture_device_frames_cpu_staged, core::StatSemantics::kCumulative)      \
    /* ---- The capture -> engine pump ---- */                                       \
    X(video_capture_pumped_frames, core::StatSemantics::kCumulative)                 \
    X(video_capture_pump_errors, core::StatSemantics::kCumulative)                   \
    X(video_capture_pumped_metadata, core::StatSemantics::kCumulative)               \
    X(video_capture_gpu_frames, core::StatSemantics::kCumulative)                    \
    X(video_capture_gpu_errors, core::StatSemantics::kCumulative)                    \
    /* ---- The format normalisation ---- */                                         \
    X(video_format_conversions, core::StatSemantics::kCumulative)                    \
    X(video_format_copies, core::StatSemantics::kCumulative)                         \
    X(video_format_zero_copy, core::StatSemantics::kCumulative)                      \
    /* ---- The codec, again (the engine-side view) ---- */                           \
    X(video_codec_frames_zero_copy, core::StatSemantics::kCumulative)                \
    X(video_codec_frames_staged, core::StatSemantics::kCumulative)                   \
    X(video_codec_keyframes, core::StatSemantics::kCumulative)                       \
    X(video_codec_bytes_copied, core::StatSemantics::kCumulative)                    \
    X(video_codec_encode_latency_avg_us, core::StatSemantics::kGauge)                \
    X(video_codec_encode_latency_max_us, core::StatSemantics::kGauge)                \
    X(video_tx_bitrate_bps, core::StatSemantics::kGauge)                             \
    /* ---- The sink ---- */                                                        \
    X(video_sink_frames_rendered, core::StatSemantics::kCumulative)                  \
    X(video_sink_frames_dropped, core::StatSemantics::kCumulative)                   \
    X(video_sink_frames_dropped_present, core::StatSemantics::kCumulative)          \
    X(video_sink_frames_presented, core::StatSemantics::kCumulative)                 \
    X(video_sink_render_errors, core::StatSemantics::kCumulative)                    \
    X(video_sink_present_fps, core::StatSemantics::kGauge)                           \
    /* ---- The per-stage CPU-time instrumentation ---- */                            \
    X(video_stage_total_us, core::StatSemantics::kGauge)                            \
    X(video_stage_total_p95_us, core::StatSemantics::kGauge)

/**
 * @brief The engine's statistics registry state.
 *
 * Holds the `core::StatsRegistry` and knows how to project the flat
 * `Engine::Stats` into it.
 */
struct StatsState {
    /**
     * @brief Publish the flat stats into the registry.
     *
     * The flat struct is the authority; this copies its numeric
     * fields into the registry's entries.  The declaration indices
     * are resolved once, on the first call, so the hot path is an
     * array walk.
     *
     * @param flat    The flat statistics.
     * @param time_us The reading's timestamp, in microseconds on the
     *                engine's monotonic clock.
     *
     * @return The sequence number of the reading.
     */
    std::uint64_t publish(const NimRTCEngine::Stats& flat, std::int64_t time_us) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_declared();
        publish_locked(flat, time_us);
        return seq_;
    }

    /**
     * @brief Take a reading of the registry *without* publishing.
     *
     * The publication already happened (see `publish()`), so this
     * only reads the values the registry holds and stamps the
     * reading with the sequence number and timestamp the publication
     * used.  The reading and the publication therefore describe the
     * *same* instant.
     *
     * @param seq     The sequence number of the publication.
     * @param time_us The publication's timestamp, in microseconds on
     *                the engine's monotonic clock.
     */
    core::StatSnapshot snapshot_at(std::uint64_t seq, std::int64_t time_us) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_declared();
        return registry_.snapshot_at(seq, time_us);
    }

    /**
     * @brief Diff the registry's current values against a previous
     *        reading.
     *
     * The publication already happened (see `publish()`), so this
     * only diffs — it does not publish again.
     *
     * The *end* of the interval is read from the registry, which holds
     * the projection of the flat statistics taken by the `publish()`
     * that preceded this call.  The projection and the flat struct
     * cannot drift apart (the flat struct is the authority and the
     * publication copies it), so the registry is the right source for
     * the end of the interval — and it is the only source that can
     * answer *by name*, which is what the diff walks.
     *
     * @param previous The earlier reading.
     * @param from     The lookup for the baseline's values.
     */
    core::StatsDelta diff_since(const core::StatSnapshot& previous,
                                 const core::StatLookup& from) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_declared();

        core::StatLookup to = [this](std::string_view group,
                                     std::string_view name,
                                     std::int64_t& out) {
            out = registry_.value_of(group, name);
            return true;
        };
        return registry_.diff(previous.seq(), previous.time_us(), from, seq_,
                              last_time_us_, to);
    }

    /**
     * @brief The sequence number of the most recent reading.
     *
     * Zero means "no reading yet".
     */
    std::uint64_t sequence() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return seq_;
    }

private:
    void publish_locked(const NimRTCEngine::Stats& flat, std::int64_t time_us) {
        std::size_t i = 0;
#define NIMRTC_ENGINE_STATS_SET(name, semantics) \
    registry_.set_at(indices_[i++], static_cast<std::int64_t>(flat.name));
        NIMRTC_ENGINE_STATS_COUNTERS(NIMRTC_ENGINE_STATS_SET)
#undef NIMRTC_ENGINE_STATS_SET
        last_time_us_ = time_us;
        ++seq_;
    }

    void ensure_declared() {
        if (declared_) return;
        indices_.reserve(128);
#define NIMRTC_ENGINE_STATS_DECLARE(name, semantics) \
    registry_.declare("", #name, semantics);           \
    indices_.push_back(registry_.index_of("", #name));
        NIMRTC_ENGINE_STATS_COUNTERS(NIMRTC_ENGINE_STATS_DECLARE)
#undef NIMRTC_ENGINE_STATS_DECLARE
        declared_ = true;
    }

    mutable std::mutex mutex_;
    core::StatsRegistry registry_;
    std::vector<std::size_t> indices_;
    std::uint64_t seq_ = 0;
    std::int64_t last_time_us_ = 0;
    bool declared_ = false;
};

}  // namespace nimrtc::engine::detail
