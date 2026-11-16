// audio_device/src/aec_delay.cpp
// The AEC-delay estimator — remote-desktop plan F4.
//
// The estimator correlates the played-out signal with the captured
// signal.  Both live in fixed-size ring buffers; the correlation is
// computed over a short window at a coarse cadence, so the steady-state
// cost is bounded and allocation-free.
//
// THE SHARED TIMELINE
// -------------------
// The correlation finds the lag L such that
//
//     captured[i] == played[i - L]
//
// for every i in the correlation window.  Both signals are therefore
// indexed on ONE axis: the absolute sample index of the first sample
// of each block.  The two feeds advance that axis independently (the
// played-out stream and the captured stream have their own clocks), so
// the estimator keeps one counter per side and the *caller* supplies
// the index.
//
// The lockstep convenience overloads keep one counter per side too, so
// a caller that feeds the same number of frames to both sides ends up
// with the two streams on the same axis.
//
// THREAD SAFETY
// -------------
// The sink (the played-out side) and the source (the captured side)
// are two different devices, driven by two different threads.  The
// estimator is therefore internally synchronised: the ring buffers and
// the counters are guarded by a mutex, and the correlation itself runs
// on a *snapshot* taken under that mutex, so the expensive part never
// blocks a device thread.

#include <nimrtc/audio_device/aec_delay.hpp>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace nimrtc::audio_device {

namespace {

/** The correlation window is capped so a mis-configuration cannot make
 *  the audio thread expensive. */
constexpr std::size_t kMaxCorrelationFrames = 8192;

/** The lag search range is capped for the same reason. */
constexpr std::size_t kMaxDelayFrames = 8192;

/** The history ring is a little longer than the correlation window, so a
 *  full window is always available. */
constexpr std::size_t kHistorySlack = 64;

} // namespace

// ---------------------------------------------------------------------------
// AecDelayEstimator
// ---------------------------------------------------------------------------

AecDelayEstimator::AecDelayEstimator(AecDelayConfig cfg) noexcept {
    set_config(std::move(cfg));
}

void AecDelayEstimator::set_config(AecDelayConfig cfg) noexcept {
    std::lock_guard<std::mutex> guard(mutex_);

    cfg_ = std::move(cfg);

    if (cfg_.sample_rate_hz == 0) {
        cfg_.sample_rate_hz = 48000;
    }
    if (cfg_.correlation_frames == 0) {
        cfg_.correlation_frames = 1024;
    }
    if (cfg_.correlation_frames > kMaxCorrelationFrames) {
        cfg_.correlation_frames = kMaxCorrelationFrames;
    }
    if (cfg_.max_delay_frames == 0 ||
        cfg_.max_delay_frames > kMaxDelayFrames) {
        cfg_.max_delay_frames = kMaxDelayFrames;
    }
    if (cfg_.smoothing_window == 0) {
        cfg_.smoothing_window = 1;
    }
    if (cfg_.confidence_threshold < 0.0f) {
        cfg_.confidence_threshold = 0.0f;
    }
    if (cfg_.confidence_threshold > 1.0f) {
        cfg_.confidence_threshold = 1.0f;
    }

    // The history ring must hold the correlation window PLUS the widest
    // lag the search may reach back: the correlation compares the
    // captured window against the played window that starts
    // `max_delay_frames` earlier.  A ring that was only as long as the
    // correlation window could never find a lag at all.
    const std::size_t history =
        cfg_.correlation_frames + cfg_.max_delay_frames + kHistorySlack;

    played_.assign(history, 0.0f);
    captured_.assign(history, 0.0f);
    played_scratch_.assign(cfg_.correlation_frames, 0.0f);
    captured_scratch_.assign(cfg_.correlation_frames, 0.0f);

    history_.clear();
    history_.reserve(cfg_.smoothing_window);

    played_write_ = 0;
    played_origin_ = 0;
    played_next_ = 0;
    played_have_origin_ = false;
    captured_write_ = 0;
    captured_origin_ = 0;
    captured_next_ = 0;
    captured_have_origin_ = false;
    lockstep_play_index_ = 0;
    lockstep_capture_index_ = 0;
    frames_since_estimate_ = 0;
    confidence_ = 0.0f;
    last_raw_delay_ms_ = 0;
    have_estimate_ = false;
    correlation_runs_ = 0;
}

