/**
 * @file src/modules/video_stats/src/video_stats.cpp
 * @brief Per-stream video statistics implementation.
 *
 * See include/nimrtc/video_stats/video_stats.hpp for the design overview.
 *
 * ## Concurrency model
 *
 *   - Counters (frames_received, bytes_received, …) are `std::atomic<uint64_t>`,
 *     so individual increments from multiple threads are race-free.
 *   - The keyframe-interval EMA uses `std::atomic<double>` for the running
 *     mean value (last_keyframe_ts_us is `std::atomic<int64_t>`). EMA updates
 *     are rare (one per keyframe) so even if the platform emulates
 *     atomic<double> with a lock, contention is negligible.
 *   - The bitrate sliding window is a small `std::deque` guarded by a mutex.
 *     Each `on_frame_received` appends one entry; `tick()` sweeps old entries
 *     and computes the EMA. The critical section is short (push / scan /
 *     pop_front), and tick is called at ~1 Hz so contention is minimal.
 *   - The freeze detector tracks last-rendered timestamp and last-tick
 *     counters in atomics. No mutex is needed for freeze state.
 *
 * @note P1. Standalone module; not yet wired into the engine.
 */

#include <nimrtc/video_stats/video_stats.hpp>

#include <nimrtc/core/log.hpp>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

namespace nimrtc::video_stats {

namespace {

// ---------------------------------------------------------------------------
// One entry in the bitrate sliding window: bytes received at a given
// capture timestamp.  We use the capture clock (media time) rather than
// wall-clock `now_us` so the rate reflects the encoder's frame cadence
// even if the wall clock is jumpy.
// ---------------------------------------------------------------------------
struct BitrateSample {
    std::int64_t  capture_ts_us = 0;
    std::uint32_t bytes         = 0;
};

// ---------------------------------------------------------------------------
// Concrete implementation
// ---------------------------------------------------------------------------
class VideoStatsSink final : public IVideoStatsSink {
public:
    explicit VideoStatsSink(StatsConfig cfg) : cfg_(cfg) {}

    // -----------------------------------------------------------------------
    // on_frame_received
    // -----------------------------------------------------------------------
    void on_frame_received(std::int64_t capture_ts_us,
                           std::uint32_t frame_size_bytes,
                           bool          is_keyframe,
                           int           qp) override {
        // Counters
        frames_received_.fetch_add(1, std::memory_order_relaxed);
        if (frame_size_bytes > 0) {
            bytes_received_.fetch_add(frame_size_bytes,
                                      std::memory_order_relaxed);
        }

        // Timeline (only update if caller passed a valid timestamp).
        if (capture_ts_us >= 0) {
            // First-ever frame: seed first_frame_ts_us.
            std::int64_t expected = -1;
            if (first_frame_ts_us_.compare_exchange_strong(
                    expected, capture_ts_us,
                    std::memory_order_acq_rel)) {
                // We just wrote the first timestamp.
            }
            last_frame_ts_us_.store(capture_ts_us,
                                    std::memory_order_release);
        }

        // Push into the bitrate sliding window.
        if (capture_ts_us >= 0 && frame_size_bytes > 0) {
            std::lock_guard<std::mutex> lk(bitrate_mutex_);
            bitrate_window_.push_back(
                BitrateSample{capture_ts_us, frame_size_bytes});
        }

        // QP sample (only if codec supplied a valid one).
        if (qp >= 0 && qp <= 63) {
            qp_sum_.fetch_add(static_cast<std::uint64_t>(qp),
                              std::memory_order_relaxed);
            qp_count_.fetch_add(1, std::memory_order_relaxed);
        }

        // Keyframe accounting + EMA of inter-keyframe gap.
        if (is_keyframe) {
            handle_keyframe(capture_ts_us);
        }
    }

