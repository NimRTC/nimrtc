// modules/video_capture/src/unix_capture_core.cpp
// ----------------------------------------------------------------------------
// The portable capture core shared by the macOS (A4) and Linux (A5)
// backends.
//
// Everything here is platform-neutral: no OS header is included, no OS
// call is made.  That is deliberate -- it means the interesting half of
// the capture mechanism (the pixel-diff, the crop, the negotiation, the
// drop policy) is unit-tested on every CI leg, including the Windows
// leg where the macOS and Linux adapters cannot even be compiled.
//
// See include/nimrtc/video_capture/unix_capture_core.hpp for the
// contract, and include/nimrtc/video_capture/unix_capture.hpp for the
// device base class that drives this core.
// ----------------------------------------------------------------------------

#include <algorithm>
#include <chrono>
#include <cstring>
#include <nimrtc/video_capture/unix_capture_core.hpp>

namespace nimrtc::video_capture {

namespace {

std::int64_t now_us() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/** A half-open horizontal interval [x0, x1) of changed pixels. */
struct Interval {
    std::uint32_t x0 = 0;
    std::uint32_t x1 = 0;
};

/** The horizontal gap between two intervals (0 when they touch or
 *  overlap). */
std::uint32_t horizontal_gap(const Interval& a, const Interval& b) noexcept {
    if (a.x1 <= b.x0)
        return b.x0 - a.x1;
    if (b.x1 <= a.x0)
        return a.x0 - b.x1;
    return 0;
}

/** Merge the intervals of a single row, joining the ones that are no
 *  further apart than @p threshold. */
std::vector<Interval> merge_intervals(std::vector<Interval>&& runs,
                                      std::uint32_t threshold) noexcept {
    if (runs.size() < 2)
        return runs;
    std::sort(runs.begin(), runs.end(),
              [](const Interval& a, const Interval& b) { return a.x0 < b.x0; });
    std::vector<Interval> out;
    out.reserve(runs.size());
    Interval cur = runs.front();
    for (std::size_t i = 1; i < runs.size(); ++i) {
        const Interval& next = runs[i];
        if (next.x0 <= cur.x1 + threshold) {
            cur.x1 = std::max(cur.x1, next.x1);
        } else {
            out.push_back(cur);
            cur = next;
        }
    }
    out.push_back(cur);
    return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// The portable pixel-diff (plan A7)
// ---------------------------------------------------------------------------

plugins::CaptureDirtyRects
compute_pixel_dirty_rects(const std::uint8_t* previous, const std::uint8_t* current,
                          std::uint32_t width, std::uint32_t height,
                          const UnixCaptureDiffOptions& options) noexcept {
    plugins::CaptureDirtyRects out;

    // The honest fallbacks: no diff, no previous frame, or a degenerate
    // frame -- "the whole frame changed".  An *empty* rectangle list
    // would claim "nothing changed", which is a claim the encoder acts
    // on, so it is only ever produced by an actual comparison that found
    // no difference.
    if (!options.enabled || previous == nullptr || current == nullptr || width == 0 ||
        height == 0) {
        out.full_frame = true;
        return out;
    }

    const std::uint32_t bpp = (options.bytes_per_pixel == 0) ? 4u : options.bytes_per_pixel;
    const std::uint32_t threshold = options.merge_threshold;

    // The row strides.  A stride of 0 means "packed"; a stride that is
    // *narrower* than the packed row is nonsense, so it is treated as
    // packed too (the comparison would otherwise read out of bounds).
    const std::size_t packed = static_cast<std::size_t>(width) * bpp;
    const std::int32_t cur_stride_opt = options.stride;
    const std::int32_t prev_stride_opt = options.previous_stride;
    const std::size_t cur_stride =
        (cur_stride_opt > 0 && static_cast<std::size_t>(cur_stride_opt) >= packed)
            ? static_cast<std::size_t>(cur_stride_opt)
            : packed;
    const std::size_t prev_stride =
        (prev_stride_opt > 0 && static_cast<std::size_t>(prev_stride_opt) >= packed)
            ? static_cast<std::size_t>(prev_stride_opt)
            : packed;

    // ---- Per row: the changed pixel runs ---------------------------------
    std::vector<std::vector<Interval>> rows(height);
    bool any_change = false;
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t* prev_row = previous + static_cast<std::size_t>(y) * prev_stride;
        const std::uint8_t* cur_row = current + static_cast<std::size_t>(y) * cur_stride;

        std::vector<Interval> runs;
        std::uint32_t run_start = 0;
        bool in_run = false;
        for (std::uint32_t x = 0; x < width; ++x) {
            const bool differs = std::memcmp(prev_row + static_cast<std::size_t>(x) * bpp,
                                             cur_row + static_cast<std::size_t>(x) * bpp, bpp) != 0;
            if (differs && !in_run) {
                run_start = x;
                in_run = true;
            } else if (!differs && in_run) {
                runs.push_back(Interval{run_start, x});
                in_run = false;
            }
        }
        if (in_run)
            runs.push_back(Interval{run_start, width});

        if (!runs.empty()) {
            any_change = true;
            rows[y] = merge_intervals(std::move(runs), threshold);
        }
    }

    if (!any_change) {
        // Nothing changed: an empty rectangle list with `full_frame ==
        // false`.  This is the property that makes a static desktop
        // nearly free to send.
        out.full_frame = false;
        out.rects.clear();
        return out;
    }

    // ---- Across rows: merge the runs that stack up -----------------------
    //
    // A rectangle is grown downwards while the next row has a run that is
    // close enough horizontally.  The result is a set of rectangles that
    // *cover* every changed pixel and may cover a little more -- the
    // conservative direction, which is the safe one.
    struct OpenRect {
        std::uint32_t x0 = 0;
        std::uint32_t x1 = 0;
        std::uint32_t y0 = 0;
        std::uint32_t last_row = 0;
        bool used_this_row = false;
    };

    std::vector<OpenRect> open;
    std::vector<plugins::CaptureRegion> rects;

    for (std::uint32_t y = 0; y < height; ++y) {
        for (const Interval& iv : rows[y]) {
            // Find an open rectangle that this run can extend.
            OpenRect* target = nullptr;
            std::uint32_t best_gap = 0;
            for (OpenRect& r : open) {
                if (r.used_this_row)
                    continue;
                if (r.last_row + 1 != y)
                    continue; // must be adjacent
                const Interval rect_span{r.x0, r.x1};
                const std::uint32_t gap = horizontal_gap(rect_span, iv);
                if (gap > threshold)
                    continue;
                if (target == nullptr || gap < best_gap) {
                    target = &r;
                    best_gap = gap;
                }
            }

            if (target != nullptr) {
                target->x0 = std::min(target->x0, iv.x0);
                target->x1 = std::max(target->x1, iv.x1);
                target->last_row = y;
                target->used_this_row = true;
            } else {
                OpenRect fresh;
                fresh.x0 = iv.x0;
                fresh.x1 = iv.x1;
                fresh.y0 = y;
                fresh.last_row = y;
                fresh.used_this_row = true;
                open.push_back(fresh);
            }
        }

        // Close the rectangles that did not take part in this row.
        for (std::size_t i = open.size(); i > 0; --i) {
            OpenRect& r = open[i - 1];
            if (!r.used_this_row) {
                plugins::CaptureRegion region;
                region.x = r.x0;
                region.y = r.y0;
                region.width = r.x1 - r.x0;
                region.height = (r.last_row + 1) - r.y0;
                if (region.width > 0 && region.height > 0) {
                    rects.push_back(region);
                }
                open.erase(open.begin() + static_cast<std::ptrdiff_t>(i - 1));
            } else {
                r.used_this_row = false;
            }
        }
    }

    // Whatever is still open ends on the last row.
    for (const OpenRect& r : open) {
        plugins::CaptureRegion region;
        region.x = r.x0;
        region.y = r.y0;
        region.width = r.x1 - r.x0;
        region.height = (r.last_row + 1) - r.y0;
        if (region.width > 0 && region.height > 0) {
            rects.push_back(region);
        }
    }

    if (rects.empty()) {
        // The diff ran and found changes, but they all collapsed away --
        // which cannot happen, so treat it conservatively.
        out.full_frame = true;
        return out;
    }

    out.full_frame = false;
    out.rects = std::move(rects);
    return out;
}

// ---------------------------------------------------------------------------
// The region crop (plan A8)
// ---------------------------------------------------------------------------

plugins::CaptureRegion clamp_region(const plugins::CaptureRegion& region, std::uint32_t src_width,
                                    std::uint32_t src_height) noexcept {
    plugins::CaptureRegion out = region;

    // A zero-sized region means "the whole surface".
    if (out.width == 0 || out.height == 0)
        return out;

    if (out.x >= src_width || out.y >= src_height) {
        out.width = 0;
        out.height = 0;
        return out;
    }

    if (out.x + out.width > src_width)
        out.width = src_width - out.x;
    if (out.y + out.height > src_height)
        out.height = src_height - out.y;
    return out;
}

bool crop_frame_bgra(const std::uint8_t* src, std::size_t src_size, std::uint32_t src_width,
                     std::uint32_t src_height, std::int32_t src_stride,
                     const plugins::CaptureRegion& region, std::vector<std::uint8_t>& out,
                     std::uint32_t& out_width, std::uint32_t& out_height,
                     std::int32_t& out_stride) noexcept {
    out_width = 0;
    out_height = 0;
    out_stride = 0;

    if (src == nullptr || src_width == 0 || src_height == 0)
        return false;

    const std::int32_t stride =
        (src_stride > 0) ? src_stride : static_cast<std::int32_t>(src_width * 4u);
    const std::size_t needed = static_cast<std::size_t>(stride) * src_height;
    if (src_size < needed)
        return false;

    const plugins::CaptureRegion clamped = clamp_region(region, src_width, src_height);

    // No region: the frame passes through unchanged.
    if (clamped.width == 0 || clamped.height == 0) {
        out_width = src_width;
        out_height = src_height;
        out_stride = stride;
        out.assign(src, src + needed);
        return true;
    }

    const std::uint32_t dst_stride = clamped.width * 4u;
    out.resize(static_cast<std::size_t>(dst_stride) * clamped.height);

    for (std::uint32_t y = 0; y < clamped.height; ++y) {
        const std::uint8_t* src_row = src + static_cast<std::size_t>(clamped.y + y) * stride +
                                      static_cast<std::size_t>(clamped.x) * 4u;
        std::uint8_t* dst_row = out.data() + static_cast<std::size_t>(y) * dst_stride;
        std::memcpy(dst_row, src_row, dst_stride);
    }

    out_width = clamped.width;
    out_height = clamped.height;
    out_stride = static_cast<std::int32_t>(dst_stride);
    return true;
}

// ---------------------------------------------------------------------------
// Format negotiation
// ---------------------------------------------------------------------------

plugins::CaptureFormat negotiate_capture_format(const plugins::CaptureDevice& device,
                                                const plugins::CaptureFormat& requested,
                                                const std::string& default_tag,
                                                bool zero_copy_capable,
                                                bool dirty_rects_capable) noexcept {
    plugins::CaptureFormat out = requested;

    // ---- The size --------------------------------------------------------
    //
    // Never upscale: the device's own maximum is the ceiling.  A request
    // that is larger is clamped down (rather than refused) and the caller
    // reads the result back from `config()`.
    const std::uint32_t device_w = (device.max_width == 0) ? requested.width : device.max_width;
    const std::uint32_t device_h = (device.max_height == 0) ? requested.height : device.max_height;

    if (requested.width == 0 || requested.height == 0) {
        // "The device's best".
        out.width = device_w;
        out.height = device_h;
    } else {
        out.width = std::min(requested.width, device_w);
        out.height = std::min(requested.height, device_h);
    }
    if (out.width == 0)
        out.width = device_w;
    if (out.height == 0)
        out.height = device_h;

    // ---- The frame rate --------------------------------------------------
    if (out.fps == 0)
        out.fps = (device.max_fps != 0) ? device.max_fps : 30u;

    // ---- The tag ---------------------------------------------------------
    if (out.tag.empty())
        out.tag = default_tag;

    // ---- The honest flags ------------------------------------------------
    //
    // These report what the backend can *deliver*, not what the caller
    // asked for.  A caller that asked for a zero-copy format on a
    // backend that cannot do it gets `false` here, and the engine can
    // see that the fast path is not available.
    out.zero_copy = zero_copy_capable;
    out.has_dirty_rects = dirty_rects_capable;

    return out;
}

// ---------------------------------------------------------------------------
// UnixCaptureCore
// ---------------------------------------------------------------------------

UnixCaptureCore::UnixCaptureCore(plugins::CaptureConfig cfg, UnixCaptureOptions opts) noexcept
    : cfg_(std::move(cfg)), opts_(opts) {
    queue_.set_drop_policy(cfg_.drop_policy);
    if (cfg_.queue_depth > 0)
        queue_.set_depth(cfg_.queue_depth);
}

UnixCaptureCore::~UnixCaptureCore() = default;

void UnixCaptureCore::set_config(const plugins::CaptureConfig& cfg) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    cfg_ = cfg;
    queue_.set_drop_policy(cfg_.drop_policy);
    if (cfg_.queue_depth > 0)
        queue_.set_depth(cfg_.queue_depth);
}

plugins::CaptureConfig UnixCaptureCore::config() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    plugins::CaptureConfig out = cfg_;
    if (negotiated_.width != 0 && negotiated_.height != 0) {
        out.format.width = negotiated_.width;
        out.format.height = negotiated_.height;
        out.format.fps = negotiated_.fps;
        out.format.tag = negotiated_.tag;
        out.format.zero_copy = negotiated_.zero_copy;
        out.format.has_dirty_rects = negotiated_.has_dirty_rects;
    }
    if (out.format.fps == 0)
        out.format.fps = opts_.default_fps;
    return out;
}