namespace {

/**
 * @brief Append @p count frames to a value ring.
 *
 * The ring holds `cap` entries; the oldest is overwritten when the ring
 * is full.  The absolute indices must be CONTIGUOUS: a discontinuity in
 * the caller's timeline means the correlation's reference axis is no
 * longer meaningful, so the history is dropped and the estimator starts
 * over rather than correlating two unrelated timelines.
 *
 * @return false when the timeline was discontinuous and the history was
 *         dropped.
 */
template <typename T>
bool ring_push(std::vector<T>& values,
               std::uint64_t& write_count,
               std::uint64_t& origin,
               std::uint64_t& next_index,
               bool& have_origin,
               const T* src,
               std::size_t count,
               std::uint64_t first_index) noexcept {
    const std::size_t cap = values.size();
    if (cap == 0 || count == 0) {
        return true;
    }

    bool continuous = true;
    if (!have_origin) {
        origin = first_index;
        have_origin = true;
    } else if (first_index != next_index) {
        // The caller's timeline jumped: the correlation cannot trust the
        // history any more.
        std::fill(values.begin(), values.end(), T{});
        write_count = 0;
        origin = first_index;
        continuous = false;
    }

    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t abs = first_index + i;
        values[static_cast<std::size_t>(abs % cap)] = src[i];
    }
    write_count += count;
    next_index = first_index + count;
    return continuous;
}

/**
 * @brief The absolute index of the oldest retained entry.
 *
 * The ring holds `cap` entries; once more than `cap` frames have been
 * fed, the oldest ones have been overwritten.
 */
template <typename T>
std::uint64_t ring_base(const std::vector<T>& values,
                         std::uint64_t write_count,
                         std::uint64_t origin) noexcept {
    const std::uint64_t cap = static_cast<std::uint64_t>(values.size());
    if (write_count <= cap) {
        return origin;
    }
    return origin + (write_count - cap);
}

/**
 * @brief The value of the entry whose absolute index is @p abs.
 *
 * The caller guarantees @p abs is within the retained range.
 */
template <typename T>
T ring_value_at(const std::vector<T>& values, std::uint64_t abs) noexcept {
    const std::size_t cap = values.size();
    return values[static_cast<std::size_t>(abs % cap)];
}

} // namespace

void AecDelayEstimator::on_render_played(const float* samples,
                                          std::size_t num_frames,
                                          std::uint64_t frame_index) noexcept {
    if (samples == nullptr || num_frames == 0) {
        return;
    }
    std::lock_guard<std::mutex> guard(mutex_);

    if (played_.empty()) {
        return;
    }
    // The scratch buffer grows to the largest block ever fed (a one-time
    // allocation); the steady-state path never allocates.
    if (played_scratch_.size() < num_frames) {
        played_scratch_.resize(num_frames);
    }
    const std::size_t take = std::min(num_frames, played_scratch_.size());
    for (std::size_t i = 0; i < take; ++i) {
        played_scratch_[i] = samples[i];
    }

    (void)ring_push(played_, played_write_, played_origin_, played_next_,
                    played_have_origin_, played_scratch_.data(), take,
                    frame_index);
}

void AecDelayEstimator::on_capture(const float* samples,
                                   std::size_t num_frames,
                                   std::uint64_t frame_index) noexcept {
    if (samples == nullptr || num_frames == 0) {
        return;
    }

    bool run_now = false;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (captured_.empty()) {
            return;
        }
        if (captured_scratch_.size() < num_frames) {
            captured_scratch_.resize(num_frames);
        }
        const std::size_t take =
            std::min(num_frames, captured_scratch_.size());
        for (std::size_t i = 0; i < take; ++i) {
            captured_scratch_[i] = samples[i];
        }

        const bool continuous =
            ring_push(captured_, captured_write_, captured_origin_,
                      captured_next_, captured_have_origin_,
                      captured_scratch_.data(), take, frame_index);
        if (!continuous) {
            frames_since_estimate_ = 0;
        }

        // The correlation is anchored on the CAPTURED head, so the
        // capture side drives the cadence.
        frames_since_estimate_ += take;
        if (frames_since_estimate_ >= cfg_.estimate_interval_frames) {
            frames_since_estimate_ = 0;
            run_now = true;
        }
    }

    if (run_now) {
        run_correlation();
    }
}

