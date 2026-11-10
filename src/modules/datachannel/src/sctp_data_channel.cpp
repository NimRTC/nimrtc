/**
 * @file src/modules/datachannel/src/sctp_data_channel.cpp
 * @brief SctpDataChannel — usrsctp-backed IDataChannel implementation.
 *
 * Implements the QoS → SCTP send-method routing and the recv-callback
 * forwarder. Construction looks up the production SCTP backend
 * (`id="usrsctp"` in `core::PluginRegistry`) so the channel is ready
 * to dispatch as soon as `open()` is called.
 *
 * ## Stream id
 *
 * Every channel writes to SCTP stream 0.  The stream id is NOT derived
 * from `cfg.priority`: RFC 8832 §6 negotiates the stream id inside
 * the DCEP `DATA_CHANNEL_OPEN` message, so a channel must send its
 * OPEN on the stream it intends to use — a priority-derived id would
 * desynchronise the handshake.  `cfg.priority` is still carried in
 * the DCEP OPEN (RFC 8832 §5.1), so it reaches the peer; it does not
 * select a stream.
 *
 * v1.2.0 (WP-E1, `docs/plan/datachannel-capability-plan.md`): the
 * former `s_stream_from_priority()` helper — which took a priority
 * and ignored it — was deleted rather than left as a promise the
 * code did not keep.  The SCTP backend is a **test-only** backend
 * (WP-A / DEC-4 = A1) and serves as the PR-SCTP oracle for WP-D;
 * the priority that orders the production data plane is the
 * WebTransport backend's intra-plane ordering (WP-E2).
 *
 * ## Reliability → SCTP send method
 *
 *   - kReliableOrdered      → send_stream()
 *   - kUnreliable           → send_datagram()
 *   - kPartialReliableTTL   → send_partial_reliable(ttl = cfg.max_packet_lifetime_ms)
 *   - kPartialReliableCount → send_partial_reliable(ttl = max_retransmits * 100 ms)
 *
 * The count-mode TTL is a conservative heuristic — usrsctp exposes TTL
 * not count, so we approximate "N retransmits" by "N * 100ms of TTL".
 * This is an approximation of the count policy, not the count policy
 * itself (v1.2.0, WP-D3): the SCTP backend is a test-only backend
 * that acts as the PR-SCTP oracle for WP-D, and the approximation is
 * acceptable in that role only.
 *
 * ## Non-blocking I/O
 *
 * `send_*` calls on `ISctpSocket` are noexcept and non-blocking; they
 * return `kOk` as soon as the message is enqueued in usrsctp's send
 * buffer. The recv callback fires on usrsctp's worker thread; we
 * forward to `on_msg_` directly (no marshalling).
 */

#include <nimrtc/datachannel/sctp_data_channel.hpp>

#include <chrono>
#include <utility>
#include <vector>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/sctp/dcep.hpp>
#include <nimrtc/sctp/sctp_socket_factory.hpp>
#include <nimrtc/sctp/sctp_socket_iface.hpp>
#include <nimrtc/sctp/usrsctp_factory.hpp>

// ---------------------------------------------------------------------------
// MSVC POSIX `open` / `close` macro guard (mirrors sctp_data_channel.hpp).
//
// <sys/stat.h> (transitively pulled in by <chrono> on some MSVC + stdlib
// configurations) defines `open` as a macro that swallows trailing
// tokens.  Without this guard the compiler would mangle
// `Status SctpDataChannel::open() noexcept` into a syntax error because
// the macro would try to eat `()` and `noexcept` as part of its
// arguments.  The header's `#pragma push_macro` only protects the
// declarations inside the class — the out-of-line method definitions
// below need their own guard.
// ---------------------------------------------------------------------------
#ifdef _MSC_VER
#pragma push_macro("open")
#pragma push_macro("close")
#undef open
#undef close
#endif

