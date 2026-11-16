/**
 * @file src/engine/src/engine_control_seam.hpp
 * @brief PRIVATE engine collaborator: the control channel **resolved
 *        through the registry seam** (remote-desktop plan G1 / G2).
 *
 * ## What this is
 *
 * The *mechanism* of the remote-desktop control plane lives in the
 * `rd_input` module (`rd_input::ControlChannel`) and is reached
 * through the `plugins::IControlChannel` seam
 * (`src/plugins/include/nimrtc/plugins/control_channel.hpp`).  This
 * header is the engine's thin adapter over that seam: it owns the
 * resolved `std::unique_ptr<plugins::IControlChannel>`, the *sink*
 * that puts the framed bytes on the wire, and the *inbound* feed.
 *
 * ## Why the engine does not own the framing
 *
 * The engine used to carry its own 2-byte big-endian length prefix
 * (`engine_control_channel.hpp`).  That was a **second** framing
 * implementation next to the module's, and two framings on the same
 * path are two protocols that will drift.  So the engine's private
 * framing is gone: the module's framing
 * (`[0xCC][version][varint length][payload]`) is the only one.
 *
 * ## Threading
 *
 * The channel itself is not thread-safe.  The engine's tick thread is
 * the only driver of the *transport*, but the public
 * `send_control_message()` is called from the host's thread, so the
 * adapter hands the outbound work to the seam through a sink that
 * takes the lock in `engine_control_inbox.hpp`.
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * @see engine_control_inbox.hpp — the lock + the pending queue.
 * @see docs/plan/remote-desktop-plan.md §Track G (G1, G2, G6).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include <nimrtc/core/time.hpp>
#include <nimrtc/plugins/base.hpp>
#include <nimrtc/plugins/control_channel.hpp>

namespace nimrtc::engine::detail {

// ---------------------------------------------------------------------------
// The statistics the engine publishes
// ---------------------------------------------------------------------------

/** The control channel's counters (the engine's public shape). */
struct ControlChannelStats {
    /** The messages the engine sent. */
    std::uint64_t messages_sent = 0;
    /** The messages the engine received and delivered. */
    std::uint64_t messages_received = 0;
    /** The payload bytes the engine sent. */
    std::uint64_t bytes_sent = 0;
    /** The payload bytes the engine received. */
    std::uint64_t bytes_received = 0;
    /** The outbound messages the queue refused (the queue was full). */
    std::uint64_t messages_dropped = 0;
    /** The inbound frames that were refused because they claimed a
     *  length over the configured limit. */
    std::uint64_t oversized_received = 0;
    /** The inbound frames that could not be parsed (a bad magic byte,
     *  an unknown version, a varint that does not terminate). */
    std::uint64_t malformed_received = 0;
    /** The heartbeats the engine received. */
    std::uint64_t heartbeats_received = 0;
    /** The heartbeats the engine sent. */
    std::uint64_t heartbeats_sent = 0;
};

// ---------------------------------------------------------------------------
// The seam-backed control channel
// ---------------------------------------------------------------------------

/**
 * @brief The engine's control channel, resolved through the seam.
 *
 * The class owns three things:
 *
 *   1. the resolved `plugins::IControlChannel` (the framing lives
 *      there — see the header's "Why the engine does not own the
 *      framing");
 *   2. the **outbound sink** the engine installs on it: the sink
 *      hands the framed bytes to the transport (the DTLS app-data
 *      seam or the control stack's raw-UDP path);
 *   3. the **heartbeat counters** the dead-man watchdog consumes.
 *
 * It does *not* own the transport: the engine tells it which
 * transport to use (see `NimRTCEngine::control_transport()`).
 */
class ControlChannelSeam {
public:
    ControlChannelSeam() = default;

    ControlChannelSeam(const ControlChannelSeam&)            = delete;
    ControlChannelSeam& operator=(const ControlChannelSeam&) = delete;

    /**
     * @brief Resolve the channel through the registry and open it.
     *
     * @param factory_id  the plugin id of the control-channel factory.
     * @param binding     the transport binding (the family, the sink and
     *                    the message limit).
     * @return `kOk` when the channel was resolved and opened;
     *         `kErrNotReady` when the factory id is unknown or the
     *         factory returned nothing.
     */
    plugins::Status resolve(std::string_view factory_id,
                            const plugins::ControlChannelBinding& binding) noexcept;

    /** True when a channel was resolved and opened. */
    [[nodiscard]] bool active() const noexcept { return channel_ != nullptr; }

    /** The resolved channel (nullptr when the plane is disabled). */
    [[nodiscard]] plugins::IControlChannel* channel() const noexcept {
        return channel_.get();
    }

    /**
     * @brief Install the callback that receives whole inbound messages.
     *
     * The callback runs synchronously inside `on_transport_bytes()`, on
     * whatever thread called it — so it must enqueue, not process.
     */
    void set_on_message(
        plugins::ControlMessageCallback cb) noexcept;

    /**
     * @brief Feed the bytes that arrived on the wire.
     *
     * A malformed frame is counted in `frames_rejected()` and never
     * allocates for an absurd length; the channel stays usable.
     */
    plugins::Status on_transport_bytes(std::span<const std::uint8_t> bytes) noexcept;

    /**
     * @brief Send one whole message.
     *
     * @return `kOk` when the message was accepted (framed and queued);
     *         `kErrInvalidParam` when the message was empty or over
     *         the configured limit; `kErrNotReady` when the control
     *         plane is disabled; `kErrBufferTooSmall` when the
     *         outbound queue is full.
     */
    plugins::Status send(std::span<const std::uint8_t> message) noexcept;

    /**
     * @brief Retry the messages the transport refused.
     *
     * The channel keeps the refused message queued, so a transport that
     * cannot take the bytes *now* does not lose them.
     */
    plugins::Status flush() noexcept;

    /**
     * @brief Send one liveness heartbeat (plan G6).
     *
     * The heartbeat's payload is the one byte the seam defines
     * (`plugins::kControlHeartbeatPayload`).
     */
    plugins::Status send_heartbeat() noexcept;

    // ---- The counters -------------------------------------------------------

    [[nodiscard]] const ControlChannelStats& stats() const noexcept {
        return stats_;
    }

    /** The instant the last heartbeat arrived (engine monotonic clock). */
    [[nodiscard]] std::optional<std::int64_t> last_heartbeat_us() const noexcept {
        return last_heartbeat_us_;
    }

    /**
     * @brief The transport the resolved channel rides.
     *
     * The value is fixed when the channel is resolved (the binding
     * carries it), and never changes afterwards — a channel's
     * transport is a property of the session, not of the moment.
     */
    [[nodiscard]] plugins::ControlChannelTransport
        transport() const noexcept { return transport_; }

    /** Reset the counters (the channel itself survives). */
    void reset_stats() noexcept;

    /** Close the channel and drop the resolved instance. */
    void close() noexcept;

private:
    /// The resolved channel (the framing lives there).
    std::unique_ptr<plugins::IControlChannel> channel_;

    /// The counters the engine publishes.
    ControlChannelStats stats_;

    /// The instant the last heartbeat arrived, in microseconds on the
    /// engine's monotonic clock.
    std::optional<std::int64_t> last_heartbeat_us_;

    /// The transport the resolved channel rides (plan G1 / G2).
    plugins::ControlChannelTransport transport_ =
        plugins::ControlChannelTransport::kDtlsAppData;
};

}  // namespace nimrtc::engine::detail
