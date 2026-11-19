/**
 * @file src/modules/video_sync/src/factory.cpp
 * @brief The `plugins::IVideoSyncController` adapter over
 *        `video_sync::RenderClock`.
 *
 * ## Why this file exists
 *
 * The *policy* lives in `video_sync::RenderClock`
 * (`include/nimrtc/video_sync/render_clock.hpp`) and it is
 * engine-agnostic: the module can be driven by the engine, by a
 * benchmark harness, or by a test rig.
 *
 * The *engine* may not include a concrete module's header (Layout
 * Invariant 4 / ADR-025), so it reaches the policy through the
 * `plugins::IVideoSyncController` seam
 * (`src/plugins/include/nimrtc/plugins/video_sync.hpp`).  This file is
 * the adapter that makes the module satisfy that seam.
 *
 * ## What the adapter does *not* do
 *
 * It adds no policy of its own.  The tolerance band, the drop / hold
 * mapping and the bounded-debt bookkeeping are the module's.  The
 * adapter is a translation layer, so there is exactly one implementation
 * of the correction policy in the tree.
 *
 * ## Threading
 *
 * Not thread-safe, like the clock it wraps.  The engine's receive path
 * is the only driver.
 */

#include "nimrtc/video_sync/render_clock.hpp"

#include <nimrtc/core/registry.hpp>
#include <nimrtc/plugins/video_sync.hpp>

#include <memory>
#include <string_view>

namespace nimrtc::video_sync {

namespace {

// ---------------------------------------------------------------------------
// The adapter
// ---------------------------------------------------------------------------

/**
 * @brief Implements `plugins::IVideoSyncController` over `RenderClock`.
 */
class RenderClockAdapter final : public plugins::IVideoSyncController {
public:
    explicit RenderClockAdapter(const plugins::VideoSyncConfig& config)
        : clock_(config) {}

    // ---- IPlugin ----------------------------------------------------------

    const char* name() const noexcept override {
        return "video_sync::RenderClock";
    }

    plugins::Status open() noexcept override {
        clock_.reset_counters();
        return plugins::kOk;
    }

    void close() noexcept override {
        clock_.reset_counters();
    }

    // ---- The decision -----------------------------------------------------

    plugins::VideoSyncDecision decide(std::int64_t skew_us,
                                       std::int64_t capture_ts_us) noexcept override {
        return clock_.decide(skew_us, capture_ts_us);
    }

    void note_delay_applied(std::int64_t actual_delay_us) noexcept override {
        clock_.note_delay_applied(actual_delay_us);
    }

    // ---- Configuration ----------------------------------------------------

    void configure(const plugins::VideoSyncConfig& config) noexcept override {
        clock_.configure(config);
    }

    std::int64_t tolerance_us() const noexcept override {
        return clock_.tolerance_us();
    }

    std::int64_t max_delay_us() const noexcept override {
        return clock_.max_delay_us();
    }

    bool enabled() const noexcept override {
        return clock_.enabled();
    }

    void set_enabled(bool enabled) noexcept override {
        clock_.set_enabled(enabled);
    }

    // ---- Reporting --------------------------------------------------------

    plugins::VideoSyncCounters counters() const noexcept override {
        return clock_.counters();
    }

    void reset_counters() noexcept override {
        clock_.reset_counters();
    }

private:
    /// The module's clock — the policy itself.
    RenderClock clock_;
};

// ---------------------------------------------------------------------------
// The factory
// ---------------------------------------------------------------------------

class RenderClockFactory final : public plugins::IVideoSyncControllerFactory {
public:
    std::string_view id() const noexcept override { return "render-clock"; }

    std::string_view display_name() const noexcept override {
        return "Render clock (video A/V sync: present / drop / delay)";
    }

    std::unique_ptr<plugins::IVideoSyncController> create(
        const plugins::VideoSyncConfig& config) const override {
        return std::make_unique<RenderClockAdapter>(config);
    }
};

const RenderClockFactory g_render_clock_factory{};

}  // anonymous namespace

// ---------------------------------------------------------------------------
// The registration entry point
// ---------------------------------------------------------------------------

void register_default_plugins() noexcept {
    static bool registered = false;
    if (registered) return;
    registered = true;
    core::PluginRegistry::instance().register_video_sync_controller(
        g_render_clock_factory.id(), &g_render_clock_factory);
}

}  // namespace nimrtc::video_sync
