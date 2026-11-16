// src/modules/rd_input/src/control_channel.cpp
// ----------------------------------------------------------------------------
// The NimRTC-native control channel (remote-desktop plan G1 / G2).
//
// The framing is documented in <nimrtc/rd_input/control_channel.hpp>; this
// file is the only place that knows it.  A reader who wants to know what
// goes on the wire reads the header, and a reader who wants to change the
// format changes it here and in one place.
// ----------------------------------------------------------------------------

#include "nimrtc/rd_input/control_channel.hpp"

#include <nimrtc/core/registry.hpp>

#include <algorithm>
#include <cstring>
#include <utility>

namespace nimrtc::rd_input {

namespace {

// ---------------------------------------------------------------------------
// Varint helpers (LEB128) — the same encoding the input-event codec uses.
// ---------------------------------------------------------------------------

/// The number of bytes a varint needs.
std::size_t varint_size(std::uint64_t value) noexcept {
    std::size_t n = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++n;
    }
    return n;
}

/// Append a varint.
void append_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<std::uint8_t>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

/// The largest number of bytes a varint of a 64-bit value can occupy.
constexpr std::size_t kMaxVarintBytes = 10;

/// The header's fixed part: the magic byte + the version byte.
constexpr std::size_t kFixedHeaderBytes = 2;

/// The outbound queue's depth.  A bounded queue is the whole point: a
/// control path that queues without bound turns a congested link into a
/// memory leak.
constexpr std::size_t kQueueDepth = 64;

}  // namespace

// ---------------------------------------------------------------------------
// ControlChannel — construction / configuration
// ---------------------------------------------------------------------------

ControlChannel::ControlChannel(std::size_t max_message_bytes) {
    set_max_message_bytes(max_message_bytes);
}

void ControlChannel::set_max_message_bytes(std::size_t max_message_bytes) noexcept {
    if (max_message_bytes == 0) {
        max_message_bytes = kDefaultMaxControlMessageBytes;
    }
    // A channel that cannot carry a single byte is useless.
    if (max_message_bytes < 1) {
        max_message_bytes = 1;
    }
    max_message_bytes_ = max_message_bytes;
}

void ControlChannel::set_on_message(MessageCallback cb) noexcept {
    on_message_ = std::move(cb);
}

void ControlChannel::set_outbound_sink(ByteSink sink) noexcept {
    sink_ = std::move(sink);
}

void ControlChannel::clear() noexcept {
    outbox_.clear();
    inbox_.clear();
}

// ---------------------------------------------------------------------------
// The framing helpers
// ---------------------------------------------------------------------------

plugins::Status ControlChannel::frame_message(core::ByteSpan message,
                                              std::size_t max_message_bytes,
                                              std::vector<std::uint8_t>& out) noexcept {
    if (max_message_bytes == 0) {
        max_message_bytes = kDefaultMaxControlMessageBytes;
    }
    if (message.empty()) {
        return plugins::kErrInvalidParam;
    }
    if (message.size() > max_message_bytes) {
        // Never truncate: a truncated input event is a *wrong* input
        // event, which is worse than a missing one.
        return plugins::kErrInvalidParam;
    }

    out.clear();
    out.reserve(kFixedHeaderBytes + varint_size(message.size()) + message.size());
    out.push_back(kControlFrameMagic);
    out.push_back(kControlFrameVersion);
    append_varint(out, static_cast<std::uint64_t>(message.size()));
    out.insert(out.end(), message.begin(), message.end());
    return plugins::kOk;
}

std::size_t ControlChannel::framed_size(core::ByteSpan message,
                                         std::size_t max_message_bytes) noexcept {
    if (max_message_bytes == 0) {
        max_message_bytes = kDefaultMaxControlMessageBytes;
    }
    if (message.empty() || message.size() > max_message_bytes) {
        return 0;
    }
    return kFixedHeaderBytes + varint_size(message.size()) + message.size();
}

// ---------------------------------------------------------------------------
// The send side
// ---------------------------------------------------------------------------

