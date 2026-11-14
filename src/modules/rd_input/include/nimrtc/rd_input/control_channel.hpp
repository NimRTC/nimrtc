/**
 * @file nimrtc/rd_input/control_channel.hpp
 * @brief The NimRTC-native control channel (remote-desktop plan **G1** /
 *        **G2**) — the module-side implementation.
 *
 * ## What this is
 *
 * The **mechanism** behind plan G1's "NimRTC-native control channel over
 * the DTLS app-data seam" and plan G2's "control plane on its own
 * transport".  The mechanism is deliberately tiny and entirely
 * transport-agnostic:
 *
 *   - it **frames** a message so the message boundary survives a
 *     transport that does not preserve one;
 *   - it **reassembles** the frames that arrive across several
 *     transport records;
 *   - it **rejects** a malformed frame cleanly, without ever
 *     allocating for an absurd length;
 *   - it **counts** what it did, so the control plane is observable.
 *
 * The *payload* is opaque: the input module's own event codec (plan
 * G3) is what defines what a message means, and the engine only moves
 * the bytes.
 *
 * ## The framing
 *
 * ```
 *   byte 0      : the magic byte (0xCC)
 *   byte 1      : the format version (1)
 *   byte 2..    : the payload length, a LEB128 varint
 *   byte N..    : the payload
 * ```
 *
 * Three properties made this shape the choice:
 *
 *   1. **A magic byte.**  The channel may be multiplexed onto a byte
 *      transport that carries other things (the raw-UDP control path
 *      shares the stack's datagram path).  A magic byte lets the
 *      receiver reject a foreign datagram instead of mis-parsing it.
 *   2. **A version byte.**  The framing may change (a new field, a
 *      different varint encoding).  The version is the second byte so
 *      a receiver can tell the two apart without a side channel.
 *   3. **A varint length.**  Most control messages are tiny (a mouse
 *      delta is a handful of bytes), so a fixed 4-byte length would
 *      quadruple the cost of the common case.  A LEB128 varint costs
 *      one byte below 128.
 *
 * The length is a varint rather than a fixed width *because* the
 * maximum is bounded (64 KiB by default), so the varint is at most
 * three bytes.
 *
 * ## Why the receiver cannot be tricked into a huge allocation
 *
 * The decoder reads the length **before** it reserves anything, and it
 * compares the length against the configured limit
 * (`max_message_bytes`).  A frame that claims more than the limit is
 * rejected outright: the counter moves, no buffer grows, and the
 * channel stays usable.  A varint that is longer than the maximum
 * possible (or that runs off the end of the input) is a malformed
 * frame, not a value.
 *
 * ## Threading
 *
 * Not thread-safe.  One channel belongs to one control path, and the
 * path is serialised by the engine's tick thread (the same rule the
 * coalescer and the ref-frame binder obey).
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/plugins/base.hpp>
#include <nimrtc/plugins/control_channel.hpp>

namespace nimrtc::rd_input {

// ---------------------------------------------------------------------------
// The framing constants
// ---------------------------------------------------------------------------

/** The byte that opens every control frame. */
inline constexpr NIMRTC_API_STABLE std::uint8_t kControlFrameMagic = 0xCC;

/** The framing version this build writes. */
inline constexpr NIMRTC_API_STABLE std::uint8_t kControlFrameVersion = 1;

/** The smallest framing version this build understands. */
inline constexpr NIMRTC_API_STABLE std::uint8_t kMinControlFrameVersion = 1;

/** The largest framing version this build understands. */
inline constexpr NIMRTC_API_STABLE std::uint8_t kMaxControlFrameVersion = 1;

/** The default ceiling on one control message, in bytes (64 KiB). */
inline constexpr NIMRTC_API_STABLE std::size_t kDefaultMaxControlMessageBytes =
    64 * 1024;

// ---------------------------------------------------------------------------
// ControlChannel
// ---------------------------------------------------------------------------

/**
 * @brief The framing / reassembly half of the control channel.
 *
 * The class is transport-agnostic: it never touches a socket.  The
 * *bytes* go out through the `plugins::ControlByteSink` the host
 * installs, and the *bytes* come in through `feed_inbound()`.  That is
 * what makes the same implementation serve the DTLS app-data path
 * (G1), the DataChannel path (G3) and the raw-UDP control path (G2).
 *
 * The channel is **message oriented**: `send()` takes one whole
 * message and `feed_inbound()` gives back one whole message at a time.
 */
