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
    if (!opened_.load(std::memory_order_acquire)) return;

    opened_.store(false, std::memory_order_release);
    stream_ready_.store(false, std::memory_order_release);

    if (stream_raw_) {
        stream_raw_->close();
        stream_raw_ = nullptr;
    }

    if (on_state_) {
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

// ===========================================================================
// WebTransportDataChannelFactory
// ===========================================================================

plugins::IDataChannel* WebTransportDataChannelFactory::create() const {
    // Return a stub channel with no session.
    return new WebTransportDataChannel(nullptr);
}

} // namespace nimrtc::web_transport
