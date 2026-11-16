/**
 * @file src/engine/src/engine_control_seam.cpp
 * @brief The engine's control channel, resolved through the registry seam
 *        (remote-desktop plan G1 / G2 / G6).
 *
 * See `engine_control_seam.hpp` for the contract.
 */

#include "engine_control_seam.hpp"

#include <chrono>
#include <utility>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>

namespace nimrtc::engine::detail {

namespace {

/// The engine's monotonic clock, in microseconds.
///
/// The same clock the rest of the engine uses, so the heartbeat's
/// timestamp is comparable with the timeline's other instants.
inline std::int64_t steady_now_us() noexcept {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            core::SteadyClock::now().time_since_epoch()).count());
}

}  // namespace

// ---------------------------------------------------------------------------
// resolve
// ---------------------------------------------------------------------------

plugins::Status ControlChannelSeam::resolve(
        std::string_view factory_id,
        const plugins::ControlChannelBinding& binding) noexcept {
    if (factory_id.empty()) return plugins::kErrNotReady;

    const plugins::IControlChannelFactory* factory =
        core::PluginRegistry::instance().get_control_channel(factory_id);
    if (!factory) {
        core::log::Logger::instance().error(
            std::string("engine[control]: the control-channel factory '") +
            std::string(factory_id) +
            "' is not registered (known ids: " +
            [&] {
                std::string ids;
                for (const auto& id :
                     core::PluginRegistry::instance().list_control_channels()) {
                    if (!ids.empty()) ids += ", ";
                    ids += id;
                }
                return ids.empty() ? std::string{"<none>"} : ids;
            }() +
            ")");
        return plugins::kErrNotReady;
    }

    std::unique_ptr<plugins::IControlChannel> channel =
        factory->create(binding);
    if (!channel) {
        core::log::Logger::instance().error(
            std::string("engine[control]: the control-channel factory '") +
            std::string(factory_id) + "' returned no channel");
        return plugins::kErrNotReady;
    }

    const plugins::Status rc = channel->open();
    if (rc != plugins::kOk) {
        core::log::Logger::instance().error(
            std::string("engine[control]: the control channel could not be "
                        "opened (rc=") +
            std::to_string(static_cast<int>(rc)) + ")");
        return rc;
    }

    channel_ = std::move(channel);
    transport_ = binding.transport;

    core::log::Logger::instance().info(
        std::string("engine[control]: the control channel is resolved "
                    "through the seam (factory='") +
        std::string(factory_id) + "', transport=" +
        plugins::control_channel_transport_name(binding.transport) + ")");

    return plugins::kOk;
}

// ---------------------------------------------------------------------------
// The inbound side
// ---------------------------------------------------------------------------

void ControlChannelSeam::set_on_message(
        plugins::ControlMessageCallback cb) noexcept {
    if (!channel_) return;

    // The seam's callback is *wrapped*: the engine has to recognise a
    // heartbeat (plan G6) without knowing the payload format of the
    // control plane, and the heartbeat convention lives in the seam
    // (`plugins::kControlHeartbeatPayload`).
    channel_->set_on_control_message(
        [this, cb = std::move(cb)](core::ByteSpan message) noexcept {
            // The counters count *whole messages* — the payload bytes,
            // framing excluded, so they answer "how much payload moved".
            ++stats_.messages_received;
            stats_.bytes_received += message.size();

            if (message.size() == 1 &&
                message[0] == plugins::kControlHeartbeatPayload) {
                ++stats_.heartbeats_received;
                last_heartbeat_us_ = steady_now_us();
            }
            if (cb) cb(message);
        });
}

plugins::Status ControlChannelSeam::on_transport_bytes(
        std::span<const std::uint8_t> bytes) noexcept {
    if (!channel_) return plugins::kErrNotReady;
    if (bytes.empty()) return plugins::kOk;

    const plugins::Status rc = channel_->on_transport_bytes(
        core::ByteSpan{bytes.data(), bytes.size()});
    // The channel itself counts the refused frames (and splits them into
    // "oversized" and "malformed"), so the engine just mirrors the
    // counters.  A refused frame is *not* a fatal condition — the
    // channel stays usable for the next frame.
    stats_.oversized_received = channel_->frames_oversized();
    stats_.malformed_received = channel_->frames_malformed();
    return rc;
}

// ---------------------------------------------------------------------------
// The outbound side
// ---------------------------------------------------------------------------

plugins::Status ControlChannelSeam::send(
        std::span<const std::uint8_t> message) noexcept {
    if (!channel_) return plugins::kErrNotReady;
    if (message.empty()) return plugins::kErrInvalidParam;

    const plugins::Status rc = channel_->send_control_message(
        core::ByteSpan{message.data(), message.size()});
    if (rc != plugins::kOk) return rc;

    ++stats_.messages_sent;
    stats_.bytes_sent += message.size();
    stats_.messages_dropped = channel_->messages_dropped();
    return plugins::kOk;
}

plugins::Status ControlChannelSeam::flush() noexcept {
    if (!channel_) return plugins::kErrNotReady;
    const plugins::Status rc = channel_->flush_control_messages();
    stats_.messages_dropped = channel_->messages_dropped();
    return rc;
}

plugins::Status ControlChannelSeam::send_heartbeat() noexcept {
    const std::uint8_t payload = plugins::kControlHeartbeatPayload;
    const plugins::Status rc = send(
        std::span<const std::uint8_t>(&payload, 1));
    if (rc != plugins::kOk) return rc;
    ++stats_.heartbeats_sent;
    return plugins::kOk;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

void ControlChannelSeam::reset_stats() noexcept {
    stats_ = ControlChannelStats{};
}

void ControlChannelSeam::close() noexcept {
    if (channel_) {
        channel_->close();
        channel_.reset();
    }
}

}  // namespace nimrtc::engine::detail
