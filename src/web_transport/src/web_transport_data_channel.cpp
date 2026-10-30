/**
 * @file src/web_transport/src/web_transport_data_channel.cpp
 * @brief WebTransportDataChannel implementation (v0.14.0-beta.1, DC-1 land).
 *
 * Implements `plugins::IDataChannel` on top of `IWebTransportSession`.
 * See `web_transport_data_channel.hpp` for design notes.
 */

#include <nimrtc/web_transport/web_transport_data_channel.hpp>
#include <nimrtc/web_transport/web_transport_state.hpp>

#include <algorithm>
#include <cstring>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>

// Windows macros conflict with std::min / std::max.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace nimrtc::web_transport {

namespace {

// Map DataChannelReliability to a state string for the on_state callback.
const char* reliability_to_state_string(plugins::DataChannelReliability r,
                                        bool stream_exists) noexcept {
    switch (r) {
        case plugins::DataChannelReliability::kReliableOrdered:      return stream_exists ? "open" : "connecting";
        case plugins::DataChannelReliability::kUnreliable:          return "open";  // datagram, no stream
        case plugins::DataChannelReliability::kPartialReliableTTL:   return stream_exists ? "open" : "connecting";
        case plugins::DataChannelReliability::kPartialReliableCount: return stream_exists ? "open" : "connecting";
    }
    return "error";
}

// QUIC variable-length integer decode that tolerates a split header:
// returns false when `buf` does not yet hold a complete integer at `pos`.
bool try_decode_varint(const std::vector<std::uint8_t>& buf,
                       std::size_t& pos,
                       std::uint64_t& value) noexcept {
    if (pos >= buf.size()) return false;
    const std::uint8_t lead = buf[pos];
    const std::size_t width = std::size_t{1} << (lead >> 6);
    if (buf.size() - pos < width) return false;
    std::uint64_t v = lead & 0x3Fu;
    for (std::size_t i = 1; i < width; ++i) {
        v = (v << 8) | buf[pos + i];
    }
    value = v;
    pos += width;
    return true;
}

} // anonymous namespace

// ===========================================================================
// WebTransportDataChannel
// ===========================================================================

WebTransportDataChannel::WebTransportDataChannel(IWebTransportSession* session) noexcept
    : session_(session) {}

WebTransportDataChannel::~WebTransportDataChannel() {
    close();
}

const char* WebTransportDataChannel::name() const noexcept {
    return "nimrtc::web_transport::WebTransportDataChannel";
}

plugins::Status WebTransportDataChannel::open() noexcept {
    if (!session_) return kErrNotReady;
    if (opened_.load(std::memory_order_acquire)) return kOk;

    // For kUnreliable, we don't need a stream — datagrams go directly
    // through the session.  For other modes, the stream is opened lazily
    // on first send().
    opened_.store(true, std::memory_order_release);

    if (cfg_.reliability != plugins::DataChannelReliability::kUnreliable) {
        // Pre-open the stream so the channel is ready for immediate send().
        ensure_stream_open();
    }

    if (on_state_) {
        on_state_(reliability_to_state_string(cfg_.reliability,
                                               stream_ready_.load(std::memory_order_acquire)));
    }
    return kOk;
}

void WebTransportDataChannel::close() noexcept {
    const bool was_opened = opened_.exchange(false, std::memory_order_acq_rel);

    stream_ready_.store(false, std::memory_order_release);

    if (stream_raw_) {
        stream_raw_->close();
        stream_raw_ = nullptr;
    }

    detach_peer_stream();

    // Unregister the session-level callbacks so a late inbound
    // datagram / peer stream cannot reach a closed channel.
    // Runs even when the channel was never open()ed: attach_session_callbacks()
    // is legal after open(cfg), so a configured-but-never-started channel
    // may still hold session subscriptions at destruction time.
    if (session_) {
        try {
            session_->set_on_datagram({});
            session_->set_on_peer_stream({});
        } catch (...) {
        }
    }

    if (was_opened && on_state_) {
        on_state_("closed");
    }
}

void WebTransportDataChannel::open(const plugins::DataChannelConfig& cfg) {
    cfg_ = cfg;

    // Initialise PR counters based on config.
    if (cfg.reliability == plugins::DataChannelReliability::kPartialReliableTTL
        && cfg.max_packet_lifetime_ms > 0) {
        using namespace std::chrono;
        pr_ttl_deadline_ = steady_clock::now()
            + milliseconds{cfg.max_packet_lifetime_ms};
    }
    if (cfg.reliability == plugins::DataChannelReliability::kPartialReliableCount) {
        pr_count_remaining_ = cfg.max_retransmits;
    }
}