class NIMRTC_API_STABLE ControlChannel {
public:
    /** The callback that receives one whole inbound message. */
    using MessageCallback = std::function<void(core::ByteSpan)>;

    /** The outbound byte sink.  A non-zero return means "not accepted";
     *  the channel then keeps the message queued for a later retry. */
    using ByteSink = std::function<plugins::Status(core::ByteSpan)>;

    ControlChannel() = default;

    /**
     * @brief Construct a channel.
     *
     * @param max_message_bytes  The largest message the channel will
     *                           send or accept.  0 means
     *                           `kDefaultMaxControlMessageBytes`.
     */
    explicit ControlChannel(std::size_t max_message_bytes);

    /**
     * @brief Set the maximum message size.
     *
     * A limit of 0 means the default.  A limit below one is raised to
     * one: a channel that cannot carry a single byte is useless.
     */
    void set_max_message_bytes(std::size_t max_message_bytes) noexcept;

    /** The effective maximum message size, in bytes. */
    [[nodiscard]] std::size_t max_message_bytes() const noexcept {
        return max_message_bytes_;
    }

    /**
     * @brief Install the callback that receives whole inbound messages.
     */
    void set_on_message(MessageCallback cb) noexcept;

    /**
     * @brief Install the outbound byte sink.
     *
     * The channel frames the message and hands the framed bytes to the
     * sink.  An empty sink means "no transport yet": the message is
     * still framed and queued, and it goes out when a sink is
     * installed.
     */
    void set_outbound_sink(ByteSink sink) noexcept;

    /**
     * @brief Queue one message for sending.
     *
     * The message is framed and handed to the outbound sink.  When the
     * sink refuses the bytes, the framed message stays at the head of
     * the queue and is retried on the next `flush()`.
     *
     * @return `kOk` when the message was accepted by the sink (or
     *         queued because there is no sink yet); `kErrInvalidParam`
     *         when the message was empty or over the limit;
     *         `kErrBufferTooSmall` when the sink refused the bytes and
     *         the queue is full.
     */
    plugins::Status send(core::ByteSpan message) noexcept;

    /**
     * @brief Retry the queued messages.
     *
     * The channel keeps at most `kQueueDepth` framed messages.  The
     * queue is bounded on purpose: a control path that queues without
     * bound turns a congested link into a memory leak.
     */
    plugins::Status flush() noexcept;

    /**
     * @brief Feed the bytes that arrived on the wire.
     *
     * The bytes may be a partial message, several messages, or a
     * mixture.  The channel reassembles whatever it needs and delivers
     * whole messages to the callback.
     *
     * @return `kOk` when the bytes were consumed; `kErrCorrupt` when a
     *         frame was malformed and the channel had to drop the
     *         inbound buffer (the stream cannot be resynchronised).
     */
    plugins::Status feed_inbound(core::ByteSpan bytes) noexcept;

    // ---- The counters -------------------------------------------------------

    /** The messages handed to the outbound sink. */
    [[nodiscard]] std::uint64_t messages_sent() const noexcept {
        return messages_sent_;
    }
    /** The messages delivered to the inbound callback. */
    [[nodiscard]] std::uint64_t messages_received() const noexcept {
        return messages_received_;
    }
    /** The payload bytes sent (framing excluded). */
    [[nodiscard]] std::uint64_t bytes_sent() const noexcept {
        return bytes_sent_;
    }
    /** The payload bytes received (framing excluded). */
    [[nodiscard]] std::uint64_t bytes_received() const noexcept {
        return bytes_received_;
    }
    /** The frames the channel refused. */
    [[nodiscard]] std::uint64_t frames_rejected() const noexcept {
        return frames_rejected_;
    }
    /** The frames refused because they claimed an over-long length. */
    [[nodiscard]] std::uint64_t frames_oversized() const noexcept {
        return frames_oversized_;
    }
    /** The frames refused because they were malformed (a bad magic
     *  byte, an unknown version, a varint that does not terminate). */
    [[nodiscard]] std::uint64_t frames_malformed() const noexcept {
        return frames_malformed_;
    }
    /** The messages the outbound queue refused (the queue was full). */
    [[nodiscard]] std::uint64_t messages_dropped() const noexcept {
        return messages_dropped_;
    }
    /** The number of framed messages waiting for the sink. */
    [[nodiscard]] std::size_t queued() const noexcept { return outbox_.size(); }

    /** Drop the queued messages and the inbound reassembly buffer. */
    void clear() noexcept;

