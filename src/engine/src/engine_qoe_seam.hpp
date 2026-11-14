/**
 * @file src/engine/src/engine_qoe_seam.hpp
 * @brief PRIVATE engine collaborator: the QoE collector **resolved
 *        through the registry seam** (remote-desktop plan J3 / J6).
 *
 * ## What this is
 *
 * The *metrics* of the remote-desktop quality-of-experience live in the
 * `rd_qoe` module (`rd_qoe::QoeCollector`) and are reached through the
 * `plugins::IQoeCollector` seam
 * (`src/plugins/include/nimrtc/plugins/qoe_collector.hpp`).  This header
 * is the engine's thin adapter over that seam: it owns the resolved
 * collector, forwards the engine's events into it, and owns the one
 * piece of *policy* the engine must have — **which presented frame
 * reflects which input event**.
 *
 * ## What the engine knows, and what it does not
 *
 * The engine knows three things about the experience:
 *
 *   - a frame **reached the viewer** (the sink's `render()` returned
 *     `kOk`) — with the capture timestamp, so the end-to-end latency is
 *     measurable;
 *   - a frame was **dropped** (the decoder failed, the render clock
 *     dropped it for sync, the jitter buffer gave up);
 *   - an **input event** went out on the control plane.
 *
 * It does **not** know what a freeze is, what the latency percentiles
 * are, or how the sharpness proxy works — those are the module's
 * definitions, and duplicating them here would give the tree two
 * implementations of the same metric that drift apart.
 *
 * ## The input-to-photon pairing (plan item J6)
 *
 * The input-to-photon latency is the one metric that needs two sides
 * of the session: the operator's event going **out**, and the frame
 * that **reflects** it coming back.  The engine pairs them with the
 * **ref-frame id space**:
 *
 *   - the operator acts on the frame they are looking at, whose id is
 *     the peer's `frame_seq` — the engine publishes it as
 *     `last_displayed_ref_frame_id()`;
 *   - the host sends the event with that id (`send_input_event()`);
 *   - the peer's screen changes, and the frames that come back carry
 *     **increasing** ids in the same space;
 *   - so the first presented frame whose id is **strictly greater** than
 *     the event's id is the frame that reflects the event.
 *
 * That is the rule this class implements.  It is a *heuristic* — the
 * peer might have changed the screen for another reason, or might not
 * have changed it at all — and it is documented as one.  The
 * alternative (the peer naming the reflecting frame explicitly) would
 * need a protocol change, and the heuristic already measures the thing
 * the operator feels.
 *
 * When the host sends an event with id **0** — it did not bind the
 * event to a frame — the event is paired with the **next** presented
 * frame, which is the best the engine can do without a binding.
 *
 * ## The clock
 *
 * Every timestamp the engine pushes is on the **host's monotonic clock**
 * (the same `now_us()` the engine's other timestamps use).  For an
 * inbound frame the capture timestamp is the *peer's*, but the ADR-017
 * timeline has already normalised it onto the local timeline by the
 * time the frame reaches the render path — so the subtraction the
 * collector performs is a subtraction of two local-clock values.
 *
 * ## Threading
 *
 * Not thread-safe, like every other pipeline object here.  The engine's
 * tick thread is the only driver: `note_frame_presented()` is called
 * from the receive path (the engine's thread) and `note_input_event()`
 * from `send_input_event()` (the host's thread).
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * @see docs/plan/remote-desktop-plan.md §Track J (J3, J6).
 */

#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string_view>
#include <utility>

#include <nimrtc/plugins/base.hpp>
#include <nimrtc/plugins/qoe_collector.hpp>

namespace nimrtc::engine::detail {

// ---------------------------------------------------------------------------
// The seam-backed QoE collector
// ---------------------------------------------------------------------------

/**
 * @brief The engine's QoE collector, resolved through the seam.
 *
 * The class owns three things:
 *
 *   1. the resolved `plugins::IQoeCollector` (the *metrics* live there);
 *   2. the **counters** the engine publishes when no collector is
 *      configured — so a host that does not ask for the QoE metrics
 *      still learns how many frames were presented and dropped, at no
 *      cost;
 *   3. the **pending input events**, which is the only policy the
 *      engine owns: which presented frame reflects which input event.
 */
class QoeSeam {
public:
    QoeSeam() = default;

