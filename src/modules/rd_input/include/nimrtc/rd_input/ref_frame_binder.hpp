/**
 * @file nimrtc/rd_input/ref_frame_binder.hpp
 * @brief The input <-> frame binding (remote-desktop plan G5, module half).
 *
 * ## The product promise
 *
 * "The click happened on the frame the operator was looking at."  That is
 * plan G5.  The engine already owns the machinery that makes it possible:
 * a bounded `ref_frame` id -> `capture_ts_us` table, recorded for both
 * directions (v1.1 T1).  What was missing is the **link from the input
 * event to the frame**.
 *
 * ## What this module contributes
 *
 * The `InputEvent` gains an optional `ref_frame` id, and the wire format
 * gains a flag that says "a ref-frame id follows".  A host that wants the
 * binding turns the flag on; a host that does not pays **zero** bytes for
 * it (the flag bit stays clear), which matters because the input path is
 * the latency-critical path.
 *
 * The `RefFrameBinder` is the piece that *stamps* an event.  It holds a
 * callable that answers "what is the current ref-frame id?" — in the
 * engine that callable reads the send-side table — and stamps every event
 * that passes through it.  Keeping the callable abstract is what lets the
 * binder be unit-tested without an engine.
 *
 * ## Why it is a module and not a seam
 *
 * Plan G3 already decided the shape: the input path owns the *payload
 * format* and the *input-side policy*; the transport is chosen elsewhere.
 * The binding is input-side policy.  So this lives in `src/modules/rd_input/`
 * and not in `src/plugins/` (AGENTS.md L2 / L3).
 *
 * @note Remote-desktop plan G5 (module half).  The engine half — the
 *       public accessor for the current ref-frame id and the resolution of
 *       an incoming event's id against the displayed frame — is the
 *       engine's.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <functional>
#include <optional>

#include "nimrtc/rd_input/input_event.hpp"

namespace nimrtc::rd_input {

// ---------------------------------------------------------------------------
// RefFrameBinder
// ---------------------------------------------------------------------------

/**
 * @brief Stamps outgoing input events with the current `ref_frame` id.
 *
 * ## The contract
 *
 * The binder asks its provider for the current ref-frame id each time an
 * event is stamped.  A provider that has no frame to point at returns
 * `std::nullopt`, and the event is then left **un-stamped** — an event
 * with no frame reference is legal on the wire (the flag stays clear), and
 * it is *not* an error.  That is the difference between "the operator
 * clicked before any frame was sent" and "the binding is broken".
 *
 * ## Threading
 *
 * Not thread-safe.  One binder belongs to one input-capture path, and
 * the path is serialised by the host (the same rule the coalescer obeys).
 */
class NIMRTC_API_STABLE RefFrameBinder {
public:
    /**
     * @brief The provider of the current ref-frame id.
     *
     * Returns the id of the frame the operator was looking at, or
     * `std::nullopt` when there is none.
     */
    using Provider = std::function<std::optional<std::uint64_t>()>;

    /**
     * @brief Construct a binder over a provider.
     *
     * An empty provider is allowed and behaves like a provider that never
     * has a frame: every event is left un-stamped.
     */
    explicit RefFrameBinder(Provider provider);

    /**
     * @brief Stamp one event in place.
     *
     * The event's `ref_frame` is set to the provider's current id.  When
     * the provider has no id, the event's existing `ref_frame` is
     * **cleared** — the binder never leaves a stale id on an event.
     *
     * @return true when the event was stamped with an id.
     */
    bool stamp(InputEvent& event) const;

    /**
     * @brief Stamp one event and return it by value.
     */
    InputEvent stamped(InputEvent event) const;

    /**
     * @brief The current ref-frame id, or nullopt.
     *
     * Exposed so a host can log or report the binding without stamping
     * an event.
     */
    std::optional<std::uint64_t> current() const;

    /** True when the binder has a usable provider. */
    bool has_provider() const noexcept;

private:
    Provider provider_;
};

} // namespace nimrtc::rd_input
