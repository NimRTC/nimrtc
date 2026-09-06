/**
 * @file nimrtc/plugins/datachannel.hpp
 * @brief IDataChannel — pluggable data-channel interface (SCTP/PR-SCTP).
 *
 * IDataChannel provides unreliable-ordered bidirectional peer-to-peer
 * data channels on top of an ITransport (ICE + DTLS-SRTP).
 *
 * P1: interface + factory only, no concrete SCTP implementation.
 * P2: usrsctp-backed SCTP via IDataChannel.
 *
 * ## Channel semantics
 *
 * Each channel is configured with QoS semantics per §8.2:
 *   - Reliable ordered  : file transfer, structured logs
 *   - Unreliable unordered : high-frequency telemetry
 *   - Partial reliable (TTL) : teleop control commands (drop-old-on-limit)
 *   - Partial reliable (count) : media metadata with bounded retries
 *
 * ## Swapping the data-channel backend
 *
 * 1. Implement `IDataChannel` and `IDataChannelFactory`.
 * 2. Register: `PluginRegistry::instance().register_datachannel("sctp", factory);`
 * 3. (P2) Wire it into NimRTCEngine via its datachannel_name config field.
 *
 * @note P1 scaffold — interface stable, binary layout TBD P4.
 */

#include "nimrtc/plugins/base.hpp"

#ifndef NIMRTC_PLUGINS_DATACHANNEL_HPP
#define NIMRTC_PLUGINS_DATACHANNEL_HPP

#include <cstdint>
#include <functional>
#include <string_view>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// QoS semantics for each channel (PR-SCTP / SCTP)
// ---------------------------------------------------------------------------

/** Per-channel reliability and ordering semantics (RFC 8831 §6). */
enum class DataChannelReliability {
    kReliableOrdered,      // SCTP reliable ordered — file, structured logs
    kUnreliable,           // SCTP unordered, no reliability — telemetry
    kPartialReliableTTL,   // PR-SCTP: drop oldest when TTL expires
    kPartialReliableCount  // PR-SCTP: stop retransmitting after count
};

/** Configuration for a single data channel. */
struct DataChannelConfig {
    /** Human-readable label, e.g. "telemetry", "cmd", "file". */
    std::string label;

    /** Reliability and ordering semantics. */
    DataChannelReliability reliability = DataChannelReliability::kReliableOrdered;

    /** Maximum retransmit attempts (PR-SCTP count policy).
     *  0 = unlimited (only bounded by session timeout). */
    std::uint16_t max_retransmits = 0;

    /** Packet lifetime in milliseconds (PR-SCTP TTL policy).
     *  0 = unlimited. */
    std::uint16_t max_packet_lifetime_ms = 0;

    /** Scheduling priority. Lower value = higher priority.
     *  Control commands use low values (e.g. 16).
     *  Telemetry uses high values (e.g. 240). */
    std::uint8_t priority = 128;

    /** Whether messages must be delivered in order.
     *  False implies unordered delivery (SCTP unordered flag). */
    bool ordered = true;
};

// ---------------------------------------------------------------------------
// Callbacks (all noexcept per §8.6)
// ---------------------------------------------------------------------------

/** Invoked when a binary message arrives on this channel. */
using DataMessageCallback = std::function<void(BufferView msg)>;

/** Invoked when the channel state changes.
 *  State string is one of: "connecting", "open", "closing", "closed", "error". */
using DataChannelStateCallback = std::function<void(const char* state)>;

// ---------------------------------------------------------------------------
// IDataChannel interface
// ---------------------------------------------------------------------------

/** Bidirectional data-channel (RFC 8831).
 *
 * A channel is created by IDataChannelFactory::create(), configured via
 * open(), used via send(), and torn down via close().
 *
 * Thread safety:
 *   - set_on_message / set_on_state are called from a single thread
 *     (the engine thread) before open().
 *   - send() is thread-safe — implementations must queue outbound data.
 *   - on_message / on_state callbacks are dispatched from the engine tick.
 */
class IDataChannel : public IPlugin {
public:
    ~IDataChannel() override = default;

    // ------------------------------------------------------------------
    // IPlugin overrides
    // ------------------------------------------------------------------

    const char* name() const noexcept override = 0;
    Status      open() noexcept override = 0;
    void        close() noexcept override = 0;

    // ------------------------------------------------------------------
    // Channel lifecycle
    // ------------------------------------------------------------------

    /** Apply channel configuration after transport handshake completes.
     *  @param cfg  Channel parameters (label, QoS, priority).
     *  @note Called from engine thread. Implementations must be reentrant. */
    virtual void open(const DataChannelConfig& cfg) = 0;

    /** Send a binary message. Implementation queues according to QoS config.
     *  @param data  Payload bytes (any size up to SCTP limit).
     *  @note Thread-safe. */
    virtual void send(BufferView data) = 0;

    /** Set the binary message callback. Must be called before open().
     *  @note Not thread-safe — call only from the engine thread. */
    virtual void set_on_message(DataMessageCallback cb) noexcept = 0;

    /** Set the state-change callback. Must be called before open().
     *  @note Not thread-safe — call only from the engine thread. */
    virtual void set_on_state(DataChannelStateCallback cb) noexcept = 0;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

/** Factory for data-channel backend instances. */
class IDataChannelFactory : public IPluginFactory {
public:
    ~IDataChannelFactory() override = default;

    std::string_view id()          const noexcept override = 0;
    std::string_view display_name() const noexcept override = 0;

    /** Create a new channel instance. Ownership passes to caller. */
    virtual IDataChannel* create() const = 0;
};

/** Template helper — creates a factory that owns a channel type T. */
template<class T>
class SimpleDataChannelFactory : public IDataChannelFactory {
    std::string_view id_;
    std::string_view name_;
public:
    explicit SimpleDataChannelFactory(std::string_view id,
                                      std::string_view name) noexcept
        : id_(id), name_(name) {}

    std::string_view id()          const noexcept override { return id_; }
    std::string_view display_name() const noexcept override { return name_; }
    IDataChannel* create()          const override { return new T(); }
};

} // namespace nimrtc::plugins
#endif // NIMRTC_PLUGINS_DATACHANNEL_HPP