    // -----------------------------------------------------------------------
    // on_frame_decoded
    // -----------------------------------------------------------------------
    void on_frame_decoded(std::int64_t /*capture_ts_us*/) override {
        frames_decoded_.fetch_add(1, std::memory_order_relaxed);
    }

    // -----------------------------------------------------------------------
    // on_frame_rendered
    // -----------------------------------------------------------------------
    void on_frame_rendered(std::int64_t capture_ts_us) override {
        const auto rendered_after = frames_rendered_.fetch_add(
            1, std::memory_order_acq_rel);

        // Freeze detection: gap between consecutive rendered frames.
        if (capture_ts_us >= 0) {
            std::int64_t expected = -1;
            if (last_rendered_ts_us_.compare_exchange_strong(
                    expected, capture_ts_us,
                    std::memory_order_acq_rel)) {
                // First ever rendered frame — no gap to measure.
            } else {
                const std::int64_t prev =
                    last_rendered_ts_us_.load(std::memory_order_acquire);
                const std::int64_t gap_us = capture_ts_us - prev;
                if (gap_us >= 0
                    && static_cast<std::uint64_t>(gap_us)
                           >= static_cast<std::uint64_t>(cfg_.freeze_threshold_ms)
                                  * 1000ull) {
                    const std::uint64_t gap_ms =
                        static_cast<std::uint64_t>(gap_us / 1000);
                    freeze_count_.fetch_add(1, std::memory_order_relaxed);
                    freeze_total_ms_.fetch_add(gap_ms,
                                               std::memory_order_relaxed);
                    last_freeze_ms_.store(gap_ms,
                                          std::memory_order_relaxed);
                }
                last_rendered_ts_us_.store(capture_ts_us,
                                            std::memory_order_release);
            }
        }

        // Rendered >= decoded (encoder may produce a frame we can't decode;
        // that's fine, the renderer never sees it).
        (void)rendered_after;
    }

    // -----------------------------------------------------------------------
    // on_frame_dropped
    // -----------------------------------------------------------------------
    void on_frame_dropped(DropReason reason) override {
        switch (reason) {
            case DropReason::kJitterBufferTimeout:
                frames_dropped_jb_.fetch_add(1, std::memory_order_relaxed);
                break;
            case DropReason::kDecoderError:
                frames_dropped_decoder_.fetch_add(1,
                                                  std::memory_order_relaxed);
                break;
            case DropReason::kDependencyLost:
                // Dependency-loss drops are not tallied separately from
                // decoder-error drops in the spec; count them as decoder
                // drops because that's where the upstream reports them.
                frames_dropped_decoder_.fetch_add(1,
                                                  std::memory_order_relaxed);
                break;
        }
    }

    // -----------------------------------------------------------------------
    // on_rtcp_feedback
    // -----------------------------------------------------------------------
    void on_rtcp_feedback(RtcpFeedbackKind kind) override {
        // The base counters (PLI/FIR/NACK/REMB) are tracked only via the
        // public VideoStreamStats fields we already expose: dropped-frames
        // tally captures NACK-induced drops via on_frame_dropped. For
        // observability of raw feedback events, we just log at debug level.
        (void)kind;
        nimrtc::core::log::debug(
            "video_stats: rtcp feedback event received");
    }

