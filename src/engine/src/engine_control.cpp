/**
 * @file src/engine/src/engine_control.cpp
 * @brief The remote-desktop control channel's engine-side plumbing.
 *
 * Plan **G1** (the control channel over the DTLS app-data seam), plan
 * **G2** (the control path on its own transport) and plan **G6** (the
 * dead-man / heartbeat).
 *
 * The *framing* lives in the `rd_input` module and is reached through
 * the `plugins::IControlChannel` seam — the engine never includes a
 * concrete module's header (Layout Invariant 4 / ADR-025).  This file
 * is the engine's half of the wiring:
 *
 *   - it resolves the channel through the registry's control-channel
 *     slot (`EngineConfig::control_channel_name`);
 *   - it installs the **outbound sink** that puts the framed bytes on
 *     the wire (the DTLS app-data seam, or the control stack's raw-UDP
 *     path — plan G2);
 *   - it feeds the inbound bytes (from either transport) into the
 *     channel and drains the whole messages to the host on the engine's
 *     thread.
 *
 * @see engine_control_seam.hpp — the seam-backed channel.
 * @see engine_control_inbox.hpp — the lock + the pending queue.
 * @see docs/plan/remote-desktop-plan.md §Track G for the plan rows.
 */

#include "nimrtc/engine/engine.hpp"

#include "engine_control_inbox.hpp"
#include "engine_control_seam.hpp"
#include "engine_impl.hpp"

#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include <nimrtc/core/log.hpp>
#include <nimrtc/dtls/dtls_session_iface.hpp>
#include <nimrtc/plugins/base.hpp>
#include <nimrtc/transport/transport_stack.hpp>