plugins::Status ControlChannel::send(core::ByteSpan message) noexcept {
    if (message.empty()) {
        return plugins::kErrInvalidParam;
    }
    if (message.size() > max_message_bytes_) {
        return plugins::kErrInvalidParam;
    }

    std::vector<std::uint8_t> framed;
    const plugins::Status framed_rc =
        frame_message(message, max_message_bytes_, framed);
    if (framed_rc != plugins::kOk) {
        return framed_rc;
    }

    if (outbox_.size() >= kQueueDepth) {
        // The transport is not draining the control path.  The message is
        // dropped rather than queued without bound — and the drop is
        // counted, because a control path that silently loses the
        // operator's clicks is worse than no control path at all.
        ++messages_dropped_;
        return plugins::kErrBufferTooSmall;
    }

    outbox_.emplace_back(std::move(framed), message.size());

    // Try to hand it to the sink right away.  A sink that refuses the
    // bytes (the DTLS session is not connected yet, the raw-UDP queue is
    // full) leaves the message at the head of the queue for a later
    // retry.
    flush();
    return plugins::kOk;
}

plugins::Status ControlChannel::flush() noexcept {
    while (!outbox_.empty()) {
        if (!sink_) {
            // No transport yet: the message stays queued.
            return plugins::kErrNotReady;
        }

        const auto& framed = outbox_.front().first;
        const plugins::Status rc = sink_(
            core::ByteSpan{framed.data(), framed.size()});
        if (rc != plugins::kOk) {
            // The transport could not take the bytes *now*.  A control
            // message must not be lost, so it stays queued and goes out
            // on a later flush.
            return rc;
        }

        // The message was accepted.  The counters count *payload* bytes,
        // so the framing overhead is excluded — the counters answer "how
        // much payload moved", not "how many bytes hit the wire".
        ++messages_sent_;
        bytes_sent_ += outbox_.front().second;
        outbox_.pop_front();
    }
    return plugins::kOk;
}

// ---------------------------------------------------------------------------
// The receive side
// ---------------------------------------------------------------------------

plugins::Status ControlChannel::feed_inbound(core::ByteSpan bytes) noexcept {
    if (bytes.empty()) return plugins::kOk;

    inbox_.insert(inbox_.end(), bytes.begin(), bytes.end());

    for (;;) {
        // ---- The fixed header --------------------------------------------
        if (inbox_.size() < kFixedHeaderBytes) break;   // wait for more

        if (inbox_[0] != kControlFrameMagic) {
            // A foreign byte stream on this transport.  There is no way
            // to resynchronise: the magic byte is the only framing we
            // have, so the inbound buffer is dropped and the channel
            // stays usable for the *next* frame.
            ++frames_rejected_;
            ++frames_malformed_;
            inbox_.clear();
            return plugins::kErrCorrupt;
        }

        const std::uint8_t version = inbox_[1];
        if (version < kMinControlFrameVersion || version > kMaxControlFrameVersion) {
            // A framing version this build does not understand.  The
            // payload format may have changed, so the frame cannot be
            // parsed safely.
            ++frames_rejected_;
            ++frames_malformed_;
            inbox_.clear();
            return plugins::kErrCorrupt;
        }

        // ---- The length (a varint) ---------------------------------------
        //
        // The varint is read *before* anything is reserved, so a frame
        // that claims an absurd length costs nothing: the comparison
        // against `max_message_bytes_` happens first, and the buffer is
        // never grown to the claimed size.
        std::uint64_t length = 0;
        std::uint32_t shift = 0;
        std::size_t header_len = kFixedHeaderBytes;
        bool length_complete = false;

        while (header_len < inbox_.size() && header_len - kFixedHeaderBytes < kMaxVarintBytes) {
            const std::uint8_t byte = inbox_[header_len++];
            length |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
            if ((byte & 0x80) == 0) {
                length_complete = true;
                break;
            }
            shift += 7;
        }

        if (!length_complete) {
            if (header_len - kFixedHeaderBytes >= kMaxVarintBytes) {
                // A varint that does not terminate within ten bytes
                // cannot fit in 64 bits: a malformed frame, not a value.
                ++frames_rejected_;
                ++frames_malformed_;
                inbox_.clear();
                return plugins::kErrCorrupt;
            }
            break;   // the length is not complete yet — wait for more
        }

        if (length > static_cast<std::uint64_t>(max_message_bytes_)) {
            // The sender's length is not something we are willing to
            // honour, and no buffer was ever reserved for it.
            ++frames_rejected_;
            ++frames_oversized_;
            inbox_.clear();
            return plugins::kErrCorrupt;
        }

        const std::size_t total = header_len + static_cast<std::size_t>(length);
        if (inbox_.size() < total) break;   // the payload is not here yet

        deliver(core::ByteSpan{inbox_.data() + header_len,
                               static_cast<std::size_t>(length)});

        inbox_.erase(inbox_.begin(),
                     inbox_.begin() + static_cast<std::ptrdiff_t>(total));
    }

    return plugins::kOk;
}