namespace nimrtc::datachannel {

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** The SCTP stream id every channel writes to.
 *
 *  RFC 8832 §6 negotiates the stream id inside the DCEP
 *  `DATA_CHANNEL_OPEN`, so the id a channel sends its OPEN on is the
 *  id it will use for its payload — it cannot be chosen locally from
 *  the channel's priority (v1.2.0, WP-E1: the former
 *  `s_stream_from_priority()` stub was deleted rather than kept as an
 *  unimplemented promise).  `cfg.priority` still travels to the peer
 *  inside the DCEP OPEN (RFC 8832 §5.1). */
constexpr std::uint16_t kSctpStreamId = 0;

/** Approximate retransmit count → TTL (usrsctp's PR-SCTP API exposes
 *  TTL, not count). 100 ms per retransmit is the WebRTC default.
 *
 *  v1.2.0 (WP-D3): this is an **approximation, not the count
 *  policy** — usrsctp has no per-message retransmit budget.  It is
 *  acceptable only because the SCTP backend is a test-only backend
 *  (WP-A / DEC-4 = A1) that serves as the PR-SCTP *oracle* for WP-D:
 *  the WebTransport implementation's per-message budget is tested
 *  against the behaviours this approximation models (a message that
 *  outlives its budget is dropped; the channel stays open). */