UnixCaptureOptions UnixCaptureCore::options() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return opts_;
}

void UnixCaptureCore::set_options(const UnixCaptureOptions& opts) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    opts_ = opts;
}

plugins::CaptureRegion UnixCaptureCore::region() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return cfg_.region;
}

void UnixCaptureCore::set_region(const plugins::CaptureRegion& region) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    cfg_.region = region;
}

plugins::CaptureCapabilities UnixCaptureCore::capabilities() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return caps_;
}

void UnixCaptureCore::set_capabilities(const plugins::CaptureCapabilities& caps) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    caps_ = caps;
}

void UnixCaptureCore::set_negotiated_format(const plugins::CaptureFormat& format) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    negotiated_ = format;
}

plugins::CaptureFormat UnixCaptureCore::negotiated_format() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return negotiated_;
}

plugins::CaptureFormat
UnixCaptureCore::negotiate_format(const plugins::CaptureDevice& device,
                                  const plugins::CaptureFormat& requested) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool zero_copy = caps_.zero_copy && opts_.allow_zero_copy;
    const bool dirty_rects = caps_.dirty_rects && opts_.enable_pixel_diff;
    const std::string tag = caps_.backend.empty() ? "bgra" : caps_.backend;
    return negotiate_capture_format(device, requested, tag, zero_copy, dirty_rects);
}