void AecDelayEstimator::on_render_played(const float* samples,
                                          std::size_t num_frames) noexcept {
    if (samples == nullptr || num_frames == 0) {
        return;
    }
    std::uint64_t idx = 0;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        idx = lockstep_play_index_;
        lockstep_play_index_ += num_frames;
    }
    on_render_played(samples, num_frames, idx);
}

void AecDelayEstimator::on_capture(const float* samples,
                                   std::size_t num_frames) noexcept {
    if (samples == nullptr || num_frames == 0) {
        return;
    }
    // The lockstep path keeps the two streams on the SAME axis: the
    // captured block and the played block describe the same instant.
    // The *content* of the captured block is what the microphone heard,
    // which is the played signal delayed by the loop delay -- that
    // delay is exactly what the correlation has to find.
    std::uint64_t idx = 0;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        idx = lockstep_capture_index_;
        lockstep_capture_index_ += num_frames;
    }
    on_capture(samples, num_frames, idx);
}

void AecDelayEstimator::run_correlation() noexcept {
    // ---- Take a snapshot under the lock ---------------------------------
    //
    // The correlation compares the captured window against the played
    // window that starts `lag` frames earlier.  Both are copied out
    // here so the expensive search runs *outside* the lock and never
    // blocks a device thread.
    const std::size_t window = cfg_.correlation_frames;
    const std::size_t max_lag = std::min(cfg_.max_delay_frames, window);

    std::vector<float> cap_window;
    std::vector<float> play_range;
    std::size_t effective_window = 0;
    std::size_t effective_max_lag = 0;
    std::size_t effective_min_lag = 0;

    {
        std::lock_guard<std::mutex> guard(mutex_);
        ++correlation_runs_;

        if (window == 0 || max_lag == 0 || played_.empty() || captured_.empty()) {
            return;
        }

        const std::uint64_t captured_head = captured_origin_ + captured_write_;
        const std::uint64_t captured_base =
            ring_base(captured_, captured_write_, captured_origin_);
        if (captured_head < window) {
            return;   // not enough captured history yet
        }
        const std::uint64_t captured_start = captured_head - window;
        if (captured_start < captured_base) {
            return;   // the window is not fully inside the retained history
        }

        const std::uint64_t played_head = played_origin_ + played_write_;
        const std::uint64_t played_base =
            ring_base(played_, played_write_, played_origin_);

        // A lag L is evaluable when its played window
        //   [captured_start - L, captured_start - L + window)
        // lies entirely inside the retained played history.  The search
        // range is therefore bounded on BOTH ends:
        //
        //   - the window must not reach before the oldest retained
        //     played frame:  L <= captured_start - played_base;
        //   - the window must not reach past the newest played frame:
        //     L >= captured_start + window - played_head.
        //
        // The second bound is what makes the clock-indexed monitor work:
        // the microphone captures a block 20 ms *after* the loudspeaker
        // played it, so the capture stream runs AHEAD of the play
        // stream and the played history is -- for a while -- shorter
        // than the captured one.  The lower bound is the delay the
        // estimator is looking for, and the search starts there.
        //
        // The estimator only reports an estimate when a *whole* lag
        // range is evaluable: a partial search would silently prefer
        // the lags whose windows happen to fit, which is exactly the
        // kind of confident-but-wrong answer the estimator must not
        // give.
        if (captured_start < played_base) {
            return;   // the oldest lag is out of the played history
        }

        const std::uint64_t needed_end = captured_start + window;
        const std::size_t lag_lo =
            needed_end > played_head
                ? static_cast<std::size_t>(needed_end - played_head)
                : 0u;
        const std::uint64_t reachable = captured_start - played_base;
        const std::size_t lag_hi = static_cast<std::size_t>(
            std::min<std::uint64_t>(max_lag, reachable));
        if (lag_lo > lag_hi) {
            return;   // no lag in the search range is evaluable yet
        }

        cap_window.resize(window);
        for (std::size_t i = 0; i < window; ++i) {
            cap_window[i] =
                ring_value_at(captured_, captured_start + i);
        }

        // The played range covers the widest lag's window.
        const std::uint64_t oldest_played = captured_start - lag_hi;
        const std::size_t span = window + lag_hi;
        play_range.resize(span);
        for (std::size_t i = 0; i < span; ++i) {
            play_range[i] = ring_value_at(played_, oldest_played + i);
        }

        effective_window = window;
        effective_max_lag = lag_hi;
        effective_min_lag = lag_lo;
    }

    if (effective_window == 0 || effective_max_lag == 0) {
        return;
    }

    // ---- Correlate (no lock held) ---------------------------------------

    // Precompute the captured window's energy and mean.
    double cap_mean = 0.0;
    for (std::size_t i = 0; i < effective_window; ++i) {
        cap_mean += static_cast<double>(cap_window[i]);
    }
    cap_mean /= static_cast<double>(effective_window);

    double cap_energy = 0.0;
    for (std::size_t i = 0; i < effective_window; ++i) {
        const double v = static_cast<double>(cap_window[i]) - cap_mean;
        cap_energy += v * v;
    }
    if (cap_energy <= 1e-12) {
        // The captured signal is silence: there is nothing to correlate.
        std::lock_guard<std::mutex> guard(mutex_);
        confidence_ = 0.0f;
        return;
    }

    double best_corr = 0.0;
    std::size_t best_lag = 0;
    bool found = false;

    for (std::size_t lag = effective_min_lag; lag <= effective_max_lag; ++lag) {
        // The played window for lag L starts at `oldest_played + L`
        // (which is `captured_start - (lag_hi - L)`).
        const std::size_t offset = effective_max_lag - lag;

        double p_mean = 0.0;
        for (std::size_t i = 0; i < effective_window; ++i) {
            p_mean += static_cast<double>(play_range[offset + i]);
        }
        p_mean /= static_cast<double>(effective_window);

        double num = 0.0;
        double p_energy = 0.0;
        for (std::size_t i = 0; i < effective_window; ++i) {
            const double p = static_cast<double>(play_range[offset + i]) - p_mean;
            const double c = static_cast<double>(cap_window[i]) - cap_mean;
            num += p * c;
            p_energy += p * p;
        }
        if (p_energy <= 1e-12) {
            continue;
        }
        const double corr = num / std::sqrt(p_energy * cap_energy);
        if (corr > best_corr) {
            best_corr = corr;
            best_lag = lag;
            found = true;
        }
    }

    std::lock_guard<std::mutex> guard(mutex_);

    if (!found) {
        confidence_ = 0.0f;
        have_estimate_ = false;
        return;
    }

    confidence_ = static_cast<float>(std::max(0.0, std::min(1.0, best_corr)));

    if (confidence_ < cfg_.confidence_threshold) {
        // Too weak to trust: keep the previous estimate rather than
        // moving the AEC delay on a coincidence.
        return;
    }

    const std::int32_t delay_ms = static_cast<std::int32_t>(
        (static_cast<double>(best_lag) * 1000.0) /
        static_cast<double>(cfg_.sample_rate_hz));
    last_raw_delay_ms_ = delay_ms;
    have_estimate_ = true;
    push_estimate(delay_ms);
}

