/**
 * @file nimrtc/datachannel/sctp_data_channel.hpp
 * @brief SctpDataChannel — usrsctp-backed implementation of IDataChannel.
 *
 * P2 of the datachannel module (P1 was interface-only). This header
 * is the public module surface for the production channel; concrete
 * behaviour lives in `sctp_data_channel.cpp`.
 *
 * ## Thread model
 *
 * The engine owns one `IDataChannel*` per logical channel (e.g. one for
 * "telemetry", one for "cmd"). Per `plugins::IDataChannel`'s contract:
 *
 *   - `set_on_message` / `set_on_state` are called from the engine thread
 *     before `open(cfg)` is invoked. Not thread-safe — call from a single
 *     thread.
 *   - `send()` is thread-safe — implementations must queue outbound data.
 *     SctpDataChannel forwards directly to the underlying ISctpSocket,
 *     which (via usrsctp) is thread-safe for outbound writes.
 *   - The on-message / on-state callbacks fire from the SCTP backend's
 *     worker thread (usrsctp's internal UDP-loop). SctpDataChannel
 *     dispatches them onto `on_msg_` / `on_state_` directly — the user
 *     is responsible for marshalling into the engine thread if required.
 *
 * ## QoS → SCTP routing
 *
 * `DataChannelReliability` maps onto the three `ISctpSocket` send
 * methods:
 *
 *   | reliability           | routed to                |
 *   |-----------------------|--------------------------|
 *   | kReliableOrdered      | send_stream()            |
 *   | kUnreliable           | send_datagram()          |
 *   | kPartialReliableTTL   | send_partial_reliable(ttl=cfg.max_packet_lifetime_ms) |
 *   | kPartialReliableCount | send_partial_reliable(ttl=max_retransmits * RTT_EST) |
 *
 * Stream id: the current revision hard-codes `stream = 0`. v0.12 will
 * derive a stream id from `cfg.priority` so high-priority control
 * traffic goes on a separate stream from bulk telemetry.
 *
 * ## Test injection
 *
 * `SctpDataChannel(std::unique_ptr<sctp::ISctpSocket>)` is a public
 * constructor intended for tests only — it skips the
 * `PluginRegistry::get_sctp_socket("usrsctp")` lookup and accepts a
 * caller-supplied socket (typically a MockSctpSocket). Production
 * code MUST use the default constructor.
 *
 * @note P2 — interface stable; binary layout TBD P4.
 */

#ifndef NIMRTC_DATACHANNEL_SCTP_DATA_CHANNEL_HPP
#define NIMRTC_DATACHANNEL_SCTP_DATA_CHANNEL_HPP

#include <cstdint>
#include <memory>
#include <string_view>

#include <nimrtc/core/bytes.hpp>          // ByteSpan (= BufferView)
#include <nimrtc/plugins/base.hpp>         // Status, IPlugin
#include <nimrtc/plugins/datachannel.hpp>  // IDataChannel / IDataChannelFactory

namespace nimrtc::sctp {
class ISctpSocket;
}

