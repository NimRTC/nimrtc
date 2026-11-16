/**
 * @file src/engine/src/engine_qoe_seam.cpp
 * @brief The QoE seam's implementation (remote-desktop plan J3 / J6).
 *
 * @see engine_qoe_seam.hpp — the header, and why this design.
 */

#include "engine_qoe_seam.hpp"

#include <nimrtc/core/registry.hpp>
#include <nimrtc/core/log.hpp>
#include <nimrtc/core/time.hpp>   // core::SteadyClock

#include <chrono>
#include <utility>

namespace nimrtc::engine::detail {

namespace {

/// The local monotonic instant, in microseconds.
std::int64_t now_us() noexcept {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            core::SteadyClock::now().time_since_epoch()).count());
}

/// The bound on the pending input events.
///
/// An input event whose frame never arrives would otherwise pin an
/// entry forever.  The bound is the same one the collector's own
/// configuration names (`QoeConfig::max_pending_inputs`), so the two
/// ends of the path agree.
constexpr std::size_t kMaxPendingInputs = 4096;

}  // anonymous namespace

// ---------------------------------------------------------------------------
// The resolve step — the registry seam
// ---------------------------------------------------------------------------

plugins::Status QoeSeam::resolve(std::string_view factory_id,
                                  const plugins::QoeConfig& config) noexcept {
    const auto* factory =
        core::PluginRegistry::instance().get_qoe_collector(factory_id);
    if (factory == nullptr) {
        core::log::Logger::instance().error(
            "engine: the QoE collector '" + std::string{factory_id} +
            "' is not registered");
        return plugins::kErrNotReady;
    }

    auto collector = factory->create(config);
    if (!collector) {
        core::log::Logger::instance().error(
            "engine: the QoE collector factory '" + std::string{factory_id} +
            "' returned no collector");
        return plugins::kErrNotReady;
    }

    const plugins::Status rc = collector->open();
    if (rc != plugins::kOk) {
        core::log::Logger::instance().error(
            "engine: the QoE collector '" + std::string{factory_id} +
            "' failed to open");
        return rc;
    }

    collector_ = std::move(collector);
    frames_presented_ = 0;
    frames_dropped_ = 0;
    pending_inputs_.clear();
    return plugins::kOk;
}

// ---------------------------------------------------------------------------
// The event surface
// ---------------------------------------------------------------------------

void QoeSeam::note_frame_presented(std::int64_t capture_ts_us,
                                   std::uint64_t ref_frame_id,
                                   double source_fps) noexcept {
    ++frames_presented_;
    if (!collector_) return;

    plugins::FrameObservation observation;
    observation.capture_ts_us = capture_ts_us;
    observation.render_ts_us  = now_us();
    observation.frame_id      = ref_frame_id;
    observation.source_fps    = source_fps;
    collector_->on_frame_presented(observation);

    // ---- The input-to-photon pairing (plan item J6) --------------------
    //
    // A presented frame reflects an input event when its ref-frame id is
    // **strictly greater** than the event's: the operator acted on the
    // frame they were looking at, so the frame that reflects the action
    // is a *later* frame in the same id space.
    //
    // An event that was **not** bound to a frame (id 0) is paired with
    // the next presented frame, whatever its id — that is the best the
    // engine can do without a binding.
    //
    // The pairings are resolved in send order, so a burst of events
    // pairs with a burst of frames in the same order.
    const std::int64_t render_ts_us = observation.render_ts_us;
    for (auto it = pending_inputs_.begin(); it != pending_inputs_.end();) {
        const bool reflects =
            !it->bound_to_frame || ref_frame_id > it->event_id;
        if (!reflects) {
            ++it;
            continue;
        }
        collector_->on_frame_presented_for_input(render_ts_us, it->event_id);
        it = pending_inputs_.erase(it);
    }
}

void QoeSeam::note_frame_dropped(std::uint64_t count) noexcept {
    if (count == 0) return;
    frames_dropped_ += count;
    if (collector_) collector_->on_frame_dropped(count);
}

void QoeSeam::note_input_event(std::int64_t ts_us,
                               std::uint64_t event_id) noexcept {
    if (!collector_) return;

    collector_->on_input_event_sent(ts_us, event_id);

    PendingInput pending;
    pending.sent_ts_us = ts_us;
    pending.event_id = event_id;
    pending.bound_to_frame = (event_id != 0);
    pending_inputs_.push_back(pending);

    // The bound: the oldest event is forgotten rather than pinning an
    // entry forever.
    while (pending_inputs_.size() > kMaxPendingInputs) {
        pending_inputs_.pop_front();
    }
}

double QoeSeam::note_sharpness(const std::uint8_t* luma,
                               std::size_t stride,
                               std::size_t width,
                               std::size_t height) noexcept {
    if (!collector_) return 0.0;
    return collector_->observe_sharpness(luma, stride, width, height);
}

// ---------------------------------------------------------------------------
// The results
// ---------------------------------------------------------------------------

plugins::QoeSnapshot QoeSeam::snapshot() const noexcept {
    if (collector_) return collector_->snapshot();

    // No collector: the frame counts are still honest (the engine keeps
    // them), and the *metrics* are zero — which is the honest answer to
    // "what was the latency percentile?", because it was not measured.
    plugins::QoeSnapshot out;
    out.frames_presented = frames_presented_;
    out.frames_dropped   = frames_dropped_;
    return out;
}

void QoeSeam::reset() noexcept {
    frames_presented_ = 0;
    frames_dropped_ = 0;
    pending_inputs_.clear();
    if (collector_) collector_->reset();
}

void QoeSeam::close() noexcept {
    if (collector_) {
        collector_->close();
        collector_.reset();
    }
    frames_presented_ = 0;
    frames_dropped_ = 0;
    pending_inputs_.clear();
}

}  // namespace nimrtc::engine::detail