void AecDelayEstimator::push_estimate(std::int32_t delay_ms) noexcept {
    history_.push_back(delay_ms);
    while (history_.size() > cfg_.smoothing_window) {
        history_.erase(history_.begin());
    }
}

std::optional<std::int32_t>
AecDelayEstimator::smoothed_delay_ms() const noexcept {
    if (history_.empty()) {
        return std::nullopt;
    }
    std::vector<std::int32_t> sorted(history_);
    std::sort(sorted.begin(), sorted.end());
    const std::size_t mid = sorted.size() / 2;
    if (sorted.size() % 2 == 1) {
        return sorted[mid];
    }
    // An even count: the mean of the two middle values.
    return (sorted[mid - 1] + sorted[mid]) / 2;
}

std::optional<std::int32_t> AecDelayEstimator::delay_ms() const noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    if (cfg_.configured_delay_ms.has_value()) {
        return cfg_.configured_delay_ms;
    }
    return smoothed_delay_ms();
}

float AecDelayEstimator::confidence() const noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    if (cfg_.configured_delay_ms.has_value()) {
        // A configured delay is trusted by definition.
        return 1.0f;
    }
    return confidence_;
}

std::optional<std::int32_t>
AecDelayEstimator::aec_delay_headroom_ms() const noexcept {
    // NOTE: `delay_ms()` takes the lock itself, so it must NOT be called
    // while the lock is already held (std::mutex is not recursive).
    const std::optional<std::int32_t> measured = delay_ms();
    if (!measured.has_value()) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    return *measured + cfg_.headroom_ms;
}