plugins::Status UnixCaptureCore::submit_frame(const std::uint8_t* pixels, std::size_t size,
                                              std::int32_t stride, std::uint32_t width,
                                              std::uint32_t height, std::int64_t ts,
                                              const plugins::CaptureDirtyRects* dirty) noexcept {
    return push_common(pixels, size, stride, width, height, ts, plugins::GpuBackend::kNone, nullptr,
                       0, dirty, nullptr);
}

plugins::Status UnixCaptureCore::submit_gpu_frame(const std::uint8_t* pixels, std::size_t size,
                                                  std::int32_t stride, std::uint32_t width,
                                                  std::uint32_t height, std::int64_t ts,
                                                  plugins::GpuBackend backend, void* handle,
                                                  std::uint32_t pitch,
                                                  const plugins::CaptureDirtyRects* dirty,
                                                  std::shared_ptr<void> owner) noexcept {
    return push_common(pixels, size, stride, width, height, ts, backend, handle, pitch, dirty,
                       std::move(owner));
}

plugins::Status UnixCaptureCore::push_common(const std::uint8_t* pixels, std::size_t size,
                                             std::int32_t stride, std::uint32_t width,
                                             std::uint32_t height, std::int64_t ts,
                                             plugins::GpuBackend backend, void* handle,
                                             std::uint32_t pitch,
                                             const plugins::CaptureDirtyRects* dirty,
                                             std::shared_ptr<void> owner) noexcept {
    if (width == 0 || height == 0)
        return plugins::kErrInvalidParam;

    std::unique_lock<std::mutex> lock(mutex_);

    // ---- The crop --------------------------------------------------------
    const plugins::CaptureRegion requested_region = cfg_.region;
    const bool wants_crop = (requested_region.width != 0 && requested_region.height != 0);

    const std::uint8_t* frame_pixels = pixels;
    std::size_t frame_size = size;
    std::int32_t frame_stride = stride;
    std::uint32_t frame_width = width;
    std::uint32_t frame_height = height;

    if (wants_crop && pixels != nullptr) {
        std::uint32_t cw = 0;
        std::uint32_t ch = 0;
        std::int32_t cs = 0;
        if (!crop_frame_bgra(pixels, size, width, height, stride, requested_region, cropped_, cw,
                             ch, cs)) {
            return plugins::kErrBufferTooSmall;
        }
        // The crop is a real copy, so it is counted: the statistics must
        // show that the frame was not passed through untouched.
        stats_.bytes_copied += static_cast<std::uint64_t>(cropped_.size());
        frame_pixels = cropped_.data();
        frame_size = cropped_.size();
        frame_stride = cs;
        frame_width = cw;
        frame_height = ch;
    }

    // ---- The dirty rectangles -------------------------------------------
    plugins::CaptureDirtyRects rects;
    if (dirty != nullptr) {
        rects = *dirty;
    } else if (!opts_.enable_pixel_diff) {
        // The diff is disabled: the honest answer is "the whole frame
        // changed", never "nothing changed".
        rects.full_frame = true;
    } else {
        UnixCaptureDiffOptions diff_opts;
        diff_opts.enabled = true;
        diff_opts.merge_threshold = opts_.diff_merge_threshold;
        diff_opts.bytes_per_pixel = 4;
        diff_opts.stride = frame_stride;
        diff_opts.previous_stride = have_prev_ ? prev_stride_ : 0;

        const std::uint8_t* prev = have_prev_ ? prev_pixels_.data() : nullptr;
        rects = compute_pixel_dirty_rects(prev, frame_pixels, frame_width, frame_height, diff_opts);

        // Remember this frame for the next comparison.
        if (frame_pixels != nullptr && frame_size > 0) {
            prev_pixels_.assign(frame_pixels, frame_pixels + frame_size);
            prev_width_ = frame_width;
            prev_height_ = frame_height;
            prev_stride_ = frame_stride;
            have_prev_ = true;
        }
    }

    // ---- The queue -------------------------------------------------------
    const std::uint32_t seq = next_seq_++;
    const auto outcome = queue_.push(frame_pixels, frame_size, frame_stride, frame_width,
                                     frame_height, ts, seq, rects);
    switch (outcome) {
    case CaptureQueue::PushResult::kQueued:
        ++stats_.frames_captured;
        break;
    case CaptureQueue::PushResult::kDroppedOldest:
        ++stats_.frames_captured;
        ++stats_.frames_dropped;
        break;
    case CaptureQueue::PushResult::kRejected:
        ++stats_.frames_dropped;
        break;
    }

    if (backend != plugins::GpuBackend::kNone && handle != nullptr) {
        // The zero-copy path: the surface is handed over as is, so the
        // pixels never enter host memory.  The handle travels beside
        // the queued frame and is released when the frame is popped.
        ++stats_.frames_zero_copy;
        GpuSubmission submission;
        submission.seq = seq;
        submission.backend = backend;
        submission.handle = handle;
        submission.pitch = pitch;
        submission.owner = std::move(owner);
        gpu_pending_[seq] = std::move(submission);
    } else {
        ++stats_.frames_cpu_staged;
    }

    // The callback is invoked with the core's lock released, so a
    // callback that calls back into the core (a statistics query, say)
    // cannot deadlock.
    lock.unlock();
    fire_callback();
    return plugins::kOk;
}