void WebTransportDataChannel::send(plugins::BufferView data) {
    if (!opened_.load(std::memory_order_acquire)) return;
    if (!data.data() || data.size() == 0) return;

    // Lazy-open the stream for reliable/partial modes.
    if (cfg_.reliability != plugins::DataChannelReliability::kUnreliable) {
        if (!stream_ready_.load(std::memory_order_acquire)) {
            if (!ensure_stream_open()) return;
        }
    }

    switch (cfg_.reliability) {
        case plugins::DataChannelReliability::kReliableOrdered: {
            if (stream_raw_) {
                stream_raw_->send(
                    std::span<const std::uint8_t>{
                        static_cast<const std::uint8_t*>(data.data()),
                        data.size()});
            }
            break;
        }

        case plugins::DataChannelReliability::kUnreliable: {
            if (session_) {
                session_->send_datagram(
                    std::span<const std::uint8_t>{
                        static_cast<const std::uint8_t*>(data.data()),
                        data.size()});
            }
            break;
        }

        case plugins::DataChannelReliability::kPartialReliableTTL: {
            using namespace std::chrono;
            const auto now = steady_clock::now();
            if (now > pr_ttl_deadline_) {
                // TTL expired — reset the stream to signal truncation.
                if (stream_raw_) {
                    stream_raw_->reset();
                    stream_raw_ = nullptr;
                    stream_ready_.store(false, std::memory_order_release);
                }
                return;
            }
            if (stream_raw_) {
                stream_raw_->send(
                    std::span<const std::uint8_t>{
                        static_cast<const std::uint8_t*>(data.data()),
                        data.size()});
            }
            break;
        }

        case plugins::DataChannelReliability::kPartialReliableCount: {
            // Sentinel 0xFFFF means "unbounded" (cfg.max_retransmits == 0).
            constexpr std::uint16_t kUnbounded = 0xFFFF;
            if (pr_count_remaining_ == 0) {
                // Count exhausted — reset the stream.
                if (stream_raw_) {
                    stream_raw_->reset();
                    stream_raw_ = nullptr;
                    stream_ready_.store(false, std::memory_order_release);
                }
                return;
            }
            if (pr_count_remaining_ != kUnbounded) {
                --pr_count_remaining_;
            }
            if (stream_raw_) {
                stream_raw_->send(
                    std::span<const std::uint8_t>{
                        static_cast<const std::uint8_t*>(data.data()),
                        data.size()});
            }
            break;
        }
    }
}

void WebTransportDataChannel::set_on_message(
    plugins::DataMessageCallback cb) noexcept {
    on_msg_ = std::move(cb);
}

void WebTransportDataChannel::set_on_state(
    plugins::DataChannelStateCallback cb) noexcept {
    on_state_ = std::move(cb);
}

void WebTransportDataChannel::attach_session_callbacks() noexcept {
    if (!session_) return;
    try {
        session_->set_on_datagram(
            [this](std::span<const std::uint8_t> bytes) noexcept {
                this->on_session_datagram(bytes);
            });
        session_->set_on_peer_stream(
            [this](IBidirectionalStream* stream) noexcept {
                this->on_peer_stream(stream);
            });
    } catch (...) {
        // Registration is best-effort: allocation failure in
        // std::function leaves the channel without inbound
        // routing but must not throw across the noexcept seam.
    }
}

bool WebTransportDataChannel::ensure_stream_open() noexcept {
    if (!session_) return false;
    if (stream_ready_.load(std::memory_order_acquire)) return true;

    // Check session state.
    if (session_->state() != WebTransportState::kConnected) return false;

    // Open a new bidirectional stream.  The stream is owned by the
    // session (in real impl) or by the test harness (in tests) — we
    // hold a non-owning raw pointer here.  The session outlives us
    // (the engine owns both), so this is safe.
    IBidirectionalStream* s = session_->open_bidi_stream();
    if (!s) return false;

    // Install the recv callback — capture `this` to dispatch into
    // the channel's on_stream_data.
    s->set_on_data(
        [this](plugins::BufferView data) noexcept {
            this->on_stream_data(data);
        });

    stream_raw_ = s;
    stream_ready_.store(true, std::memory_order_release);
    return true;
}

