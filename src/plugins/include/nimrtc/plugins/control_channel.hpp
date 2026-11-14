/**
 * @file nimrtc/plugins/control_channel.hpp
 * @brief IControlChannel — the remote-desktop control-channel seam
 *        (remote-desktop plan **G1** / **G2**).
 *
 * ## What this seam is for
 *
 * A remote-desktop session has two very different kinds of traffic:
 *
 *   - the **data plane** — a 15 Mbps video stream, which is allowed to
 *     be late;
 *   - the **control plane** — the operator's mouse moves, key presses
 *     and the heartbeat that says the operator is still there.  Every
 *     millisecond of delay here is a millisecond of "the remote cursor
 *     did not move".
 *
 * The control traffic therefore needs its own path, and plan Track G
 * names two:
 *
 *   - **G1** — a NimRTC-native control channel over the **DTLS
 *     application-data seam**.  The DTLS session already exposes
 *     `send_app_data()` / `take_app_data()`, and the record layer keeps
 *     the record boundaries intact, so the channel costs *no* transport
 *     work.  This is the interim path.
 *   - **G2** — the same channel riding the transport session's
 *     **control stack** (its raw-UDP datagram path), so the control
 *     plane has its own ICE-selected pair and therefore its own
 *     congestion-control domain.
 *
 * This header is the **seam** the engine drives.  The engine does not
 * know whether the bytes it hands over travel over DTLS records or
 * over raw UDP; it knows only that the channel is **message
 * oriented**.
 *
 * ## Message orientation — the one property that matters
 *
 * The underlying transports differ in what they preserve:
 *
 *   - a **DataChannel** preserves message boundaries (one `send()` is
 *     one inbound message) — but only in the *framed* mode; the raw
 *     mode is a byte stream;
 *   - the **DTLS record layer** preserves *record* boundaries, not
 *     *message* boundaries: one record may carry several messages, and
 *     one message may be split across several records;
 *   - **raw UDP** preserves *datagram* boundaries, which is the
 *     strongest guarantee of the three — but a datagram that exceeds
 *     the path MTU is lost, so the channel still frames.
 *
 * So the channel is defined as a **message-oriented** abstraction that
 * takes bytes in and gives whole messages out, and the framing that
 * makes that true lives in the *implementation* (the `rd_input`
 * module's `ControlChannel`), not in this interface.  The interface
 * only says: "hand me a message; give me a message".
 *
 * ## What the payload means is NOT this seam's business
 *
 * The interface carries **opaque bytes**.  The *payload format* is the
 * input module's business (plan G3's compact event encoding), and the
 * *policy* around it — the dead-man switch, the heartbeat, the
 * input-to-frame binding — is the host's business.  A control channel
 * that understood mouse events would be a much worse seam than one
 * that does not.
 *
 * ## Threading
 *
 * Not thread-safe.  The engine's tick thread is the only driver: it
 * calls `send_control_message()` and feeds the inbound bytes.  The
 * host's inbound callback therefore runs on the engine's thread, and it
 * should enqueue rather than process.
 *
 * @see docs/plan/remote-desktop-plan.md §Track G (G1, G2).
 */

#ifndef NIMRTC_PLUGINS_CONTROL_CHANNEL_HPP
#define NIMRTC_PLUGINS_CONTROL_CHANNEL_HPP

// Stability classification: NIMRTC_API_STABLE (file-level marker from the
// v0.13.0 sweep; see ADR-015 / docs/plan/v0.13-plan.md).
//
// `nimrtc_plugins` links `nimrtc::core` PUBLIC (see src/plugins/CMakeLists.txt),
// so <nimrtc/core/api_version.hpp> is reachable from this header.
#include <nimrtc/core/api_version.hpp>   // NIMRTC_API_STABLE

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "nimrtc/plugins/base.hpp"

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// The transport a control channel rides
// ---------------------------------------------------------------------------

/**
 * @brief The transport family a control channel is built on.
 *
 * The enum names the *wire path*, not the payload: every transport
 * carries the same opaque bytes.
 */
enum class NIMRTC_API_STABLE ControlChannelTransport : std::uint8_t {
    /** The DTLS application-data seam (plan G1).  The default. */
    kDtlsAppData = 0,
    /** A DataChannel (the plan's G3 transport; the browser-interoperable
     *  path — see G1b for the DTLS-SCTP association work that path
     *  needs). */
    kDataChannel = 1,
    /** The control stack's raw-UDP datagram path (plan G2). */
    kRawUdp = 2,
    /** Let the host pick: the raw-UDP control path when the session has
     *  one, the DTLS app-data seam otherwise.
     *
     *  The *host* picks, not the channel — the channel is created after
     *  the transport session exists, so the choice is made once, when
     *  the channel is created, and never changes afterwards. */
    kAuto = 3,
};