bool AecDelayEstimator::uses_configured_delay() const noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    return cfg_.configured_delay_ms.has_value();
}

void AecDelayEstimator::reset() noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    std::fill(played_.begin(), played_.end(), 0.0f);
    std::fill(captured_.begin(), captured_.end(), 0.0f);
    played_write_ = 0;
    played_origin_ = 0;
    played_next_ = 0;
    played_have_origin_ = false;
    captured_write_ = 0;
    captured_origin_ = 0;
    captured_next_ = 0;
    captured_have_origin_ = false;
    lockstep_play_index_ = 0;
    lockstep_capture_index_ = 0;
    frames_since_estimate_ = 0;
    history_.clear();
    confidence_ = 0.0f;
    last_raw_delay_ms_ = 0;
    have_estimate_ = false;
    correlation_runs_ = 0;
}

// ---------------------------------------------------------------------------
// AecDelayMonitor
// ---------------------------------------------------------------------------

AecDelayMonitor::AecDelayMonitor(AecDelayConfig cfg) noexcept
    : estimator_(std::move(cfg)) {}

std::uint64_t AecDelayMonitor::index_at(std::int64_t now_us) noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!have_origin_) {
        origin_us_ = now_us;
        have_origin_ = true;
    }
    const std::int64_t delta = now_us - origin_us_;
    if (delta <= 0) {
        return 0;
    }
    const std::int64_t rate =
        static_cast<std::int64_t>(estimator_.config().sample_rate_hz);
    return static_cast<std::uint64_t>((delta * rate) / 1000000);
}

void AecDelayMonitor::on_render_played(const float* samples,
                                       std::size_t num_frames,
                                       std::int64_t now_us) noexcept {
    if (samples == nullptr || num_frames == 0) {
        return;
    }
    estimator_.on_render_played(samples, num_frames, index_at(now_us));
}

void AecDelayMonitor::on_capture(const float* samples,
                                 std::size_t num_frames,
                                 std::int64_t now_us) noexcept {
    if (samples == nullptr || num_frames == 0) {
        return;
    }
    estimator_.on_capture(samples, num_frames, index_at(now_us));
}

std::optional<std::int32_t> AecDelayMonitor::delay_ms() const noexcept {
    return estimator_.delay_ms();
}

std::optional<std::int32_t>
AecDelayMonitor::aec_delay_headroom_ms() const noexcept {
    return estimator_.aec_delay_headroom_ms();
}

float AecDelayMonitor::confidence() const noexcept {
    return estimator_.confidence();
}

void AecDelayMonitor::reset() noexcept {
    estimator_.reset();
    std::lock_guard<std::mutex> guard(mutex_);
    have_origin_ = false;
    origin_us_ = 0;
}

} // namespace nimrtc::audio_device
