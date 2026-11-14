/**
 * @file src/modules/rd_qoe/src/qoe_plugin.cpp
 * @brief The `plugins::IQoeCollector` adapter over `rd_qoe::QoeCollector`.
 *
 * ## Why this file exists
 *
 * The *metrics* live in `rd_qoe::QoeCollector` (`qoe_metrics.hpp`) and
 * they are engine-agnostic: the module can be driven by the engine, by a
 * benchmark harness, or by a test rig.
 *
 * The *engine* may not include a concrete module's header (Layout
 * Invariant 4 / ADR-025), so it reaches the collector through the
 * `plugins::IQoeCollector` seam
 * (`src/plugins/include/nimrtc/plugins/qoe_collector.hpp`).  This file
 * is the adapter that makes the module satisfy that seam: it translates
 * the seam's flat value types into the module's richer ones and back.
 *
 * ## What the adapter does *not* do
 *
 * It adds no policy of its own.  Every metric — what a freeze is, how
 * the latency percentiles are computed, how the sharpness proxy works —
 * is the module's.  The adapter is a translation layer, so there is
 * exactly one implementation of each metric in the tree.
 *
 * ## Threading
 *
 * Not thread-safe, like the collector it wraps.  The engine's tick
 * thread is the only driver.
 */

#include <nimrtc/rd_qoe/qoe_metrics.hpp>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/plugins/qoe_collector.hpp>

#include <memory>
#include <string_view>

namespace nimrtc::rd_qoe {

namespace {

// ---------------------------------------------------------------------------
// The adapter
// ---------------------------------------------------------------------------

/**
 * @brief Implements `plugins::IQoeCollector` over `QoeCollector`.
 */
class QoeCollectorAdapter final : public plugins::IQoeCollector {
public:
    explicit QoeCollectorAdapter(const plugins::QoeConfig& config)
        : collector_(to_module_config(config)) {}

    // ---- IPlugin ----------------------------------------------------------

    const char* name() const noexcept override {
        return "rd_qoe::QoeCollector";
    }

    plugins::Status open() noexcept override {
        collector_.reset();
        return plugins::kOk;
    }

    void close() noexcept override {
        collector_.reset();
    }

    // ---- The event surface -------------------------------------------------

    void on_frame_presented(
        const plugins::FrameObservation& observation) override {
        FrameObservation local;
        local.capture_ts_us = observation.capture_ts_us;
        local.render_ts_us  = observation.render_ts_us;
        local.frame_id      = observation.frame_id;
        local.source_fps    = observation.source_fps;
        collector_.on_frame_presented(local);
    }

    void on_frame_dropped(std::uint64_t count) override {
        collector_.on_frame_dropped(count);
    }

    void on_freeze_detected(std::int64_t duration_ms) override {
        collector_.on_freeze_detected(duration_ms);
    }

    void on_input_event_sent(std::int64_t ts_us,
                             std::uint64_t event_id) override {
        collector_.on_input_event_sent(ts_us, event_id);
    }

    void on_frame_presented_for_input(std::int64_t ts_us,
                                      std::uint64_t event_id) override {
        collector_.on_frame_presented_for_input(ts_us, event_id);
    }

    double observe_sharpness(const std::uint8_t* luma,
                             std::size_t stride,
                             std::size_t width,
                             std::size_t height) override {
        return collector_.observe_sharpness(luma, stride, width, height);
    }

    void on_sharpness_observed(double value) override {
        collector_.on_sharpness_observed(value);
    }

    plugins::QoeSnapshot snapshot() const override {
        const auto local = collector_.snapshot();
        plugins::QoeSnapshot out;
        out.frames_presented      = local.frames_presented;
        out.frames_dropped        = local.frames_dropped;
        out.freeze_count          = local.freeze_count;
        out.freeze_total_ms       = local.freeze_total_ms;
        out.freeze_longest_ms     = local.freeze_longest_ms;
        out.presented_fps         = local.presented_fps;
        out.latency_p50_us        = local.latency_p50_us;
        out.latency_p95_us        = local.latency_p95_us;
        out.latency_p99_us        = local.latency_p99_us;
        out.latency_max_us        = local.latency_max_us;
        out.input_to_photon_p50_us = local.input_to_photon_p50_us;
        out.input_to_photon_p95_us = local.input_to_photon_p95_us;
        out.input_to_photon_max_us = local.input_to_photon_max_us;
        out.sharpness_proxy       = local.sharpness_proxy;
        out.freeze_ratio          = local.freeze_ratio;
        out.session_duration_ms   = local.session_duration_ms;
        out.source_fps            = local.source_fps;
        return out;
    }

    void reset() noexcept override {
        collector_.reset();
    }

private:
    /// The seam's configuration translated into the module's.
    static QoeConfig to_module_config(const plugins::QoeConfig& config) {
        QoeConfig out;
        out.latency_reservoir  = config.latency_reservoir;
        out.input_reservoir    = config.input_reservoir;
        out.fps_window_us      = config.fps_window_us;
        out.max_pending_inputs = config.max_pending_inputs;
        return out;
    }

    /// The module's collector — the metrics themselves.
    QoeCollector collector_;
};

// ---------------------------------------------------------------------------
// The factory
// ---------------------------------------------------------------------------

class QoeCollectorFactory final : public plugins::IQoeCollectorFactory {
public:
    std::string_view id() const noexcept override { return "rd-qoe"; }

    std::string_view display_name() const noexcept override {
        return "Remote-desktop QoE metrics (freeze / latency / sharpness)";
    }

    std::unique_ptr<plugins::IQoeCollector> create(
        const plugins::QoeConfig& config) const override {
        return std::make_unique<QoeCollectorAdapter>(config);
    }
};

const QoeCollectorFactory g_qoe_collector_factory{};

}  // anonymous namespace

// ---------------------------------------------------------------------------
// The registration entry point
// ---------------------------------------------------------------------------

void register_default_plugins() noexcept {
    static bool registered = false;
    if (registered) return;
    registered = true;
    core::PluginRegistry::instance().register_qoe_collector(
        g_qoe_collector_factory.id(), &g_qoe_collector_factory);
}

}  // namespace nimrtc::rd_qoe