    // ---- The framing helpers (exposed for the tests) ----------------------

    /**
     * @brief Frame one message.
     *
     * @return `kOk` and the framed bytes, or `kErrInvalidParam` when
     *         the message was empty or over the limit.
     */
    static plugins::Status frame_message(
        core::ByteSpan message, std::size_t max_message_bytes,
        std::vector<std::uint8_t>& out) noexcept;

    /**
     * @brief The number of bytes the framing of `message` occupies.
     *
     * Returns 0 when the message cannot be framed (empty, or over the
     * limit).
     */
    static std::size_t framed_size(core::ByteSpan message,
                                   std::size_t max_message_bytes) noexcept;

private:
    /// Deliver one whole message to the callback.
    void deliver(core::ByteSpan message) noexcept;

    /// The limit on one message.
    std::size_t max_message_bytes_ = kDefaultMaxControlMessageBytes;

    /// The inbound callback.
    MessageCallback on_message_;

    /// The outbound sink.
    ByteSink sink_;

    /// The framed messages waiting for the sink, each paired with its
    /// payload size (the framing overhead is excluded from the byte
    /// counters, so the counter answers "how much payload moved").
    std::deque<std::pair<std::vector<std::uint8_t>, std::size_t>> outbox_;

    /// The inbound reassembly buffer.
    std::vector<std::uint8_t> inbox_;

    std::uint64_t messages_sent_ = 0;
    std::uint64_t messages_received_ = 0;
    std::uint64_t bytes_sent_ = 0;
    std::uint64_t bytes_received_ = 0;
    std::uint64_t frames_rejected_ = 0;
    std::uint64_t frames_oversized_ = 0;
    std::uint64_t frames_malformed_ = 0;
    std::uint64_t messages_dropped_ = 0;
};

// ---------------------------------------------------------------------------
// The plugin adapter
// ---------------------------------------------------------------------------

/**
 * @brief The `plugins::IControlChannel` adapter over `ControlChannel`.
 *
 * The adapter is what the engine drives: it holds the framing channel
 * and forwards the seam's calls into it.
 */
class NIMRTC_API_STABLE PluginControlChannel final
    : public plugins::IControlChannel {
public:
    explicit PluginControlChannel(
        const plugins::ControlChannelBinding& binding);
    ~PluginControlChannel() override = default;

    // ---- IPlugin ------------------------------------------------------------
    const char* name() const noexcept override;
    plugins::Status open() noexcept override;
    void close() noexcept override;

    // ---- IControlChannel ----------------------------------------------------
    plugins::Status send_control_message(
        core::ByteSpan bytes) noexcept override;
    void set_on_control_message(
        plugins::ControlMessageCallback cb) noexcept override;
    plugins::Status on_transport_bytes(
        core::ByteSpan bytes) noexcept override;
    void set_outbound_sink(plugins::ControlByteSink sink) noexcept override;
    plugins::Status flush_control_messages() noexcept override;

    std::uint64_t messages_sent() const noexcept override;
    std::uint64_t messages_received() const noexcept override;
    std::uint64_t bytes_sent() const noexcept override;
    std::uint64_t bytes_received() const noexcept override;
    std::uint64_t frames_rejected() const noexcept override;
    std::uint64_t frames_oversized() const noexcept override;
    std::uint64_t frames_malformed() const noexcept override;
    std::uint64_t messages_dropped() const noexcept override;

    /** The transport family this channel was built for. */
    [[nodiscard]] plugins::ControlChannelTransport transport() const noexcept {
        return binding_.transport;
    }

    /** The underlying framing channel (for the tests). */
    [[nodiscard]] ControlChannel& channel() noexcept { return channel_; }

private:
    plugins::ControlChannelBinding binding_;
    ControlChannel channel_;
    bool open_ = false;
};

/**
 * @brief The factory that produces `PluginControlChannel` instances.
 *
 * Registered under the id `"control-channel"` in the registry's
 * control-channel slot, so the engine resolves it through
 * `core::PluginRegistry::get_control_channel(...)`.
 */
class NIMRTC_API_STABLE ControlChannelFactory final
    : public plugins::IControlChannelFactory {
public:
    std::string_view id() const noexcept override;
    std::string_view display_name() const noexcept override;

    std::unique_ptr<plugins::IControlChannel> create(
        const plugins::ControlChannelBinding& binding) const override;

    /** The factory id under which the module registers itself. */
    static constexpr const char* kBackendId = "control-channel";
};

} // namespace nimrtc::rd_input