plugins::Status UnixCaptureCore::pop_frame(plugins::CaptureFrame& out) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!queue_.pop(out))
        return plugins::kErrNotReady;
    out.format = plugins::VideoPixelFormat::kBGRA;

    // The zero-copy handle travels beside the queued frame: it is
    // looked up by the frame's sequence number and stays alive until
    // the *next* pop, exactly as the `CaptureFrame` contract promises.
    //
    // The sequence numbers are monotonic and `pop()` returns the
    // *oldest* queued frame, so every pending entry below the popped
    // sequence number belongs to a frame that was already handed out
    // or dropped -- those are released here.
    for (auto it = gpu_pending_.begin(); it != gpu_pending_.end() && it->first < out.frame_seq;) {
        it = gpu_pending_.erase(it);
    }

    auto it = gpu_pending_.find(out.frame_seq);
    if (it != gpu_pending_.end()) {
        last_gpu_ = std::move(it->second);
        gpu_pending_.erase(it);
        out.gpu_backend = last_gpu_.backend;
        out.gpu_handle = last_gpu_.handle;
        out.has_gpu_handle = (last_gpu_.handle != nullptr);
        out.gpu_pitch_bytes = last_gpu_.pitch;
    } else {
        out.gpu_backend = plugins::GpuBackend::kNone;
        out.gpu_handle = nullptr;
        out.has_gpu_handle = false;
        out.gpu_pitch_bytes = 0;
    }
    return plugins::kOk;
}

