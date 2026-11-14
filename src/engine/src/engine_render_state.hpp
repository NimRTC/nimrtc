/**
 * @file src/engine/src/engine_render_state.hpp
 * @brief PRIVATE engine collaborator: the receive-side render state.
 *
 * ## What lives here
 *
 *   - the **render clock** (remote-desktop plan E5): the
 *     `detail::VideoSyncController` that turns the observed skew into an
 *     align / drop / delay decision, plus the counters the engine
 *     publishes;
 *   - the **displayed-frame record** (plan E9): what the operator was
 *     looking at when they clicked — the capture timestamp and the
 *     `ref_frame` id of the last frame the engine handed to the sink;
 *   - the **presentation configuration** (plan E7): the target size and
 *     the aspect policy the engine resolves into the sink's
 *     configuration;
 *   - the **input-echo state** (plan E6): the switch, the maximum echo
 *     age, the rate limiter and the seam through which an echoed
 *     event leaves the engine.
 *
 * ## Why it is a collaborator and not a pile of `Impl` members
 *
 * Architecture remediation plan §4.3 (D1 — engine split): a member
 * that only one feature reads or writes belongs to that feature's
 * collaborator, not to `Impl`.  All four features above are the
 * *receive / presentation* plane, and grouping them keeps
 * `engine_impl.hpp` from becoming the catch-all it used to be.
 *
 * ## Threading
 *
 * Not thread-safe by itself: the engine's tick loop and the receive
 * path are serialised by the caller (the engine's contract), and the
 * render state is written from the same thread that reads it.
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

#include <nimrtc/engine/engine.hpp>

#include "input_echo_controller.hpp"
#include "video_sync_controller.hpp"

namespace nimrtc::engine::detail {

/** One displayed frame: what the operator was looking at. */
struct DisplayedFrameRecord {
    /** The frame's capture timestamp (the *peer's* capture instant, on
     *  the ADR-017 timeline).  0 when the timeline had not reconstructed
     *  it yet. */
    std::int64_t capture_ts_us = 0;
    /** The `ref_frame` id the capture timestamp resolves to.  0 when
     *  the id is unknown. */
    std::uint64_t ref_frame_id = 0;
    /** The frame's per-SSRC sequence number. */
    std::uint32_t frame_seq = 0;
    /** The local monotonic instant at which the frame was handed to the
     *  sink. */
    std::int64_t rendered_at_us = 0;
    /** True when `ref_frame_id` resolves to a known frame. */
    bool has_ref_frame = false;
};

/** The receive-side render state. */
struct RenderState {
    // ---- The render clock (plan E5) ------------------------------------
    //
    // The controller owns the decision rule and the counters.  The
    // engine asks it before every render; the answer is a present, a
    // drop or a (bounded) delay.
    VideoSyncController sync;

    /** The peer capture instant of the audio frame that is currently at
     *  the speaker, in Unix microseconds (the peer's timeline).
     *
     *  This is the **audio side** of the A/V comparison.  It is written
     *  from the audio delivery path (where the ADR-017 reconstruction
     *  produced it) and read by the render path — so the two halves of
     *  the comparison live on the *same* timeline and the skew between
     *  them is meaningful.
     *
     *  Zero means "no audio reference yet" — the render path then has
     *  nothing to align against and presents every frame. */
    std::int64_t audio_playout_capture_us = 0;

    // ---- The displayed-frame record (plan E9) --------------------------
    //
    // The engine records, per displayed frame, the capture timestamp
    // and the `ref_frame` id it resolves to.  The record is what lets
    // a host answer "the user clicked on what they were looking at".
    //
    // A bounded history is kept so a *recent* frame can still be
    // resolved by id (the operator's click may lag the click by a
    // frame or two).  The ring is bounded so it cannot grow without
    // bound.
    static constexpr std::size_t kDisplayedHistory = 64;

    /** The most recently displayed frame. */
    std::optional<DisplayedFrameRecord> last_displayed;

    /** The bounded history of displayed frames (oldest first). */
    std::deque<DisplayedFrameRecord> displayed_history;

    // ---- The presentation configuration (plan E7) ----------------------
    //
    // The target size the viewer wants and the policy that says how
    // the picture is fitted into it.  The engine resolves the pair
    // into the sink's configuration (`plugins::VideoSinkConfig`) when
    // it creates the sink.
    //
    // The policy is the *engine's* enum (`VideoAspectPolicy`), because
    // the engine's public header is the surface a host configures.  It
    // is translated into the sink seam's own enum
    // (`plugins::VideoScaleMode`) at the seam, so neither header has
    // to know about the other's enum.
    std::uint32_t presentation_width  = 0;
    std::uint32_t presentation_height = 0;

    /** How the picture is fitted into the target. */
    VideoAspectPolicy presentation_policy = VideoAspectPolicy::kLetterbox;

    /** True once the host configured a presentation (i.e. the engine
     *  has something to say about the sink's geometry). */
    bool presentation_configured = false;

    // ---- The input-echo state (plan E6) -------------------------------
    //
    // The controller owns the decision (on / off, the age budget, the
    // rate limit) and the counters.  The engine asks it before every
    // echo candidate.
    InputEchoController echo;
};

}  // namespace nimrtc::engine::detail