void WebTransportDataChannel::on_stream_data(plugins::BufferView data) noexcept {
    if (on_msg_) {
        on_msg_(data);
    }
}

void WebTransportDataChannel::on_stream_closed() noexcept {
    stream_ready_.store(false, std::memory_order_release);
    if (on_state_) {
        on_state_("closed");
    }
}

void WebTransportDataChannel::on_peer_stream(
    IBidirectionalStream* stream) noexcept {
    if (!opened_.load(std::memory_order_acquire)) return;
    if (stream == nullptr) return;
    // Datagram mode has no stream surface — ignore peer streams.
    if (cfg_.reliability == plugins::DataChannelReliability::kUnreliable) {
        return;
    }
    detach_peer_stream();
    peer_stream_raw_ = stream;
    peer_header_done_ = false;
    peer_header_pending_.clear();
    try {
        stream->set_on_data(
            [this](plugins::BufferView data) noexcept {
                this->on_peer_stream_data(data);
            });
    } catch (...) {
        peer_stream_raw_ = nullptr;
        peer_header_pending_.clear();
    }
}

void WebTransportDataChannel::on_session_datagram(
    std::span<const std::uint8_t> bytes) noexcept {
    if (!opened_.load(std::memory_order_acquire)) return;
    if (bytes.empty()) return;
    // Stream modes ignore datagrams — only kUnreliable delivers them.
    if (cfg_.reliability != plugins::DataChannelReliability::kUnreliable) {
        return;
    }
    if (on_msg_) {
        try {
            on_msg_(plugins::BufferView{bytes.data(), bytes.size()});
        } catch (...) {
        }
    }
}

void WebTransportDataChannel::on_peer_stream_data(
    plugins::BufferView data) noexcept {
    if (!opened_.load(std::memory_order_acquire)) return;
    if (data.data() == nullptr || data.size() == 0) return;

    // Strip the WebTransport stream-type header (signal varint 0x41 +
    // session-id varint, RFC draft-ietf-webtrans-http3 §4.2) before
    // delivering payload bytes.  The header may be split across
    // multiple on_data invocations, so buffer until it decodes.
    if (!peer_header_done_) {
        try {
            peer_header_pending_.insert(peer_header_pending_.end(),
                                        data.begin(), data.end());
        } catch (...) {
            return;
        }
        std::size_t pos = 0;
        std::uint64_t signal = 0;
        if (!try_decode_varint(peer_header_pending_, pos, signal)) {
            return;  // split header — wait for more bytes
        }
        if (signal != 0x41u) {
            // Not a WebTransport data stream after all — stop parsing
            // and deliver nothing further on this stream.
            peer_header_done_ = true;
            peer_header_pending_.clear();
            return;
        }
        std::uint64_t session_id = 0;
        if (!try_decode_varint(peer_header_pending_, pos, session_id)) {
            return;  // split header — wait for more bytes
        }
        (void)session_id;
        peer_header_done_ = true;
        if (pos >= peer_header_pending_.size()) {
            peer_header_pending_.clear();
            return;  // header only — wait for payload bytes
        }
        std::vector<std::uint8_t> payload(
            peer_header_pending_.begin() +
                static_cast<std::ptrdiff_t>(pos),
            peer_header_pending_.end());
        peer_header_pending_.clear();
        on_stream_data(plugins::BufferView{payload.data(), payload.size()});
        return;
    }

    on_stream_data(data);
}

void WebTransportDataChannel::detach_peer_stream() noexcept {
    if (peer_stream_raw_) {
        try {
            peer_stream_raw_->set_on_data({});
        } catch (...) {
        }
        peer_stream_raw_ = nullptr;
    }
    peer_header_done_ = false;
    try {
        peer_header_pending_.clear();
    } catch (...) {
    }
}

// ===========================================================================
// WebTransportDataChannelFactory
// ===========================================================================

plugins::IDataChannel* WebTransportDataChannelFactory::create() const {
    // Negotiation probe: the engine resolves this factory once at
    // init_datachannel_registry() to confirm the backend is available.
    // The channel is unbound (no session) and reports kErrNotReady
    // until replaced by a session-bound channel from
    // create(IWebTransportSession*).
    return new WebTransportDataChannel(nullptr);
}

} // namespace nimrtc::web_transport