void ControlChannel::deliver(core::ByteSpan message) noexcept {
    ++messages_received_;
    bytes_received_ += message.size();
    if (on_message_) {
        on_message_(message);
    }
}

// ---------------------------------------------------------------------------
// PluginControlChannel — the seam adapter
// ---------------------------------------------------------------------------

PluginControlChannel::PluginControlChannel(
        const plugins::ControlChannelBinding& binding)
    : binding_(binding) {
    channel_.set_max_message_bytes(binding.max_message_bytes);
    if (binding.sink) {
        channel_.set_outbound_sink(
            [sink = binding.sink](core::ByteSpan bytes) {
                return sink(bytes);
            });
    }
}

const char* PluginControlChannel::name() const noexcept {
    return "NimRTC-ControlChannel";
}

plugins::Status PluginControlChannel::open() noexcept {
    open_ = true;
    return plugins::kOk;
}

void PluginControlChannel::close() noexcept {
    open_ = false;
    channel_.clear();
}

plugins::Status PluginControlChannel::send_control_message(
        core::ByteSpan bytes) noexcept {
    if (!open_) return plugins::kErrNotReady;
    return channel_.send(bytes);
}

void PluginControlChannel::set_on_control_message(
        plugins::ControlMessageCallback cb) noexcept {
    channel_.set_on_message([cb = std::move(cb)](core::ByteSpan message) {
        if (cb) cb(message);
    });
}

plugins::Status PluginControlChannel::on_transport_bytes(
        core::ByteSpan bytes) noexcept {
    if (!open_) return plugins::kErrNotReady;
    return channel_.feed_inbound(bytes);
}

void PluginControlChannel::set_outbound_sink(plugins::ControlByteSink sink) noexcept {
    channel_.set_outbound_sink(
        [sink = std::move(sink)](core::ByteSpan bytes) {
            return sink ? sink(bytes) : plugins::kErrNotReady;
        });
}

plugins::Status PluginControlChannel::flush_control_messages() noexcept {
    return channel_.flush();
}

std::uint64_t PluginControlChannel::messages_sent() const noexcept {
    return channel_.messages_sent();
}

std::uint64_t PluginControlChannel::messages_received() const noexcept {
    return channel_.messages_received();
}

std::uint64_t PluginControlChannel::bytes_sent() const noexcept {
    return channel_.bytes_sent();
}

std::uint64_t PluginControlChannel::bytes_received() const noexcept {
    return channel_.bytes_received();
}

std::uint64_t PluginControlChannel::frames_rejected() const noexcept {
    return channel_.frames_rejected();
}

std::uint64_t PluginControlChannel::frames_oversized() const noexcept {
    return channel_.frames_oversized();
}

std::uint64_t PluginControlChannel::frames_malformed() const noexcept {
    return channel_.frames_malformed();
}

std::uint64_t PluginControlChannel::messages_dropped() const noexcept {
    return channel_.messages_dropped();
}

// ---------------------------------------------------------------------------
// ControlChannelFactory
// ---------------------------------------------------------------------------

std::string_view ControlChannelFactory::id() const noexcept {
    return kBackendId;
}

std::string_view ControlChannelFactory::display_name() const noexcept {
    return "NimRTC control channel (framed over a byte transport)";
}

std::unique_ptr<plugins::IControlChannel> ControlChannelFactory::create(
        const plugins::ControlChannelBinding& binding) const {
    return std::make_unique<PluginControlChannel>(binding);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

namespace {

const ControlChannelFactory g_control_channel_factory{};

}  // anonymous namespace

void register_default_plugins() noexcept {
    static bool registered = false;
    if (registered) return;
    registered = true;
    core::PluginRegistry::instance().register_control_channel(
        g_control_channel_factory.id(), &g_control_channel_factory);
}

} // namespace nimrtc::rd_input
