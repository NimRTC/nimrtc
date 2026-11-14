/**
 * @file nimrtc/video_sink/video_sink.hpp
 * @brief Headless / file / debug video sinks.
 *
 * Per ARCHITECTURE.md §6, the receiver pipeline needs somewhere to deliver
 * decoded frames. This module provides:
 *
 *   - **HeadlessSink**: counts frames, drops pixels. Useful for tests and
 *     for benchmarks where rendering overhead would distort measurements.
 *   - **PinnedSink**:   headless sink that asserts every frame's `capture_ts_us`
 *     is strictly increasing (catches timestamp mishandling).
 *
 * Both implement the plugin `plugins::IVideoSink` interface so they can
 * be registered with `core::PluginRegistry` under the ids `"headless"`
 * and `"pinned"`. Real renderer plugins (SDL, Vulkan, OpenGL, ...) would
 * ship in separate modules implementing the same interface.
 *
 * @note P1 (R2-Batch2). Headless / pinned sinks only.
 */

#pragma once


// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>

#include <nimrtc/core/error.hpp>
#include <nimrtc/plugins/video_sink.hpp>

namespace nimrtc::video_sink {

// ---------------------------------------------------------------------------
// HeadlessSink — counts frames, drops pixels
// ---------------------------------------------------------------------------

/** Headless sink: counts frames, drops pixel data. Useful for tests,
 *  benchmarks, and CI where rendering overhead is undesirable.
 *
 *  Thread safety: the counters are atomic, so render() may run on the
 *  producer thread while stats() is read from another one.  The
 *  *ordering* of the rendered frames is still the caller's
 *  responsibility — the sink does not serialize concurrent renders. */
class NIMRTC_API_STABLE HeadlessSink : public plugins::IVideoSink {
public:
    explicit HeadlessSink(plugins::VideoSinkConfig cfg) noexcept;
    ~HeadlessSink() override;

    HeadlessSink(const HeadlessSink&)            = delete;
    HeadlessSink& operator=(const HeadlessSink&) = delete;

    // ---- plugins::IPlugin --------------------------------------------------
    const char*     name()  const noexcept override;
    plugins::Status open()  noexcept override;
    void            close() noexcept override;

    // ---- plugins::IVideoSink -----------------------------------------------
    plugins::Status render(const plugins::VideoSourceFrame& frame) noexcept override;
    plugins::Status render(const plugins::GpuBuffer& surface) noexcept override;
    plugins::Status render(const plugins::GpuBuffer& surface,
                           const plugins::VideoFrameInfo& info) noexcept override;
    plugins::VideoSinkStats stats() const noexcept override;
    plugins::VideoSinkConfig config() const noexcept override;
    plugins::Status set_config(plugins::VideoSinkConfig cfg) noexcept override;

    // ---- Capability probes ------------------------------------------------
    //
    // The headless sink drops the pixels either way, so it accepts both
    // delivery paths.  Saying so is what lets the engine take the GPU
    // path when the decoder can produce a surface.
    bool supports_gpu_render() const noexcept override { return true; }
    bool supports_cpu_render() const noexcept override { return true; }

private:
    plugins::VideoSinkConfig cfg_;
    // The counters live in atomics: render() runs on the producer
    // thread while stats() is read from the engine thread.  A plain
    // `VideoSinkStats` member was a data race (TSan: write of size 8
    // in HeadlessSink::render() vs. read in stats()).
    std::atomic<std::uint64_t> frames_rendered_{0};
    std::atomic<bool>          opened_{false};
};

/** PinnedSink — headless sink that asserts every frame's `capture_ts_us`
 *  is strictly increasing. Catches timestamp mishandling in tests. */
class NIMRTC_API_STABLE PinnedSink : public plugins::IVideoSink {
public:
    explicit PinnedSink(plugins::VideoSinkConfig cfg) noexcept;
    ~PinnedSink() override;

    PinnedSink(const PinnedSink&)            = delete;
    PinnedSink& operator=(const PinnedSink&) = delete;

    // ---- plugins::IPlugin --------------------------------------------------
    const char*     name()  const noexcept override;
    plugins::Status open()  noexcept override;
    void            close() noexcept override;

    // ---- plugins::IVideoSink -----------------------------------------------
    plugins::Status render(const plugins::VideoSourceFrame& frame) noexcept override;
    plugins::Status render(const plugins::GpuBuffer& surface) noexcept override;
    plugins::Status render(const plugins::GpuBuffer& surface,
                           const plugins::VideoFrameInfo& info) noexcept override;
    plugins::VideoSinkStats stats() const noexcept override;
    plugins::VideoSinkConfig config() const noexcept override;
    plugins::Status set_config(plugins::VideoSinkConfig cfg) noexcept override;

