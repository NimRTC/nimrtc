/**
 * @file src/modules/video_sink/src/video_sink.cpp
 * @brief Concrete headless / pinned video sinks + plugin adapters.
 *
 * @note P1 (R2-Batch2).
 */

#include <nimrtc/video_sink/video_sink.hpp>

#include <cassert>
#include <cstdint>
#include <utility>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>

namespace nimrtc::video_sink {

// ---------------------------------------------------------------------------
// HeadlessSink
// ---------------------------------------------------------------------------

HeadlessSink::HeadlessSink(plugins::VideoSinkConfig cfg) noexcept
    : cfg_(cfg) {}

HeadlessSink::~HeadlessSink() = default;

const char* HeadlessSink::name() const noexcept {
    return "nimrtc::video_sink::HeadlessSink";
}

plugins::Status HeadlessSink::open() noexcept {
    opened_ = true;
    return plugins::kOk;
}

void HeadlessSink::close() noexcept {
    opened_ = false;
}

plugins::Status HeadlessSink::render(const plugins::VideoSourceFrame& /*frame*/) noexcept {
    if (!opened_) return plugins::kErrNotReady;
    stats_.frames_rendered++;
    return plugins::kOk;
}

plugins::VideoSinkStats HeadlessSink::stats() const noexcept {
    return stats_;
}

plugins::VideoSinkConfig HeadlessSink::config() const noexcept {
    return cfg_;
}

// ---------------------------------------------------------------------------
// PinnedSink
// ---------------------------------------------------------------------------

PinnedSink::PinnedSink(plugins::VideoSinkConfig cfg) noexcept
    : cfg_(cfg) {}

PinnedSink::~PinnedSink() = default;

const char* PinnedSink::name() const noexcept {
    return "nimrtc::video_sink::PinnedSink";
}

plugins::Status PinnedSink::open() noexcept {
    opened_ = true;
    last_capture_ts_us_ = -1;
    return plugins::kOk;
}

void PinnedSink::close() noexcept {
    opened_ = false;
    last_capture_ts_us_ = -1;
}

plugins::Status PinnedSink::render(const plugins::VideoSourceFrame& frame) noexcept {
    if (!opened_) return plugins::kErrNotReady;
    // Strictly increasing capture_ts invariant.
    if (last_capture_ts_us_ >= 0
        && frame.capture_ts_us <= static_cast<plugins::TimestampUs>(last_capture_ts_us_)) {
        stats_.render_errors++;
        return plugins::kErrCorrupt;
    }
    last_capture_ts_us_ = frame.capture_ts_us;
    stats_.frames_rendered++;
    return plugins::kOk;
}

plugins::VideoSinkStats PinnedSink::stats() const noexcept {
    return stats_;
}

plugins::VideoSinkConfig PinnedSink::config() const noexcept {
    return cfg_;
}

// ---------------------------------------------------------------------------
// Concrete-side factories
// ---------------------------------------------------------------------------

std::unique_ptr<plugins::IVideoSink>
create_headless_sink(plugins::VideoSinkConfig cfg) noexcept {
    return std::make_unique<HeadlessSink>(std::move(cfg));
}

std::unique_ptr<plugins::IVideoSink>
create_pinned_sink(plugins::VideoSinkConfig cfg) noexcept {
    return std::make_unique<PinnedSink>(std::move(cfg));
}

// ---------------------------------------------------------------------------
// PluginAdapter
// ---------------------------------------------------------------------------

PluginAdapter::PluginAdapter(std::unique_ptr<plugins::IVideoSink> concrete) noexcept
    : concrete_(std::move(concrete)) {}

PluginAdapter::~PluginAdapter() = default;

const char* PluginAdapter::name() const noexcept {
    return concrete_ ? concrete_->name() : "nimrtc::video_sink::PluginAdapter(null)";
}

plugins::Status PluginAdapter::open() noexcept {
    return concrete_ ? concrete_->open() : plugins::kErrNotReady;
}

void PluginAdapter::close() noexcept {
    if (concrete_) concrete_->close();
}

plugins::Status PluginAdapter::render(const plugins::VideoSourceFrame& frame) noexcept {
    return concrete_ ? concrete_->render(frame) : plugins::kErrNotReady;
}

plugins::VideoSinkStats PluginAdapter::stats() const noexcept {
    return concrete_ ? concrete_->stats() : plugins::VideoSinkStats{};
}

plugins::VideoSinkConfig PluginAdapter::config() const noexcept {
    return concrete_ ? concrete_->config() : plugins::VideoSinkConfig{};
}

// ---------------------------------------------------------------------------
// Plugin factories
// ---------------------------------------------------------------------------

std::string_view HeadlessPluginFactory::id() const noexcept {
    return "headless";
}

std::string_view HeadlessPluginFactory::display_name() const noexcept {
    return "Headless sink (drops pixels; counts frames)";
}

plugins::IVideoSink*
HeadlessPluginFactory::create(plugins::VideoSinkConfig cfg) const {
    auto concrete = std::make_unique<HeadlessSink>(std::move(cfg));
    return new PluginAdapter(std::move(concrete));
}

std::string_view PinnedPluginFactory::id() const noexcept {
    return "pinned";
}

std::string_view PinnedPluginFactory::display_name() const noexcept {
    return "Headless sink with strict capture_ts ordering check";
}

plugins::IVideoSink*
PinnedPluginFactory::create(plugins::VideoSinkConfig cfg) const {
    auto concrete = std::make_unique<PinnedSink>(std::move(cfg));
    return new PluginAdapter(std::move(concrete));
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

namespace detail {

void do_register_default_plugins() noexcept {
    static const struct Registrar {
        Registrar() {
            static HeadlessPluginFactory s_headless{};
            static PinnedPluginFactory   s_pinned{};
            nimrtc::core::PluginRegistry::instance().register_video_sink(
                s_headless.id(), &s_headless);
            nimrtc::core::PluginRegistry::instance().register_video_sink(
                s_pinned.id(), &s_pinned);
        }
    } s_registrar;
    (void)s_registrar;
}

} // namespace detail

void register_default_plugins() noexcept {
    static const int once = []() {
        detail::do_register_default_plugins();
        return 1;
    }();
    (void)once;
}

} // namespace nimrtc::video_sink