inline std::chrono::milliseconds s_ttl_from_retransmits(
    std::uint16_t max_retransmits) noexcept {
    // Cap at 60s to avoid pathological values; usrsctp's max TTL is
    // effectively unbounded but a sane upper bound keeps test
    // expectations stable.
    const auto ms = static_cast<std::int64_t>(max_retransmits) * 100;
    if (ms <= 0) return std::chrono::milliseconds{0};
    if (ms > 60000) return std::chrono::milliseconds{60000};
    return std::chrono::milliseconds{ms};
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

SctpDataChannel::SctpDataChannel()
    : sock_(nullptr) {
    // Production ctor — resolve the usrsctp backend through the
    // registry. If the registry is empty (e.g. datachannel registered
    // before sctp::register_default_plugins()), `sock_` stays null
    // and `send()` returns `kErrNotReady`.
    const auto* sctp_factory =
        core::PluginRegistry::instance().get_sctp_socket("usrsctp");
    if (!sctp_factory) {
        core::log::Logger::instance().warn(
            "SctpDataChannel: no usrsctp factory registered (id=\"usrsctp\"); "
            "channel will report kErrNotReady from every send(). Call "
            "nimrtc::sctp::register_default_plugins() before creating the "
            "channel to enable the production backend.");
        return;
    }

    sctp::SctpConfig sctp_cfg{};
    sctp_cfg.label           = "datachannel.sctp";
    sctp_cfg.max_num_streams = 16;   // matches WebRTC DataChannel default
    sctp_cfg.local_port      = 0;    // usrsctp picks an ephemeral port
    sock_ = sctp_factory->create(sctp_cfg);
    if (!sock_) {
        core::log::Logger::instance().error(
            "SctpDataChannel: UsrsctpSocketFactory::create() returned null");
    }
}

SctpDataChannel::SctpDataChannel(std::unique_ptr<sctp::ISctpSocket> injected)
    : sock_(std::move(injected)) {
    // Test ctor — caller owns the socket semantics; we just take
    // ownership and move on. `sock_` may be null if the test injected
    // a no-op socket.
}

SctpDataChannel::~SctpDataChannel() {
    // `close()` already tears down the socket + clears callbacks, but
    // belt-and-braces: if the user forgot to call close() before
    // destruction, drop the socket here so usrsctp's recv thread
    // doesn't fire callbacks into a freed object.
    close();
}

// ---------------------------------------------------------------------------
// IPlugin
// ---------------------------------------------------------------------------

const char* SctpDataChannel::name() const noexcept {
    return "nimrtc::datachannel::SctpDataChannel";
}

Status SctpDataChannel::open() noexcept {
    if (opened_) return kOk;   // idempotent
    if (!sock_)  return kErrNotReady;

    // Wire the SCTP recv callbacks into our dispatchers. The PPID-aware
    // callback is preferred (DCEP demux); the legacy callback stays as
    // the fallback for backends/mocks that do not speak the PPID seam.
    // Lifetime: `this` is captured raw; close() clears both callbacks
    // before the socket is dropped, so no use-after-free.
    sock_->set_on_recv_with_ppid(
        [this](std::uint16_t stream, std::uint32_t ppid, core::ByteSpan data) {
            this->on_sctp_recv_with_ppid(stream, ppid, data);
        });
    sock_->set_on_recv([this](std::uint16_t stream, core::ByteSpan data) {
        this->on_sctp_recv(stream, data);
    });
    opened_ = true;

    // Fire "connecting" then "open" state events so the engine knows
    // the channel is up. The "open" event is informational only —
    // real SCTP COMM_UP notifications are forwarded by the engine,
    // not by this channel.
    if (on_state_) {
        on_state_("connecting");
        on_state_("open");
    }
    return kOk;
}

void SctpDataChannel::close() noexcept {
    // Idempotent. Reset state in a safe order:
    //   1. Clear the recv callback first so the SCTP worker thread
    //      can't fire into a half-destroyed channel.
    //   2. Drop the socket — usrsctp tears down its send buffer and
    //      the per-instance handle here.
    //   3. Mark the channel closed.
    if (sock_) {
        // set_on_recv with an empty std::function clears the
        // callback. Per `ISctpSocket` contract this is noexcept.
        sock_->set_on_recv({});
        sock_->set_on_recv_with_ppid({});
    }
    sock_.reset();
    opened_ = false;
    dcep_open_sent_ = false;

    if (on_state_) {
        on_state_("closed");
        on_state_ = nullptr;
    }
    on_msg_ = nullptr;
}

// ---------------------------------------------------------------------------
// IDataChannel
// ---------------------------------------------------------------------------

void SctpDataChannel::open(const plugins::DataChannelConfig& cfg) {
    // Cache the config so subsequent `send()` calls know which QoS
    // path to take. The actual SCTP association is opened by the
    // engine (via ISctpSocket::listen / connect / wait_for_established)
    // before this method is called.
    cfg_ = cfg;
}

void SctpDataChannel::send(plugins::BufferView data) {
    // Not noexcept? Per the seam contract `send` is *not* noexcept
    // (only the IPlugin surface is). We can still abort loudly on
    // programmer errors via the return Status path.
    //
    // Route the message. `route_send()` is noexcept and returns the
    // underlying send's Status; we forward that as the function
    // return value.
    const Status s = route_send(data);
    (void)s;   // suppress unused-variable warnings if the caller ignores
    // Note: IDataChannel::send has `void` return type — the engine
    // detects failures via the on_state callback firing "error".
    if (s != kOk && on_state_) {
        on_state_("error");
    }
}

void SctpDataChannel::set_on_message(
    plugins::DataMessageCallback cb) noexcept {
    on_msg_ = std::move(cb);
}

void SctpDataChannel::set_on_state(
    plugins::DataChannelStateCallback cb) noexcept {
    on_state_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

Status SctpDataChannel::route_send(plugins::BufferView data) noexcept {
    if (!opened_) return kErrNotReady;
    if (!sock_)   return kErrNotReady;

    const std::uint16_t stream = kSctpStreamId;
    // User payloads ride PPID string(51)/binary(53) per RFC 8831 §6.
    // The DCEP OPEN handshake uses PPID 50 (see send_dcep_open()).
    const std::uint32_t ppid = sctp::dcep_ppid_for_config(cfg_);
    const bool unordered =
        (cfg_.reliability == plugins::DataChannelReliability::kUnreliable) ||
        !cfg_.ordered;

    switch (cfg_.reliability) {
        case plugins::DataChannelReliability::kReliableOrdered:
            return sock_->send_stream_with_ppid(stream, data, ppid);

        case plugins::DataChannelReliability::kUnreliable:
            return sock_->send_datagram_with_ppid(stream, data, ppid);

        case plugins::DataChannelReliability::kPartialReliableTTL: {
            const auto ttl = std::chrono::milliseconds{
                cfg_.max_packet_lifetime_ms};
            return sock_->send_partial_reliable_with_ppid(stream, data, ttl,
                                                          ppid, unordered);
        }

        case plugins::DataChannelReliability::kPartialReliableCount: {
            // See header comment — usrsctp exposes TTL not count, so
            // we approximate count by N * 100 ms.
            const auto ttl = s_ttl_from_retransmits(cfg_.max_retransmits);
            return sock_->send_partial_reliable_with_ppid(stream, data, ttl,
                                                          ppid, unordered);
        }
    }
    return kErrInternal;   // unknown reliability mode
}

void SctpDataChannel::send_dcep_open() noexcept {
    if (!opened_ || !sock_) return;
    if (dcep_open_sent_) return;
    const std::vector<std::uint8_t> open = sctp::dcep_encode_open(cfg_);
    plugins::BufferView bv{open.data(), open.size()};
    // DATA_CHANNEL_OPEN rides PPID 50 on the negotiated stream.
    const plugins::Status s =
        sock_->send_stream_with_ppid(kSctpStreamId, bv, sctp::kPpidDcep);
    if (s == kOk) {
        dcep_open_sent_ = true;
    } else if (on_state_) {
        on_state_("error");
    }
}

void SctpDataChannel::on_sctp_recv_with_ppid(
    uint16_t /*stream*/, std::uint32_t ppid, core::ByteSpan data) noexcept {
    if (ppid == sctp::kPpidDcep) {
        if (data.empty()) return;
        if (data[0] == sctp::kDcepAck) {
            // Peer acknowledged our OPEN — channel is usable.
            if (on_state_) on_state_("open");
            return;
        }
        if (data[0] == sctp::kDcepOpen) {
            // Peer opened a channel toward us: adopt its label/QoS so
            // send() routes correctly, reply ACK, and report open.
            const auto* bytes = data.data();
            if (const auto params =
                    sctp::dcep_decode_open(bytes, data.size())) {
                cfg_.label = params->label;
                cfg_.reliability = params->reliability;
                cfg_.ordered = params->ordered;
                cfg_.max_retransmits = params->max_retransmits;
                cfg_.max_packet_lifetime_ms =
                    params->max_packet_lifetime_ms;
            }
            const std::vector<std::uint8_t> ack = sctp::dcep_encode_ack();
            plugins::BufferView bv{ack.data(), ack.size()};
            (void)sock_->send_stream_with_ppid(
                kSctpStreamId, bv, sctp::kPpidDcep);
            if (on_state_) on_state_("open");
            return;
        }
        return;  // unknown DCEP type — drop
    }
    if (ppid == sctp::kPpidString || ppid == sctp::kPpidBinary) {
        on_sctp_recv(/*stream=*/0, data);
        return;
    }
    // Unknown PPID — drop per RFC 8831 §6 (no user callback).
}

void SctpDataChannel::on_sctp_recv(uint16_t /*stream*/,
                                   core::ByteSpan data) noexcept {
    // Forward to the user callback. The callback signature is
    // `void(BufferView)` and `BufferView = core::ByteSpan`, so the
    // conversion is a no-op.
    if (on_msg_) {
        on_msg_(plugins::BufferView{data.data(), data.size()});
    }
}

// ---------------------------------------------------------------------------
// SctpDataChannelFactory
// ---------------------------------------------------------------------------

plugins::IDataChannel* SctpDataChannelFactory::create() const {
    return new SctpDataChannel();
}

} // namespace nimrtc::datachannel

#ifdef _MSC_VER
#pragma pop_macro("open")
#pragma pop_macro("close")
#endif
