/**
 * @file src/engine/src/engine_control_inbox.hpp
 * @brief PRIVATE engine collaborator: the control channel's engine-side
 *        inbox (the lock + the pending-message queue).
 *
 * ## What this is
 *
 * The remote-desktop control channel (plan G1 / G2 / G6) is *framing*
 * and lives in the `rd_input` module (`engine_control_seam.hpp` is the
 * engine's adapter over it).  What the **engine** adds on top of it is
 * a small piece of plumbing with two parts:
 *
 *   1. **a lock** — `send_control_message()` is called from the host's
 *      thread while `flush_control_channel()` runs on the engine's tick
 *      thread, and the raw-UDP control path delivers on the ARQ
 *      thread.  So the channel's state needs a lock.  It is the *only*
 *      lock in the control path.
 *   2. **a pending queue** — the raw-UDP path delivers on the ARQ
 *      thread and the DTLS path on the engine's thread, so a message
 *      cannot be handed to the host from inside the receive callback:
 *      the host's callback would then run on an arbitrary thread, and
 *      (worse) under the channel lock.  So the receive path *queues* the
 *      message here and the engine's tick *drains* it — which means the
 *      host's callback always runs on the engine's thread, never under
 *      a lock.
 *
 * ## Why it is its own type
 *
 * The remediation plan §4.3 (D1 — engine split) puts "a member that
 * only one feature reads or writes" into that feature's collaborator.
 * The lock and the queue belong to the control channel and to nothing
 * else, so they live here rather than as loose `Impl` members — which
 * also keeps `engine_impl.hpp` under its line budget.
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * @see engine_control_seam.hpp — the engine's adapter over the channel.
 * @see docs/plan/remote-desktop-plan.md §Track G (G1, G2, G6).
 */
#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace nimrtc::engine::detail {

/**
 * @brief The engine-side inbox of the control channel.
 *
 * The queue is bounded by the tick's drain rate and is cleared when the
 * session has no host callback, so a session that does not consume the
 * control path cannot accumulate messages without bound.
 */
class ControlChannelInbox {
public:
    ControlChannelInbox() = default;

    ControlChannelInbox(const ControlChannelInbox&)            = delete;
    ControlChannelInbox& operator=(const ControlChannelInbox&) = delete;

    /** The lock that guards the **channel's** state (the framing, the
     *  outbound queue, the inbound reassembly buffer).
     *
     *  It is the *only* lock the channel itself is guarded by. */
    std::mutex channel_mutex;

    /** The lock that guards the **pending queue**.
     *
     *  It is a *separate* lock from `channel_mutex` on purpose: the
     *  channel's inbound callback runs **inside** `on_transport_bytes()`,
     *  which the receive path calls while holding `channel_mutex`.  If
     *  the callback took the same lock, the path would deadlock on a
     *  non-recursive mutex.  Two locks with a strict hierarchy
     *  (`channel_mutex` → `queue_mutex`, never the other way round)
     *  keep the path deadlock-free. */
    std::mutex queue_mutex;

    /** The control messages that arrived but have not been delivered to
     *  the host yet (oldest first). */
    std::deque<std::vector<std::uint8_t>> pending;
};

}  // namespace nimrtc::engine::detail