/** Human-readable transport name (for logs and diagnostics). */
NIMRTC_API_STABLE inline const char* control_channel_transport_name(
    ControlChannelTransport t) noexcept {
    switch (t) {
        case ControlChannelTransport::kDtlsAppData: return "dtls-app-data";
        case ControlChannelTransport::kDataChannel:  return "data-channel";
        case ControlChannelTransport::kRawUdp:       return "raw-udp";
        case ControlChannelTransport::kAuto:         return "auto";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// The liveness heartbeat (plan G6)
// ---------------------------------------------------------------------------

/**
 * @brief The payload of a control-plane liveness heartbeat.
 *
 * A heartbeat is a control message whose payload is **exactly this one
 * byte**.  The convention lives here — in the seam — because both ends
 * of the control plane have to agree on it, and because the engine has
 * to be able to recognise a heartbeat without knowing the payload
 * format of the control plane (the input module's business).
 *
 * The heartbeat carries no information beyond its existence: the
 * *dead-man watchdog* (plan G6) in the `rd_input` module interprets the
 * arrival of a heartbeat, and the safe state it triggers is the host's
 * business.  The engine only counts the heartbeats and publishes the
 * instant of the last one, so a host can wire the watchdog without the
 * engine having to know what a heartbeat means.
 */
inline constexpr NIMRTC_API_STABLE std::uint8_t kControlHeartbeatPayload = 0x01;

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

/**
 * @brief The callback that receives one whole inbound control message.
 *
 * The span is valid only for the duration of the call: the channel owns
 * the buffer and reclaims it as soon as the callback returns.  A host
 * that needs to keep the bytes must copy them.
 */
using NIMRTC_API_STABLE ControlMessageCallback =
    std::function<void(core::ByteSpan message)>;

/**
 * @brief The outbound byte sink.
 *
 * The channel *frames* a message and hands the framed bytes to this
 * sink; the sink is whatever transport the channel rides.  Returning a
 * non-zero status tells the channel that the bytes were **not**
 * accepted — the channel then keeps the message queued and retries on
 * the next opportunity.  This is what makes the channel lossless over
 * a transport that cannot always take bytes (a DTLS session that is
 * not connected yet, a full raw-UDP queue).
 */
using NIMRTC_API_STABLE ControlByteSink =
    std::function<Status(core::ByteSpan bytes)>;

// ---------------------------------------------------------------------------
// IControlChannel
// ---------------------------------------------------------------------------

/**
 * @brief A message-oriented control channel.
 *
 * The contract is deliberately small:
 *
 *   - `send_control_message()` hands one **whole message** to the
 *     channel;
 *   - `set_on_control_message()` installs the callback that receives
 *     one **whole message** at a time;
 *   - `on_transport_bytes()` feeds the bytes that arrived on the wire
 *     (which may be a partial message, several messages, or a mixture),
 *     and the channel reassembles whatever it needs.
 *
 * The counters make the path observable: a control plane that silently
 * drops the operator's clicks is worse than no control plane at all.
 */
class NIMRTC_API_STABLE IControlChannel : public IPlugin {
public:
    ~IControlChannel() override = default;

    // ---- IPlugin overrides -------------------------------------------------

    NIMRTC_API_STABLE const char* name() const noexcept override = 0;
    NIMRTC_API_STABLE Status      open() noexcept override = 0;
    NIMRTC_API_STABLE void        close() noexcept override = 0;

    // ---- The message surface ------------------------------------------------

    /**
     * @brief Send one control message.
     *
     * @param bytes  The message.  Must not be empty.
     * @return `kOk` when the message was accepted (framed and queued);
     *         `kErrInvalidParam` when the message was empty or over the
     *         configured limit; `kErrNotReady` when the channel is
     *         closed; and `kErrBufferTooSmall` when the outbound queue
     *         is full (the peer is not draining the control path).
     *
     * @note A message that the outbound sink refuses is **not** an
     *       error here: the channel keeps it queued and retries it on
     *       the next `flush_control_messages()`.  A control message must
     *       not be lost, so the *sink* reports the refusal, not the
     *       send.
     */
    NIMRTC_API_STABLE virtual Status send_control_message(
        core::ByteSpan bytes) noexcept = 0;

    /**
     * @brief Install the callback that receives whole inbound messages.
     *
     * The callback runs synchronously inside `on_transport_bytes()`,
     * on whatever thread called it.  It should enqueue rather than
     * process.
     */
    NIMRTC_API_STABLE virtual void set_on_control_message(
        ControlMessageCallback cb) noexcept = 0;

    /**
     * @brief Feed the bytes that arrived on the wire.
     *
     * The bytes may be a partial message, several messages, or a
     * message boundary that does not align with the transport's.  The
     * channel reassembles whatever it needs and delivers whole
     * messages to the callback.
     *
     * A malformed frame (a bad magic byte, an absurd length) is
     * **rejected**: the channel counts it in `frames_rejected()` and
     * never allocates for it.
     */
    NIMRTC_API_STABLE virtual Status on_transport_bytes(
        core::ByteSpan bytes) noexcept = 0;

    /**
     * @brief Install the outbound sink.
     *
     * The channel frames each message and hands the framed bytes to the
     * sink.  A sink that returns a non-zero status causes the channel
     * to keep the message queued for a later retry.
     */
    NIMRTC_API_STABLE virtual void set_outbound_sink(
        ControlByteSink sink) noexcept = 0;

    /**
     * @brief Retry the outbound messages the sink has refused.
     *
     * A sink that could not take the bytes *now* leaves the message
     * queued inside the channel, so the host must ask for a retry.  The
     * engine does this on its tick thread, which is also the only
     * thread on which the transport may be touched.
     *
     * @return `kOk` when nothing is left queued; a transport-specific
     *         status when the sink still refuses the bytes.
     */
    NIMRTC_API_STABLE virtual Status flush_control_messages() noexcept = 0;

    // ---- The counters -------------------------------------------------------

    /** The messages the channel handed to the outbound sink. */
    NIMRTC_API_STABLE virtual std::uint64_t messages_sent() const noexcept = 0;
    /** The messages the channel delivered to the inbound callback. */
    NIMRTC_API_STABLE virtual std::uint64_t messages_received() const noexcept = 0;
    /** The payload bytes the channel sent (framing excluded). */
    NIMRTC_API_STABLE virtual std::uint64_t bytes_sent() const noexcept = 0;
    /** The payload bytes the channel received (framing excluded). */
    NIMRTC_API_STABLE virtual std::uint64_t bytes_received() const noexcept = 0;
    /** The frames the channel refused (a bad magic byte, an absurd
     *  length, a length that does not fit the configured limit). */
    NIMRTC_API_STABLE virtual std::uint64_t frames_rejected() const noexcept = 0;
    /** The frames the channel refused because they claimed a length
     *  over the configured limit.
     *
     *  A separate counter from `frames_rejected()` because it is a
     *  *different* failure: the sender is misbehaving (it framed a
     *  message the receiver cannot accept), while a malformed frame is
     *  a corrupt stream. */
    NIMRTC_API_STABLE virtual std::uint64_t frames_oversized() const noexcept = 0;
    /** The frames the channel refused because they were malformed (a bad
     *  magic byte, an unknown version, a varint that does not
     *  terminate). */
    NIMRTC_API_STABLE virtual std::uint64_t frames_malformed() const noexcept = 0;
    /** The messages the outbound queue refused (the queue was full).
     *
     *  A non-zero value means the peer is not draining the control
     *  path — the operator's clicks are being lost, and that is worth
     *  surfacing rather than hiding. */
    NIMRTC_API_STABLE virtual std::uint64_t messages_dropped() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// The binding
// ---------------------------------------------------------------------------

/**
 * @brief How a control channel is bound to its transport.
 *
 * The factory receives this and builds the channel accordingly.  The
 * transport-specific parts (the DTLS session, the data channel, the
 * raw-UDP path) are supplied through the *sink* / the feed, so the
 * binding stays a small, transport-agnostic value.
 */
struct NIMRTC_API_STABLE ControlChannelBinding {
    /** The transport family.  Defaults to the DTLS app-data seam. */
    ControlChannelTransport transport = ControlChannelTransport::kDtlsAppData;

    /** The outbound sink.  An empty sink makes the channel a sink-only
     *  (inbound-only) channel. */
    ControlByteSink sink;

    /** The largest message the channel will send or accept, in bytes.
     *
     *  A message over the limit is refused on the send side and
     *  rejected on the receive side — never truncated (a truncated
     *  input event is a *wrong* input event, which is worse than a
     *  missing one). */
    std::size_t max_message_bytes = 65536;
};

// ---------------------------------------------------------------------------
// The factory
// ---------------------------------------------------------------------------

/**
 * @brief The factory that produces control channels.
 *
 * The engine resolves a factory through the registry's control-channel
 * slot and creates one channel from it.  The concrete implementation
 * lives in the `rd_input` module (the input module's natural home for
 * the control plane — plan G3 decided the payload format is the input
 * module's business).
 */
class NIMRTC_API_STABLE IControlChannelFactory : public IPluginFactory {
public:
    ~IControlChannelFactory() override = default;

    NIMRTC_API_STABLE std::string_view id()          const noexcept override = 0;
    NIMRTC_API_STABLE std::string_view display_name() const noexcept override = 0;

    /** Create one control channel.  Ownership passes to the caller. */
    NIMRTC_API_STABLE virtual std::unique_ptr<IControlChannel> create(
        const ControlChannelBinding& binding) const = 0;
};

} // namespace nimrtc::plugins

#endif // NIMRTC_PLUGINS_CONTROL_CHANNEL_HPP
