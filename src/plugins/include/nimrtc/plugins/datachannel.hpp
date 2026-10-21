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
 * ## Per-symbol stability annotations (v0.14.x PER-SYMBOL-CLASSIFY)
 *
 * The v0.13.0 sweep marked this header at the file level. The v0.14.x
 * per-symbol follow-up annotates each `IDataChannel` member:
 *
 *   - `name()` / `open()` / `close()` — STABLE. These are the `IPlugin`
 *     overrides every backend implements; the engine's create/dispatch
 *     path goes through them.
 *   - `IDataChannel::open(cfg)` / `send()` / `set_on_message()` /
 *     `set_on_state()` — STABLE. The four-member channel contract is
 *     the load-bearing data-plane seam. Two concrete impls consume it
 *     in-tree: `SctpDataChannel` (id `"sctp"`) and
 *     `WebTransportDataChannel` (id `"web_transport"`, landed
 *     v0.14.0-beta.1). A v1.0.x patch changing these signatures would
 *     break both, so they are frozen.
 *   - `IDataChannelFactory::id()` / `display_name()` / `create()` —
 *     STABLE. Registry lookups key off `id()`.
 *   - `SimpleDataChannelFactory` — STABLE. Template helper used by the
 *     in-tree factories.
 *
 * @note P1 scaffold — interface stable, binary layout TBD P4.
 */

#include "nimrtc/plugins/base.hpp"

#ifndef NIMRTC_PLUGINS_DATACHANNEL_HPP
#define NIMRTC_PLUGINS_DATACHANNEL_HPP

// Stability classification: NIMRTC_API_STABLE (file-level marker from the
// v0.13.0 sweep; see ADR-015 / docs/plan/v0.13-plan.md). The v0.14.x
// per-symbol follow-up annotates every `IDataChannel` member below; see the
// "Per-symbol stability annotations" note in the header comment above.
//
// `nimrtc_plugins` links `nimrtc::core` PUBLIC (see src/plugins/CMakeLists.txt),
// so <nimrtc/core/api_version.hpp> is reachable from this header.
#include <nimrtc/core/api_version.hpp>   // NIMRTC_API_STABLE

#include <cstdint>
#include <functional>
#include <string_view>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// QoS semantics for each channel (PR-SCTP / SCTP)
// ---------------------------------------------------------------------------

/** Per-channel reliability and ordering semantics (RFC 8831 §6). */
enum class NIMRTC_API_STABLE DataChannelReliability {
    kReliableOrdered,      // SCTP reliable ordered — file, structured logs
    kUnreliable,           // SCTP unordered, no reliability — telemetry
    kPartialReliableTTL,   // PR-SCTP: drop oldest when TTL expires
    kPartialReliableCount  // PR-SCTP: stop retransmitting after count
};

/** Configuration for a single data channel. */
struct NIMRTC_API_STABLE DataChannelConfig {
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
using NIMRTC_API_STABLE DataMessageCallback = std::function<void(BufferView msg)>;

/** Invoked when the channel state changes.
 *  State string is one of: "connecting", "open", "closing", "closed", "error". */
using NIMRTC_API_STABLE DataChannelStateCallback = std::function<void(const char* state)>;

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
 *
 * ## Per-symbol stability annotations (v0.14.x PER-SYMBOL-CLASSIFY)
 *
 * The file-level header comment carries the full list.  In brief:
 *   - `name()` / `open()` / `close()` — STABLE (IPlugin overrides).
 *   - `open(cfg)` / `send()` / `set_on_message()` / `set_on_state()` — STABLE.
 *     The four-member channel contract is the load-bearing data-plane seam;
 *     both in-tree backends (`SctpDataChannel` id `"sctp"` and
 *     `WebTransportDataChannel` id `"web_transport"`, v0.14.0-beta.1) consume it.
 *   - `IDataChannelFactory::id()` / `display_name()` / `create()` — STABLE.
 *   - `SimpleDataChannelFactory` — STABLE (template helper). */
// STABLE: the four-member contract (`open` / `send` / `set_on_message` /
// `set_on_state`) plus the three IPlugin overrides is the load-bearing
// data-plane seam. Two concrete impls consume it in-tree — `SctpDataChannel`
// (id `"sctp"`) and `WebTransportDataChannel` (id `"web_transport"`, landed
// v0.14.0-beta.1). A v1.0.x patch changing any signature below breaks both.
class NIMRTC_API_STABLE IDataChannel : public IPlugin {
public:
    ~IDataChannel() override = default;

    // ------------------------------------------------------------------
    // IPlugin overrides
    // ------------------------------------------------------------------

    NIMRTC_API_STABLE const char* name() const noexcept override = 0;
    NIMRTC_API_STABLE Status      open() noexcept override = 0;
    NIMRTC_API_STABLE void        close() noexcept override = 0;

    // ------------------------------------------------------------------
    // Channel lifecycle
    // ------------------------------------------------------------------

    /** Apply channel configuration after transport handshake completes.
     *  @param cfg  Channel parameters (label, QoS, priority).
     *  @note Called from engine thread. Implementations must be reentrant. */
    NIMRTC_API_STABLE virtual void open(const DataChannelConfig& cfg) = 0;

    /** Send a binary message. Implementation queues according to QoS config.
     *  @param data  Payload bytes (any size up to SCTP limit).
     *  @note Thread-safe. */
    NIMRTC_API_STABLE virtual void send(BufferView data) = 0;

    /** Set the binary message callback. Must be called before open().
     *  @note Not thread-safe — call only from the engine thread. */
    NIMRTC_API_STABLE virtual void set_on_message(DataMessageCallback cb) noexcept = 0;

    /** Set the state-change callback. Must be called before open().
     *  @note Not thread-safe — call only from the engine thread. */
    NIMRTC_API_STABLE virtual void set_on_state(DataChannelStateCallback cb) noexcept = 0;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

/** Factory for data-channel backend instances.
 *
 *  STABLE: the factory pattern (id / display_name / create) is the
 *  engine-facing seam for backend substitution. `SctpDataChannelFactory`
 *  (id `"sctp"`) and `WebTransportDataChannelFactory` (id `"web_transport"`)
 *  are the two concrete impls; both use this interface. Marking STABLE so
 *  the registry key identity is frozen. */
class NIMRTC_API_STABLE IDataChannelFactory : public IPluginFactory {
public:
    ~IDataChannelFactory() override = default;

    NIMRTC_API_STABLE std::string_view id()          const noexcept override = 0;
    NIMRTC_API_STABLE std::string_view display_name() const noexcept override = 0;

    /** Create a new channel instance. Ownership passes to caller.
     *  STABLE: factory->create() is the one-step channel allocation path;
     *  the engine calls it from create_data_channel(). */
    NIMRTC_API_STABLE virtual IDataChannel* create() const = 0;
};

/** Template helper — creates a factory that owns a channel type T.
 *
 *  STABLE: the most common in-tree factory implementation pattern. */
template<class T>
class NIMRTC_API_STABLE SimpleDataChannelFactory : public IDataChannelFactory {
    std::string_view id_;
    std::string_view name_;
public:
    explicit SimpleDataChannelFactory(std::string_view id,
                                      std::string_view name) noexcept
        : id_(id), name_(name) {}

    NIMRTC_API_STABLE std::string_view id()          const noexcept override { return id_; }
    NIMRTC_API_STABLE std::string_view display_name() const noexcept override { return name_; }
    NIMRTC_API_STABLE IDataChannel* create()          const override { return new T(); }
};

} // namespace nimrtc::plugins
#endif // NIMRTC_PLUGINS_DATACHANNEL_HPP