    // -----------------------------------------------------------------------
    // on_qp_sample
    // -----------------------------------------------------------------------
    void on_qp_sample(int qp) override {
        if (qp < 0 || qp > 63) return;
        qp_sum_.fetch_add(static_cast<std::uint64_t>(qp),
                          std::memory_order_relaxed);
        qp_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // -----------------------------------------------------------------------
    // tick — drive bitrate window + freeze detector
    // -----------------------------------------------------------------------
    void tick(std::int64_t now_us) override {
        if (now_us < 0) return;

        // 1) Freeze detection via "no frames rendered between ticks".
        //
        //    We compare the count of rendered frames since the last tick.
        //    If zero AND enough wall-clock time has passed since the last
        //    rendered frame, that's a freeze.
        const auto rendered_now = frames_rendered_.load(
            std::memory_order_acquire);
        const auto rendered_prev = rendered_at_last_tick_.exchange(
            rendered_now, std::memory_order_acq_rel);

        if (rendered_now == rendered_prev) {
            // No frames rendered since last tick.
            const std::int64_t last_rendered = last_rendered_ts_us_.load(
                std::memory_order_acquire);
            if (last_rendered >= 0 && now_us > last_rendered) {
                const std::int64_t idle_us = now_us - last_rendered;
                if (static_cast<std::uint64_t>(idle_us)
                    >= static_cast<std::uint64_t>(cfg_.freeze_threshold_ms)
                           * 1000ull) {
                    const std::uint64_t idle_ms =
                        static_cast<std::uint64_t>(idle_us / 1000);
                    freeze_count_.fetch_add(1,
                                            std::memory_order_relaxed);
                    freeze_total_ms_.fetch_add(
                        idle_ms, std::memory_order_relaxed);
                    last_freeze_ms_.store(idle_ms,
                                          std::memory_order_relaxed);
                    // Anchor future freezes to "now" so we don't keep
                    // attributing the same idle period on every tick.
                    last_rendered_ts_us_.store(now_us,
                                               std::memory_order_release);
                }
            }
        }
        last_tick_us_.store(now_us, std::memory_order_release);

        // 2) Bitrate sliding window: prune, compute, EMA.
        const std::uint64_t window_us =
            static_cast<std::uint64_t>(cfg_.bitrate_window_ms) * 1000ull;
        const std::uint64_t total_bytes_in_window = [&] {
            std::lock_guard<std::mutex> lk(bitrate_mutex_);
            const std::int64_t cutoff =
                static_cast<std::int64_t>(now_us)
                - static_cast<std::int64_t>(window_us);
            while (!bitrate_window_.empty()
                   && bitrate_window_.front().capture_ts_us < cutoff) {
                bitrate_window_.pop_front();
            }
            std::uint64_t total = 0;
            for (const auto& s : bitrate_window_) {
                total += s.bytes;
            }
            return total;
        }();

        // Instantaneous bps over the window. Avoid divide-by-zero when
        // window_ms is 0 (defensive — default is 1000 ms).
        const double window_seconds =
            static_cast<double>(cfg_.bitrate_window_ms) / 1000.0;
        const double sample_bps =
            window_seconds > 0.0
                ? (static_cast<double>(total_bytes_in_window) * 8.0
                   / window_seconds)
                : 0.0;

        // EMA: seed on the first sample so the first reading reflects the
        // actual rate (not alpha * rate). Subsequent updates use the
        // standard EMA formula.
        const std::uint64_t sample_count = bitrate_samples_.fetch_add(
            1, std::memory_order_acq_rel);
        if (sample_count == 0) {
            bitrate_ema_.store(sample_bps, std::memory_order_release);
        } else {
            const double prev = bitrate_ema_.load(
                std::memory_order_acquire);
            const double alpha = cfg_.ema_alpha;
            const double next = alpha * sample_bps + (1.0 - alpha) * prev;
            bitrate_ema_.store(next, std::memory_order_release);
        }
    }

    // -----------------------------------------------------------------------
    // snapshot — read all counters atomically and return a consistent view
    // -----------------------------------------------------------------------
    VideoStreamStats snapshot() const override {
        VideoStreamStats s;

        s.frames_received        = frames_received_.load(
            std::memory_order_relaxed);
        s.frames_decoded         = frames_decoded_.load(
            std::memory_order_relaxed);
        s.frames_rendered        = frames_rendered_.load(
            std::memory_order_relaxed);
        s.frames_dropped_jb      = frames_dropped_jb_.load(
            std::memory_order_relaxed);
        s.frames_dropped_decoder = frames_dropped_decoder_.load(
            std::memory_order_relaxed);
        s.keyframes_received     = keyframes_received_.load(
            std::memory_order_relaxed);
        s.bytes_received         = bytes_received_.load(
            std::memory_order_relaxed);
        s.qp_sum                 = qp_sum_.load(std::memory_order_relaxed);
        s.qp_count               = qp_count_.load(
            std::memory_order_relaxed);
        s.freeze_count           = freeze_count_.load(
            std::memory_order_relaxed);
        s.freeze_total_ms        = freeze_total_ms_.load(
            std::memory_order_relaxed);
        s.last_freeze_ms         = last_freeze_ms_.load(
            std::memory_order_relaxed);
        s.first_frame_ts_us      = first_frame_ts_us_.load(
            std::memory_order_acquire);
        s.last_frame_ts_us       = last_frame_ts_us_.load(
            std::memory_order_acquire);

        s.keyframe_interval_avg_ms = keyframe_interval_avg_ms_.load(
            std::memory_order_acquire);

        // Current bitrate is the running EMA (set by tick()).
        const double ema = bitrate_ema_.load(std::memory_order_acquire);
        s.current_bitrate_bps = static_cast<std::uint32_t>(
            ema > 0.0 ? ema + 0.5 : 0.0);

        // Session-average bitrate = bytes * 8 / session duration.
        if (s.first_frame_ts_us >= 0 && s.last_frame_ts_us > s.first_frame_ts_us) {
            const double dur_s =
                static_cast<double>(s.last_frame_ts_us - s.first_frame_ts_us)
                / 1'000'000.0;
            if (dur_s > 0.0) {
                const double avg_bps =
                    (static_cast<double>(s.bytes_received) * 8.0) / dur_s;
                s.avg_bitrate_bps = static_cast<std::uint32_t>(
                    avg_bps > 0.0 ? avg_bps + 0.5 : 0.0);
            }
        }

        return s;
    }

    // -----------------------------------------------------------------------
    // reset — clear all counters
    // -----------------------------------------------------------------------
    void reset() override {
        frames_received_.store(0, std::memory_order_relaxed);
        frames_decoded_.store(0, std::memory_order_relaxed);
        frames_rendered_.store(0, std::memory_order_relaxed);
        frames_dropped_jb_.store(0, std::memory_order_relaxed);
        frames_dropped_decoder_.store(0, std::memory_order_relaxed);
        keyframes_received_.store(0, std::memory_order_relaxed);
        bytes_received_.store(0, std::memory_order_relaxed);
        qp_sum_.store(0, std::memory_order_relaxed);
        qp_count_.store(0, std::memory_order_relaxed);
        freeze_count_.store(0, std::memory_order_relaxed);
        freeze_total_ms_.store(0, std::memory_order_relaxed);
        last_freeze_ms_.store(0, std::memory_order_relaxed);

        first_frame_ts_us_.store(-1, std::memory_order_release);
        last_frame_ts_us_.store(-1, std::memory_order_release);
        last_rendered_ts_us_.store(-1, std::memory_order_release);
        last_keyframe_ts_us_.store(-1, std::memory_order_release);
        last_tick_us_.store(-1, std::memory_order_release);

        keyframe_interval_avg_ms_.store(0.0, std::memory_order_release);
        bitrate_ema_.store(0.0, std::memory_order_release);
        bitrate_samples_.store(0, std::memory_order_relaxed);
        rendered_at_last_tick_.store(0, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lk(bitrate_mutex_);
            bitrate_window_.clear();
        }
    }

private:
    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------
    void handle_keyframe(std::int64_t capture_ts_us) {
        keyframes_received_.fetch_add(1, std::memory_order_relaxed);

        if (capture_ts_us < 0) return;

        std::int64_t expected = -1;
        if (last_keyframe_ts_us_.compare_exchange_strong(
                expected, capture_ts_us,
                std::memory_order_acq_rel)) {
            // First keyframe — no inter-keyframe gap to measure.
            return;
        }
        const std::int64_t prev = last_keyframe_ts_us_.load(
            std::memory_order_acquire);
        if (capture_ts_us <= prev) {
            // Out-of-order keyframe timestamp; ignore for EMA but keep
            // the latest as the new reference.
            last_keyframe_ts_us_.store(capture_ts_us,
                                       std::memory_order_release);
            return;
        }
        const double gap_ms =
            static_cast<double>(capture_ts_us - prev) / 1000.0;
        const double alpha = cfg_.ema_alpha;

        double prev_ema = keyframe_interval_avg_ms_.load(
            std::memory_order_acquire);

        // Seed the EMA with the first observed gap so the running mean
        // reflects the actual inter-keyframe distance from the second
        // keyframe onward (rather than slowly approaching it from 0).
        double next_ema = 0.0;
        if (prev_ema == 0.0) {
            next_ema = gap_ms;
        } else {
            next_ema = alpha * gap_ms + (1.0 - alpha) * prev_ema;
        }
        // CAS-loop to update the EMA atomically (atomic<double> may
        // not provide a portable fetch_mul on all platforms, and even if
        // it does, we want atomicity of the read-modify-write).
        while (!keyframe_interval_avg_ms_.compare_exchange_weak(
                   prev_ema,
                   next_ema,
                   std::memory_order_acq_rel,
                   std::memory_order_acquire)) {
            // prev_ema was updated by another thread; recompute next_ema.
            next_ema = (prev_ema == 0.0)
                           ? gap_ms
                           : alpha * gap_ms + (1.0 - alpha) * prev_ema;
        }

        last_keyframe_ts_us_.store(capture_ts_us,
                                   std::memory_order_release);
    }

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    StatsConfig cfg_;

    // Atomic counters (relaxed is sufficient for plain increments;
    // release/acquire is used where cross-thread ordering matters).
    std::atomic<std::uint64_t> frames_received_       {0};
    std::atomic<std::uint64_t> frames_decoded_        {0};
    std::atomic<std::uint64_t> frames_rendered_       {0};
    std::atomic<std::uint64_t> frames_dropped_jb_     {0};
    std::atomic<std::uint64_t> frames_dropped_decoder_{0};
    std::atomic<std::uint64_t> keyframes_received_    {0};
    std::atomic<std::uint64_t> bytes_received_        {0};
    std::atomic<std::uint64_t> qp_sum_                {0};
    std::atomic<std::uint64_t> qp_count_              {0};
    std::atomic<std::uint64_t> freeze_count_          {0};
    std::atomic<std::uint64_t> freeze_total_ms_       {0};
    std::atomic<std::uint64_t> last_freeze_ms_        {0};

    // Timeline (sentinel -1 = not yet seen).
    std::atomic<std::int64_t>  first_frame_ts_us_     {-1};
    std::atomic<std::int64_t>  last_frame_ts_us_      {-1};
    std::atomic<std::int64_t>  last_rendered_ts_us_   {-1};
    std::atomic<std::int64_t>  last_keyframe_ts_us_   {-1};
    std::atomic<std::int64_t>  last_tick_us_          {-1};

    // Derived state (updated in-place).
    std::atomic<double>        keyframe_interval_avg_ms_ {0.0};
    std::atomic<double>        bitrate_ema_              {0.0};
    std::atomic<std::uint64_t> bitrate_samples_          {0};
    std::atomic<std::uint64_t> rendered_at_last_tick_    {0};

    // Bitrate sliding window (small critical section per push / per tick).
    mutable std::mutex         bitrate_mutex_;
    std::deque<BitrateSample>  bitrate_window_;
};

} // namespace

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
std::unique_ptr<IVideoStatsSink> create_stats_sink(StatsConfig cfg) {
    return std::make_unique<VideoStatsSink>(cfg);
}

} // namespace nimrtc::video_stats