plugins::CaptureStats UnixCaptureCore::stats() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    // The drop counter is maintained by `push_common()` in step with the
    // queue's own push result, so the two cannot disagree.
    return stats_;
}

void UnixCaptureCore::reset_stats() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = plugins::CaptureStats{};
}

void UnixCaptureCore::note_device_error() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.device_errors;
}

void UnixCaptureCore::add_bytes_copied(std::uint64_t bytes) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.bytes_copied += bytes;
}

void UnixCaptureCore::note_device_restart() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.device_restarts;
}

void UnixCaptureCore::note_format_change() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.format_changes;
}

void UnixCaptureCore::set_frame_callback(plugins::CaptureFrameCallback cb) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    cb_ = std::move(cb);
}

void UnixCaptureCore::set_drop_policy(plugins::CaptureDropPolicy policy) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    cfg_.drop_policy = policy;
    queue_.set_drop_policy(policy);
}

void UnixCaptureCore::set_queue_depth(std::uint32_t depth) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    cfg_.queue_depth = depth;
    queue_.set_depth(depth);
}

bool UnixCaptureCore::pixel_diff_enabled() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return opts_.enable_pixel_diff;
}

void UnixCaptureCore::set_pixel_diff_enabled(bool enabled) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    opts_.enable_pixel_diff = enabled;
}

std::uint32_t UnixCaptureCore::merge_threshold() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return opts_.diff_merge_threshold;
}

void UnixCaptureCore::set_merge_threshold(std::uint32_t threshold) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    opts_.diff_merge_threshold = threshold;
}

void UnixCaptureCore::fire_callback() noexcept {
    plugins::CaptureFrameCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!cb_)
            return;
        cb = cb_;
    }
    plugins::CaptureFrame view;
    if (!queue_.peek(view))
        return; // the frame was dropped by policy
    view.format = plugins::VideoPixelFormat::kBGRA;

    // The zero-copy handle travels with the frame, so the callback sees
    // the same surface the consumer will later pop.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = gpu_pending_.find(view.frame_seq);
        if (it != gpu_pending_.end()) {
            view.gpu_backend = it->second.backend;
            view.gpu_handle = it->second.handle;
            view.has_gpu_handle = (it->second.handle != nullptr);
            view.gpu_pitch_bytes = it->second.pitch;
        }
    }
    cb(view);
}

} // namespace nimrtc::video_capture