namespace nimrtc::datachannel {

// ---------------------------------------------------------------------------
// Bring `Status` (an alias for uint32_t in nimrtc/plugins/base.hpp) into the
// datachannel namespace so the unqualified `Status open() noexcept override`
// and `Status route_send(...) noexcept` declarations parse correctly.
// Without this, MSVC's C++ grammar recovery treats `Status open() noexcept
// override` as `int open() noexcept override` (the default-int fallback
// in pre-C++23 mode), which then collides with the macro-`open` from
// <sys/stat.h> and produces a cascade of misleading "open override
// specifier unknown" / "cannot instantiate abstract class" errors.
// ---------------------------------------------------------------------------
// Bring `Status` and its error-code constants from
// nimrtc/plugins/base.hpp into the datachannel namespace so
// unqualified `Status` / `kOk` / `kErrNotReady` / `kErrInternal` use
// sites inside this header (and its .cpp) resolve to the canonical
// plugin seam values. Without these, MSVC's default-int fallback +
// the POSIX `open` macro from <sys/stat.h> collide and produce a
// cascade of "unknown override specifier" / "abstract class" errors
// on the IPlugin::open / IDataChannel::open method declarations.
using plugins::Status;
using plugins::kOk;
using plugins::kErrNotReady;
using plugins::kErrInternal;

// ---------------------------------------------------------------------------
// Guard against the POSIX / Windows SDK `open` (and `close`) macro from
// <io.h> / <sys/stat.h> / <fcntl.h>. Some MSVC + stdlib combinations
// (notably when <chrono> transitively pulls in <sys/stat.h> on
// Debug builds) define `open` as a macro that swallows trailing
// tokens, which makes the C++ `override` specifier on our method
// ill-formed and produces a cascade of misleading parse errors
// downstream (Status / kOk / route_send / on_state_ all "undeclared").
//
// `#pragma push_macro` saves the current definition (so we don't break
// callers that legitimately use POSIX open() / close() after this
// header); `#undef open` / `#undef close` neutralise the macro for the
// duration of the class body; `#pragma pop_macro` restores the prior
// definition on header exit. Scoped to this TU.
//
// Compilers other than MSVC don't define `open` as a macro in C++ mode,
// so the push/pop pair is a no-op there.
// ---------------------------------------------------------------------------
#ifdef _MSC_VER
#pragma push_macro("open")
#pragma push_macro("close")
#pragma push_macro("send")
#pragma push_macro("recv")
#pragma push_macro("accept")
#pragma push_macro("listen")
#pragma push_macro("connect")
#pragma push_macro("bind")
#pragma push_macro("shutdown")
#pragma push_macro("setsockopt")
#pragma push_macro("getsockopt")
#pragma push_macro("ioctl")
#undef open
#undef close
#undef send
#undef recv
#undef accept
#undef listen
#undef connect
#undef bind
#undef shutdown
#undef setsockopt
#undef getsockopt
#undef ioctl
#endif

/**
 * @brief SCTP-backed (usrsctp 0.9.5.0) implementation of IDataChannel.
 *
 * Wraps a `nimrtc::sctp::ISctpSocket` instance and dispatches:
 *   - `send()` → `send_stream` / `send_datagram` / `send_partial_reliable`
 *     based on the QoS in the last `open(cfg)` config;
 *   - inbound SCTP DATA payloads → `set_on_message` callback.
 *
 * The `IPlugin::open()` / `close()` pair wraps an `IDataChannel::open()`-
 * style lifecycle on top of `ISctpSocket::set_on_recv()` wiring (no
 * underlying transport handshake is performed here — that's the
 * engine's responsibility).
 */
class SctpDataChannel final : public plugins::IDataChannel {
public:
    /**
     * @brief Production constructor — creates the underlying
     *        `ISctpSocket` by looking up the `UsrsctpSocketFactory`
     *        (`id="usrsctp"`) in `core::PluginRegistry` and calling
     *        `factory->create({...})`.
     *
     * If the registry has no `id="usrsctp"` factory registered, the
     * channel is created in a "no-socket" state and every `send()`
     * returns `plugins::kErrNotReady`. The factory pointer is
     * resolved at construction time so callers do not need to call
     * `open()` before checking readiness.
     */
    SctpDataChannel();

    /**
     * @brief Test-only constructor — accepts an injected
     *        `ISctpSocket` (e.g. a MockSctpSocket).
     *
     * Production code MUST use the default constructor. This overload
     * exists so unit tests can drive the channel end-to-end without
     * standing up a real usrsctp socket.
     */
    explicit SctpDataChannel(std::unique_ptr<sctp::ISctpSocket> injected);

    ~SctpDataChannel() override;

    SctpDataChannel(const SctpDataChannel&)            = delete;
    SctpDataChannel& operator=(const SctpDataChannel&) = delete;
    SctpDataChannel(SctpDataChannel&&)                 = delete;
    SctpDataChannel& operator=(SctpDataChannel&&)      = delete;

    // ---- IPlugin ---------------------------------------------------------

    /** Returns `"nimrtc::datachannel::SctpDataChannel"`. */
    const char* name() const noexcept override;

    /** Wire the SCTP on-recv callback into `on_msg_` and mark the channel
     *  as opened. Returns kOk; kErrNotReady if the underlying socket
     *  was never created (factory not registered). */
    Status open() noexcept override;

    /** Release the underlying socket and clear callbacks. Idempotent. */
    void close() noexcept override;

    // ---- IDataChannel ----------------------------------------------------

    /** Store the per-channel configuration. The actual SCTP association
     *  is opened by the engine via the underlying `ISctpSocket`; this
     *  method only caches `cfg` for later `send()` routing. */
    void open(const plugins::DataChannelConfig& cfg) override;