namespace nimrtc::engine {

// ---------------------------------------------------------------------------
// The public surface
// ---------------------------------------------------------------------------

plugins::Status NimRTCEngine::send_control_message(
        std::span<const std::uint8_t> payload) noexcept {
    if (!is_open()) return plugins::kErrNotReady;
    if (payload.empty()) return plugins::kErrInvalidParam;

    {
        std::lock_guard<std::mutex> lock(impl_->control_inbox.channel_mutex);
        const plugins::Status rc = impl_->control_seam.send(payload);
        if (rc != plugins::kOk) {
            // The seam refuses a message for one of three reasons: the
            // control plane is disabled, the message is too long, or
            // the queue is full.  They mean different things to the
            // caller, so they get different status codes.
            if (rc == plugins::kErrNotReady) return plugins::kErrNotReady;
            if (rc == plugins::kErrInvalidParam) return plugins::kErrInvalidParam;
            return plugins::kErrBufferTooSmall;
        }
    }

    // Put it on the wire now rather than waiting for the next tick: a
    // control message is latency-critical and the host asked for it
    // *now*.
    flush_control_channel();
    return plugins::kOk;
}

void NimRTCEngine::set_on_control_message(
        std::function<void(std::span<const std::uint8_t>)> cb) noexcept {
    on_control_message_ = std::move(cb);
}

NimRTCEngine::ControlChannelStats
NimRTCEngine::control_channel_stats() const noexcept {
    const auto& s = impl_->control_seam.stats();
    ControlChannelStats out;
    out.messages_sent       = s.messages_sent;
    out.messages_received   = s.messages_received;
    out.bytes_sent          = s.bytes_sent;
    out.bytes_received      = s.bytes_received;
    out.messages_dropped    = s.messages_dropped;
    out.oversized_received  = s.oversized_received;
    out.malformed_received  = s.malformed_received;
    out.heartbeats_received = s.heartbeats_received;
    out.heartbeats_sent     = s.heartbeats_sent;
    return out;
}

std::optional<std::int64_t>
NimRTCEngine::last_control_heartbeat_us() const noexcept {
    return impl_->control_seam.last_heartbeat_us();
}

plugins::Status NimRTCEngine::send_control_heartbeat() noexcept {
    if (!is_open()) return plugins::kErrNotReady;

    {
        std::lock_guard<std::mutex> lock(impl_->control_inbox.channel_mutex);
        const plugins::Status rc = impl_->control_seam.send_heartbeat();
        if (rc != plugins::kOk) {
            return rc == plugins::kErrBufferTooSmall
                       ? plugins::kErrBufferTooSmall
                       : rc;
        }
    }

    flush_control_channel();
    return plugins::kOk;
}

// ---------------------------------------------------------------------------
// G5 — the input <-> frame binding, engine half
// ---------------------------------------------------------------------------

std::optional<std::uint64_t>
NimRTCEngine::last_displayed_ref_frame_id() const noexcept {
    if (!is_open()) return std::nullopt;

    // The last *displayed* frame is what the operator was looking at
    // (plan E9), so its `ref_frame` id is the id an outgoing input
    // event has to carry (plan G5).  The record is only written when
    // the frame reached the sink, which is exactly the semantics the
    // binding needs: "the frame the operator saw", not "the frame the
    // operator sent".
    const auto& rec = impl_->render.last_displayed;
    if (!rec.has_value()) return std::nullopt;
    if (!rec->has_ref_frame) return std::nullopt;
    return rec->ref_frame_id;
}

// ---------------------------------------------------------------------------
// The transport selection
// ---------------------------------------------------------------------------

plugins::ControlChannelTransport
NimRTCEngine::control_channel_transport() const noexcept {
    // The *resolved* transport, not the requested one: `kAuto` was
    // resolved once, when the channel was created, and the answer is
    // fixed from then on.
    if (!impl_->control_seam.active()) return config_.control_channel_transport;
    return impl_->control_seam.transport();
}

NimRTCEngine::ControlTransport
NimRTCEngine::control_transport() const noexcept {
    // The host's choice wins.  A session that asked for the DTLS
    // app-data seam (plan G1) must not silently end up on the
    // raw-UDP path (plan G2) just because the transport strategy also
    // built a control stack — the two paths differ in their latency
    // and their security properties (the raw-UDP path is not DTLS
    // encrypted), so the choice must be the host's.
    switch (config_.control_channel_transport) {
        case plugins::ControlChannelTransport::kRawUdp:
            if (impl_->session.control_stack &&
                impl_->session.control_stack->has_control_path()) {
                return ControlTransport::kRawControl;
            }
            return ControlTransport::kNone;

        case plugins::ControlChannelTransport::kDtlsAppData:
            if (impl_->dtls.lock()) return ControlTransport::kDtlsAppData;
            return ControlTransport::kNone;

        case plugins::ControlChannelTransport::kDataChannel:
            // The DataChannel transport is not wired by the engine: a
            // host that wants it wires the DataChannel itself (the
            // plan's G3 transport, which needs the DTLS-SCTP
            // association the plan's G1b describes).
            return ControlTransport::kNone;

        case plugins::ControlChannelTransport::kAuto:
            // `kAuto` prefers the raw-UDP control path when the
            // session has one (that is the path with its own
            // congestion-control domain), and falls back to the DTLS
            // app-data seam otherwise.
            if (impl_->session.control_stack &&
                impl_->session.control_stack->has_control_path()) {
                return ControlTransport::kRawControl;
            }
            if (impl_->dtls.lock()) return ControlTransport::kDtlsAppData;
            return ControlTransport::kNone;
    }

    return ControlTransport::kNone;
}

// ---------------------------------------------------------------------------
// The resolve step — the registry seam
// ---------------------------------------------------------------------------

uint32_t NimRTCEngine::resolve_control_channel() noexcept {
    // An empty id means "no control plane": the engine does not resolve
    // anything and the control API reports kErrNotReady.  This is the
    // default, so a session that does not want a control plane does not
    // pay for one.
    if (config_.control_channel_name.empty()) return 0;

    // The transport the host asked for.  `kAuto` resolves to the raw
    // control path when the session has one, and to the DTLS app-data
    // seam otherwise — the choice is made **once**, here, and never
    // changes afterwards.
    plugins::ControlChannelTransport transport = config_.control_channel_transport;
    if (transport == plugins::ControlChannelTransport::kAuto) {
        transport = (control_transport() == ControlTransport::kRawControl)
                       ? plugins::ControlChannelTransport::kRawUdp
                       : plugins::ControlChannelTransport::kDtlsAppData;
    }

    // A transport the session cannot provide is a **hard error**: a host
    // that asked for G2's separate congestion-control domain must be
    // told when it did not get one — a silent downgrade would make the
    // product promise false.
    if (transport == plugins::ControlChannelTransport::kRawUdp &&
        control_transport() != ControlTransport::kRawControl) {
        const std::string msg =
            "engine: the control channel was asked to ride the control "
            "stack's raw-UDP path (plan G2), but this session has no "
            "control stack with a control path";
        core::log::Logger::instance().error(msg);
        if (on_error_) on_error_(core::kEngineConfig, msg);
        return core::kEngineConfig;
    }
    if (transport == plugins::ControlChannelTransport::kDtlsAppData &&
        control_transport() == ControlTransport::kNone) {
        const std::string msg =
            "engine: the control channel was asked to ride the DTLS "
            "application-data seam (plan G1), but this session has no "
            "DTLS session";
        core::log::Logger::instance().error(msg);
        if (on_error_) on_error_(core::kEngineConfig, msg);
        return core::kEngineConfig;
    }

    // The sink: the framed bytes go to the transport the host selected.
    plugins::ControlChannelBinding binding;
    binding.transport = transport;
    binding.max_message_bytes = config_.control_channel_max_message_bytes;
    binding.sink = [this](core::ByteSpan bytes) noexcept -> plugins::Status {
        return send_control_bytes(bytes);
    };

    const plugins::Status rc =
        impl_->control_seam.resolve(config_.control_channel_name, binding);
    if (rc != plugins::kOk) {
        const std::string msg =
            std::string{"engine: the control channel '"} +
            config_.control_channel_name +
            "' could not be resolved through the registry";
        core::log::Logger::instance().error(msg);
        if (on_error_) on_error_(core::kEngineConfig, msg);
        return core::kEngineConfig;
    }

    // The inbound callback: the channel hands the whole messages over,
    // and the engine *queues* them (the raw-UDP path delivers on the
    // ARQ thread, so a direct call would run the host's callback on an
    // arbitrary thread — and under the channel lock).
    impl_->control_seam.set_on_message(
        [this](core::ByteSpan message) noexcept {
            // The queue has its own lock: this callback runs *inside*
            // `on_transport_bytes()`, which the receive path calls while
            // holding the channel lock.  Taking the channel lock again
            // here would deadlock on the non-recursive mutex.
            std::lock_guard<std::mutex> lock(impl_->control_inbox.queue_mutex);
            impl_->control_inbox.pending.emplace_back(message.begin(),
                                                       message.end());
        });

    return 0;
}

// ---------------------------------------------------------------------------
// The send path
// ---------------------------------------------------------------------------

plugins::Status NimRTCEngine::send_control_bytes(
        core::ByteSpan bytes) noexcept {
    const ControlTransport transport = control_transport();
    if (transport == ControlTransport::kNone) return plugins::kErrNotReady;

    if (transport == ControlTransport::kDtlsAppData) {
        auto d = impl_->dtls.lock();
        if (!d) return plugins::kErrNotReady;
        return d->send_app_data(bytes);
    }

    // Plan G2: the control stack's own raw-UDP path.
    return impl_->session.control_stack->send_control(bytes);
}

void NimRTCEngine::flush_control_channel() noexcept {
    if (!impl_->control_seam.active()) return;

    // The channel keeps a refused message queued, so the flush is
    // idempotent: a transport that cannot take the bytes *now* leaves
    // them for the next tick.  A control message must not be lost.
    {
        std::lock_guard<std::mutex> lock(impl_->control_inbox.channel_mutex);
        impl_->control_seam.flush();
    }
}

// ---------------------------------------------------------------------------
// The receive path
// ---------------------------------------------------------------------------

void NimRTCEngine::on_control_bytes(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.empty()) return;