    QoeSeam(const QoeSeam&)            = delete;
    QoeSeam& operator=(const QoeSeam&) = delete;

    /**
     * @brief Resolve the collector through the registry and open it.
     *
     * @param factory_id  the plugin id of the qoe-collector factory.
     * @param config      the collector's configuration.
     * @return `kOk` when the collector was resolved and opened;
     *         `kErrNotReady` when the factory id is unknown or the
     *         factory returned nothing.
     */
    plugins::Status resolve(std::string_view factory_id,
                             const plugins::QoeConfig& config) noexcept;

    /** True when a collector was resolved and opened. */
    [[nodiscard]] bool active() const noexcept { return collector_ != nullptr; }

    /** The resolved collector (nullptr when the QoE metrics are off). */
    [[nodiscard]] plugins::IQoeCollector* collector() const noexcept {
        return collector_.get();
    }

    // ---- The event surface -------------------------------------------------

    /**
     * @brief Report a frame the viewer saw.
     *
     * @param capture_ts_us  the frame's capture timestamp (the local
     *                       timeline for an inbound frame).
     * @param ref_frame_id   the frame's id in the **peer's ref-frame id
     *                       space** — the id the operator's input events
     *                       carry, so the engine can pair the two.
     * @param source_fps     the source frame rate, or 0 when unknown.
     */
    void note_frame_presented(std::int64_t capture_ts_us,
                              std::uint64_t ref_frame_id,
                              double source_fps) noexcept;

    /** Report a frame the pipeline dropped. */
    void note_frame_dropped(std::uint64_t count = 1) noexcept;

    /**
     * @brief Report an input event that went out on the control plane.
     *
     * @param ts_us     the moment the event went out, on the host's
     *                  monotonic clock.
     * @param event_id  the id of the frame the operator acted on (the
     *                  peer's `frame_seq`), or 0 when the host did not
     *                  bind the event to a frame.
     */
    void note_input_event(std::int64_t ts_us, std::uint64_t event_id) noexcept;

    /** Report the sharpness of a presented picture. */
    double note_sharpness(const std::uint8_t* luma,
                          std::size_t stride,
                          std::size_t width,
                          std::size_t height) noexcept;

    // ---- The results --------------------------------------------------------

    /**
     * @brief The metrics so far.
     *
     * When no collector is configured the snapshot carries the
     * *counters* the engine keeps on its own (the presented and dropped
     * frame counts) and zero for everything else — which is the honest
     * answer: the metrics were not measured.
     */
    [[nodiscard]] plugins::QoeSnapshot snapshot() const noexcept;

    /** The number of frames the engine saw reach the viewer. */
    [[nodiscard]] std::uint64_t frames_presented() const noexcept {
        return frames_presented_;
    }

    /** The number of frames the pipeline dropped. */
    [[nodiscard]] std::uint64_t frames_dropped() const noexcept {
        return frames_dropped_;
    }

    /** The number of input events still waiting for their photon frame. */
    [[nodiscard]] std::size_t pending_input_events() const noexcept {
        return pending_inputs_.size();
    }

    /** Reset the counters and the collector. */
    void reset() noexcept;

    /** Close the collector and drop the resolved instance. */
    void close() noexcept;

private:
    /// One input event waiting for the frame that reflects it.
    struct PendingInput {
        std::int64_t  sent_ts_us = 0;
        std::uint64_t event_id = 0;
        bool          bound_to_frame = false;
    };

    /// The resolved collector (the metrics live there).
    std::unique_ptr<plugins::IQoeCollector> collector_;

    /// The counters the engine keeps whether or not a collector is
    /// configured — a host gets the frame counts for free.
    std::uint64_t frames_presented_ = 0;
    std::uint64_t frames_dropped_ = 0;

    /// The input events waiting for their photon frame, in send order.
    std::deque<PendingInput> pending_inputs_;
};

}  // namespace nimrtc::engine::detail
