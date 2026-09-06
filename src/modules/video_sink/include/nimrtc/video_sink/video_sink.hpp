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
 *  Thread safety: NOT thread-safe — render() must be serialized externally. */
class HeadlessSink : public plugins::IVideoSink {
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
    plugins::VideoSinkStats stats() const noexcept override;
    plugins::VideoSinkConfig config() const noexcept override;

private:
    plugins::VideoSinkConfig cfg_;
    plugins::VideoSinkStats  stats_{};
    bool                     opened_ = false;
};

/** PinnedSink — headless sink that asserts every frame's `capture_ts_us`
 *  is strictly increasing. Catches timestamp mishandling in tests. */
class PinnedSink : public plugins::IVideoSink {
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
    plugins::VideoSinkStats stats() const noexcept override;
    plugins::VideoSinkConfig config() const noexcept override;

private:
    plugins::VideoSinkConfig cfg_;
    plugins::VideoSinkStats  stats_{};
    bool                     opened_              = false;
    plugins::TimestampUs     last_capture_ts_us_  = -1;
};

// ---------------------------------------------------------------------------
// Factory (concrete-side)
// ---------------------------------------------------------------------------

/** Build a headless sink with the given config. */
std::unique_ptr<plugins::IVideoSink> create_headless_sink(
    plugins::VideoSinkConfig cfg) noexcept;

/** Build a pinned sink (headless + capture_ts ordering check) with the
 *  given config. */
std::unique_ptr<plugins::IVideoSink> create_pinned_sink(
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
class PluginAdapter : public plugins::IVideoSink {
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
    plugins::VideoSinkStats stats() const noexcept override;
    plugins::VideoSinkConfig config() const noexcept override;

private:
    std::unique_ptr<plugins::IVideoSink> concrete_;
};

/** Plugin factory for the headless sink (id "headless"). */
class HeadlessPluginFactory : public plugins::IVideoSinkFactory {
public:
    std::string_view id()           const noexcept override;
    std::string_view display_name() const noexcept override;
    plugins::IVideoSink* create(plugins::VideoSinkConfig cfg) const override;
};

/** Plugin factory for the pinned sink (id "pinned"). */
class PinnedPluginFactory : public plugins::IVideoSinkFactory {
public:
    std::string_view id()           const noexcept override;
    std::string_view display_name() const noexcept override;
    plugins::IVideoSink* create(plugins::VideoSinkConfig cfg) const override;
};

/** Register the default video_sink plugin(s) with core::PluginRegistry.
 *  Idempotent. */
void register_default_plugins() noexcept;

} // namespace nimrtc::video_sink