    /** Forward `data` to the matching SCTP send method based on
     *  `cfg_.reliability`. Returns `kErrNotReady` if the channel has
     *  not been opened. */
    void send(plugins::BufferView data) override;

    /** Replace the message callback. noexcept per the seam contract. */
    void set_on_message(plugins::DataMessageCallback cb) noexcept override;

    /** Replace the state callback. noexcept per the seam contract. */
    void set_on_state(plugins::DataChannelStateCallback cb) noexcept override;

    // ---- Test-only accessors --------------------------------------------

    /** Returns true if the channel has a non-null underlying socket.
     *  Production code can use this to check readiness. */
    bool is_socket_ready() const noexcept { return static_cast<bool>(sock_); }

    /** Returns true if `open()` (the IPlugin overload) has been called. */
    bool is_opened() const noexcept { return opened_; }

    /** Returns the cached config (read-only). Useful in tests. */
    const plugins::DataChannelConfig& config() const noexcept { return cfg_; }

private:
    /** Translate a DataChannelReliability to the matching ISctpSocket
     *  send method on `sock_`. Returns the result of that send. */
    Status route_send(plugins::BufferView data) noexcept;

    /** Forwarding trampoline from usrsctp's recv thread to `on_msg_`. */
    void on_sctp_recv(uint16_t stream, core::ByteSpan data) noexcept;

    plugins::DataChannelConfig        cfg_{};
    plugins::DataMessageCallback      on_msg_;
    plugins::DataChannelStateCallback on_state_;
    std::unique_ptr<sctp::ISctpSocket> sock_;
    bool                              opened_ = false;
};

#ifdef _MSC_VER
#pragma pop_macro("open")
#pragma pop_macro("close")
#endif

// ---------------------------------------------------------------------------
// SctpDataChannelFactory — registration id "sctp".
// ---------------------------------------------------------------------------

/**
 * @brief Factory producing `SctpDataChannel` instances.
 *
 * Registered under id `"sctp"` (matching the production seam
 * `plugins::IDataChannelFactory`). The engine resolves
 * `cfg.datachannel_name = "sctp"` through
 * `core::PluginRegistry::get_datachannel("sctp")` and calls
 * `create()` to obtain a fresh channel per logical stream.
 *
 * Implementation lives in `sctp_data_channel.cpp`. The factory itself
 * is a stateless thin shell — see the .cpp for the registration
 * entry point.
 */
class SctpDataChannelFactory final : public plugins::IDataChannelFactory {
public:
    SctpDataChannelFactory() = default;
    ~SctpDataChannelFactory() override = default;

    SctpDataChannelFactory(const SctpDataChannelFactory&)            = delete;
    SctpDataChannelFactory& operator=(const SctpDataChannelFactory&) = delete;

    /** Registration id — MUST be the literal `"sctp"` so the engine /
     *  Profile loader can resolve the production backend explicitly. */
    std::string_view id() const noexcept override { return "sctp"; }

    /** Short human-readable name, surfaced in logs / registry listings. */
    std::string_view display_name() const noexcept override {
        return "usrsctp-backed DataChannel (SCTP / PR-SCTP / SCTP-over-DTLS)";
    }

    /** Allocate a fresh `SctpDataChannel`. Ownership passes to the caller;
     *  the caller is responsible for `delete`. */
    plugins::IDataChannel* create() const override;
};

// ---------------------------------------------------------------------------
// Public registration entry point
// ---------------------------------------------------------------------------

/**
 * @brief Register `SctpDataChannelFactory` with `core::PluginRegistry`
 *        under id `"sctp"`.
 *
 * Mirrors the same Meyers-singleton latch pattern used by
 * `nimrtc::sctp::register_default_plugins()`,
 * `nimrtc::ice::register_default_plugins()` and
 * `nimrtc::audio3a::register_default_plugins()` — see those for the
 * rationale (MSVC static-link workaround + idempotency).
 *
 * Also published in `pal_default_registrars.cpp` so the unified
 * `core::register_all_default_plugins()` walks it.
 *
 * Idempotent. Safe to call from multiple TUs. Not inline — see the
 * ice.cpp / audio3a_plugin.cpp comment for why the symbol must land
 * in a real .obj inside `nimrtc_datachannel.lib`.
 */
void register_default_plugins() noexcept;

} // namespace nimrtc::datachannel

#endif // NIMRTC_DATACHANNEL_SCTP_DATA_CHANNEL_HPP
