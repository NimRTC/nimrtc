/**
 * @file src/engine/src/engine_qoe.cpp
 * @brief The QoE collector's engine-side plumbing (remote-desktop plan
 *        **J3** / **J6**).
 *
 * The *metrics* live in the `rd_qoe` module and are reached through the
 * `plugins::IQoeCollector` seam — the engine never includes a concrete
 * module's header (Layout Invariant 4 / ADR-025).  This file is the
 * engine's half of the wiring:
 *
 *   - it resolves the collector through the registry's qoe-collector
 *     slot (`EngineConfig::qoe_collector_name`), whose default is the
 *     built-in `"rd-qoe"`;
 *   - it reports the events the engine knows about — a frame reached
 *     the viewer, a frame was dropped, an input event went out;
 *   - it publishes the snapshot through `Stats::qoe`.
 *
 * @see engine_qoe_seam.hpp — the seam-backed collector.
 * @see docs/plan/remote-desktop-plan.md §Track J (J3, J6).
 */

#include "nimrtc/engine/engine.hpp"

#include "engine_impl.hpp"
#include "engine_qoe_seam.hpp"

#include <chrono>
#include <string>
#include <string_view>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/core/time.hpp>   // core::SteadyClock

namespace nimrtc::engine {

namespace {

/// The local monotonic instant, in microseconds.
std::int64_t now_us() noexcept {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            core::SteadyClock::now().time_since_epoch()).count());
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// The resolve step — the registry seam
// ---------------------------------------------------------------------------

uint32_t NimRTCEngine::resolve_qoe_collector() noexcept {
    // The explicit switch wins over everything: a host that turned the
    // metrics off gets no collector, whatever the name says.
    if (!config_.qoe_collector_enabled) return 0;

    // An empty id is the **explicit opt-out**: the host asked for no
    // QoE collector, so the engine resolves nothing and `Stats::qoe`
    // carries only the frame counters (the presented / dropped counts,
    // which are free).  The name is the only place that says so, and
    // it says it unambiguously.
    if (config_.qoe_collector_name.empty()) return 0;

    // The default id names the built-in collector, which lives in the
    // `rd_qoe` module — an **optional** module.  A build without it has
    // no such factory, and that is not an error: the metrics simply are
    // not measured.  The engine degrades gracefully rather than failing
    // `open()`, because "this build has no QoE module" is a property of
    // the build, not a mistake by the host.
    //
    // Every *other* id is a hard error: a typo'd id would otherwise
    // silently disable the metrics the host asked for, which is exactly
    // the failure mode a default is supposed to prevent.
    if (config_.qoe_collector_name == kDefaultQoeCollectorId &&
        core::PluginRegistry::instance().get_qoe_collector(
            kDefaultQoeCollectorId) == nullptr) {
        const std::string msg =
            std::string{"engine: the QoE collector '"} +
            std::string{kDefaultQoeCollectorId} +
            "' is not in this build (the rd_qoe module is optional) -- "
            "the QoE metrics are not measured; set "
            "EngineConfig::qoe_collector_name to \"\" to silence this";
        core::log::Logger::instance().warn(msg);
        return 0;
    }

    const plugins::Status rc =
        impl_->qoe.resolve(config_.qoe_collector_name, config_.qoe_config);
    if (rc != plugins::kOk) {
        const std::string msg =
            std::string{"engine: the QoE collector '"} +
            config_.qoe_collector_name +
            "' could not be resolved through the registry";
        core::log::Logger::instance().error(msg);
        if (on_error_) on_error_(core::kEngineConfig, msg);
        return core::kEngineConfig;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The event surface
// ---------------------------------------------------------------------------

void NimRTCEngine::note_frame_presented(std::int64_t capture_ts_us,
                                         std::uint64_t frame_id,
                                         double source_fps) noexcept {
    if (impl_ == nullptr) return;
    impl_->qoe.note_frame_presented(capture_ts_us, frame_id, source_fps);
}

void NimRTCEngine::note_frame_dropped(std::uint64_t count) noexcept {
    if (impl_ == nullptr) return;
    impl_->qoe.note_frame_dropped(count);
}

void NimRTCEngine::note_freeze(std::int64_t duration_ms) noexcept {
    if (impl_ == nullptr) return;
    impl_->qoe.note_freeze(duration_ms);
}

double NimRTCEngine::note_sharpness(const std::uint8_t* luma,
                                    std::size_t stride,
                                    std::size_t width,
                                    std::size_t height) noexcept {
    if (impl_ == nullptr) return 0.0;
    return impl_->qoe.note_sharpness(luma, stride, width, height);
}

plugins::Status NimRTCEngine::send_input_event(
    std::span<const std::uint8_t> payload,
    std::uint64_t event_id) noexcept {
    const plugins::Status rc = send_control_message(payload);

    // The event is stamped only when the control channel **accepted**
    // it — a message the channel refused (the control plane is off, the
    // queue is full) never reached the peer, so there is no honest
    // latency to report for it.
    //
    // The stamp is the moment of acceptance, which is the engine's last
    // point of responsibility before the transport: the measurement
    // therefore includes the queueing delay and the wire time, and it
    // includes the control transport (plan item J6 asks for the
    // end-to-end value, "including the control transport").
    if (rc == plugins::kOk && impl_ != nullptr && impl_->qoe.active()) {
        impl_->qoe.note_input_event(now_us(), event_id);
    }
    return rc;
}

}  // namespace nimrtc::engine