    // ---- Capability probes ------------------------------------------------
    bool supports_gpu_render() const noexcept override { return true; }
    bool supports_cpu_render() const noexcept override { return true; }

private:
    plugins::VideoSinkConfig cfg_;
    // Atomic counters — see HeadlessSink's note on the data race.
    std::atomic<std::uint64_t> frames_rendered_{0};
    std::atomic<std::uint64_t> render_errors_{0};
    std::atomic<bool>          opened_{false};
    // The capture timestamp of the last rendered frame.  Only touched
    // by render(), which the caller serializes.
    plugins::TimestampUs     last_capture_ts_us_  = -1;
};

// ---------------------------------------------------------------------------
// Factory (concrete-side)
// ---------------------------------------------------------------------------

/** Build a headless sink with the given config. */
NIMRTC_API_STABLE std::unique_ptr<plugins::IVideoSink> create_headless_sink(
    plugins::VideoSinkConfig cfg) noexcept;

/** Build a pinned sink (headless + capture_ts ordering check) with the
 *  given config. */
NIMRTC_API_STABLE std::unique_ptr<plugins::IVideoSink> create_pinned_sink(
    plugins::VideoSinkConfig cfg) noexcept;

// ---------------------------------------------------------------------------
// PluginAdapter + factories
// ---------------------------------------------------------------------------

/** Adapter that registers as a plugins::IVideoSink. Wraps an owned
 *  concrete IVideoSink so the plugin lookup is uniform with non-headless
 *  sinks (which will plug in their own adapter).
 *
 *  Owned by the registry factory — single use; consumer calls `delete`
 *  (or wraps in unique_ptr) after no longer needed. */
class NIMRTC_API_STABLE PluginAdapter : public plugins::IVideoSink {
public:
    explicit PluginAdapter(std::unique_ptr<plugins::IVideoSink> concrete) noexcept;
    ~PluginAdapter() override;

    PluginAdapter(const PluginAdapter&)            = delete;
    PluginAdapter& operator=(const PluginAdapter&) = delete;

    // ---- plugins::IPlugin --------------------------------------------------
    const char*     name()  const noexcept override;
    plugins::Status open()  noexcept override;
    void            close() noexcept override;

    // ---- plugins::IVideoSink -----------------------------------------------
    plugins::Status render(const plugins::VideoSourceFrame& frame) noexcept override;
    plugins::Status render(const plugins::GpuBuffer& surface) noexcept override;
    plugins::Status render(const plugins::GpuBuffer& surface,
                           const plugins::VideoFrameInfo& info) noexcept override;
    plugins::VideoSinkStats stats() const noexcept override;
    plugins::VideoSinkConfig config() const noexcept override;
    plugins::Status set_config(plugins::VideoSinkConfig cfg) noexcept override;

    // ---- Capability probes ------------------------------------------------
    //
    // The adapter forwards the probes to the sink it wraps: the adapter
    // is what the engine sees, so a GPU-only concrete sink must be
    // reported as GPU-capable through the adapter too.
    bool supports_gpu_render() const noexcept override {
        return concrete_ && concrete_->supports_gpu_render();
    }
    bool supports_cpu_render() const noexcept override {
        return concrete_ && concrete_->supports_cpu_render();
    }

private:
    std::unique_ptr<plugins::IVideoSink> concrete_;
};

/** Plugin factory for the headless sink (id "headless"). */
class NIMRTC_API_STABLE HeadlessPluginFactory : public plugins::IVideoSinkFactory {
public:
    std::string_view id()           const noexcept override;
    std::string_view display_name() const noexcept override;
    plugins::IVideoSink* create(plugins::VideoSinkConfig cfg) const override;
};

/** Plugin factory for the pinned sink (id "pinned"). */
class NIMRTC_API_STABLE PinnedPluginFactory : public plugins::IVideoSinkFactory {
public:
    std::string_view id()           const noexcept override;
    std::string_view display_name() const noexcept override;
    plugins::IVideoSink* create(plugins::VideoSinkConfig cfg) const override;
};

/** Register the default video_sink plugin(s) with core::PluginRegistry.
 *  Idempotent. */
NIMRTC_API_STABLE void register_default_plugins() noexcept;

} // namespace nimrtc::video_sink