    // The channel reassembles the messages and hands each one to its
    // callback — which the engine installed in `resolve_control_channel()`
    // and which *queues* the message.  The tick loop delivers it to the
    // host, so the host's callback always runs on the engine's thread.
    {
        std::lock_guard<std::mutex> lock(impl_->control_inbox.channel_mutex);
        impl_->control_seam.on_transport_bytes(bytes);
    }
}

void NimRTCEngine::drain_control_messages() noexcept {
    if (!on_control_message_) {
        // No host callback: drop the queued messages rather than let
        // them pile up.  The queue is bounded by the tick's drain rate,
        // but a session with no callback must not accumulate.
        std::lock_guard<std::mutex> lock(impl_->control_inbox.queue_mutex);
        impl_->control_inbox.pending.clear();
        return;
    }

    for (;;) {
        std::vector<std::uint8_t> message;
        {
            std::lock_guard<std::mutex> lock(impl_->control_inbox.queue_mutex);
            if (impl_->control_inbox.pending.empty()) break;
            message = std::move(impl_->control_inbox.pending.front());
            impl_->control_inbox.pending.pop_front();
        }
        // The host's callback runs *outside* the lock, on the engine's
        // thread.
        on_control_message_(std::span<const std::uint8_t>(message.data(),
                                                          message.size()));
    }
}

}  // namespace nimrtc::engine
