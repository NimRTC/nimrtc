/**
 * @file src/web_transport/src/web_transport_data_channel.cpp
 * @brief WebTransportDataChannel implementation
 *        (v0.14.0-beta.1 DC-1 land; v1.2.0 WP-B / WP-C / WP-D / WP-E).
 *
 * Implements `plugins::IDataChannel` on top of `IWebTransportSession`.
 * See `web_transport_data_channel.hpp` for design notes.
 *
 * The four capability work packages of the v1.2.0 data-channel plan
 * map onto this file as follows:
 *
 *   - WP-B (message boundary): `detail::encode_frame()` /
 *     `detail::FrameReader` — see `web_transport_frame.hpp`.
 *   - WP-C (channel isolation): the channel *subscribes* to the
 *     session's multicast inbound routes and removes only its own
 *     subscription on close; in framed mode the frame's channel label
 *     decides the owner of a payload.
 *   - WP-D (partial reliability): the TTL and the retransmit budget
 *     are per message; an expired message is dropped and the channel
 *     stays open.
 *   - WP-E (priority): the channel registers an outbound priority
 *     source; the session drains the sources in priority order.
 */

#include <nimrtc/web_transport/web_transport_data_channel.hpp>
#include <nimrtc/web_transport/web_transport_state.hpp>

#include "web_transport_frame.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <set>

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

// "Unlimited" sentinel for the per-message retransmit budget.
constexpr std::uint16_t kUnlimitedRetries = 0xFFFF;

// The ARQ retransmission timer (v1.2.0, WP-D4).  A message that is
// on the wire but not acknowledged within this window is offered to
// the transport again.  The value is a compromise: long enough not to
// retransmit a message that is merely slow, short enough that the
// partial-reliability modes keep their promise within one TTL.
constexpr std::chrono::milliseconds kArqRetransmitTimeout{250};

// The bound of the de-duplication window (v1.2.0, WP-D4): the number
// of sequence numbers the receiver remembers.  A retransmission that
// arrives later than this window is delivered again — the window is
// sized for the in-flight window of the sender, not for an unbounded
// history.
constexpr std::size_t kArqMaxRememberedSeqs = 4096;

// The WebTransport stream-type signal (RFC draft-ietf-webtrans-http3
// §4.2): the stream starts with the varint 0x41 (the "WebTransport
// data stream" type) followed by the session-id varint.
//
// The signal is accepted in the two encodings that occur on the wire:
//   - the *canonical* QUIC varint of 0x41, i.e. the two bytes
//     0x40 0x41 (0x41 == 65 > 63, so a one-byte varint cannot carry
//     it), and
//   - the *single byte* 0x41, which is what the Chrome stack writes
//     (and what the interop harness replays).
constexpr std::uint8_t kWebTransportStreamSignal = 0x41u;
// The first byte of the canonical two-byte varint encoding of 0x41.
constexpr std::uint8_t kWebTransportStreamSignalPrefix = 0x40u;

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

// The outcome of the WebTransport stream-header probe.
enum class HeaderProbe : std::uint8_t {
    kNoHeader,   // the stream carries no WebTransport header
    kHeader,     // a header was found; `consumed` holds its length
    kIncomplete  // the header is split across chunks — wait for more
};

// Probe the head of a peer stream for the WebTransport stream-type
// header (the signal 0x41 followed by the session-id varint).
//
// The probe is re-run on the accumulated buffer until it succeeds, so
// a header split across chunks is handled without extra state: the
// buffer always starts at the header.
//
// A first byte that is not the signal means "no WebTransport header" —
// a NimRTC peer starts the stream with the frame header (whose first
// byte is the frame kind, i.e. 0x01 for a data frame), and a raw-mode
// peer may start with arbitrary payload bytes.
HeaderProbe probe_webtransport_header(const std::vector<std::uint8_t>& buf,
                                      std::size_t& consumed) noexcept {
    consumed = 0;
    if (buf.empty()) return HeaderProbe::kIncomplete;

    std::size_t signal_len = 0;
    if (buf[0] == kWebTransportStreamSignal) {
        signal_len = 1;  // the single-byte form
    } else if (buf[0] == kWebTransportStreamSignalPrefix) {
        // The canonical form starts with 0x40: wait for the second
        // byte before deciding (the header may be split across chunks).
        if (buf.size() < 2) return HeaderProbe::kIncomplete;
        if (buf[1] != kWebTransportStreamSignal) return HeaderProbe::kNoHeader;
        signal_len = 2;  // the canonical two-byte varint form
    } else {
        return HeaderProbe::kNoHeader;
    }

    // The session-id varint follows the signal.
    std::size_t pos = signal_len;
    std::uint64_t session_id = 0;
    if (!try_decode_varint(buf, pos, session_id)) {
        return HeaderProbe::kIncomplete;
    }
    consumed = pos;
    return HeaderProbe::kHeader;
}

} // anonymous namespace

// ===========================================================================
// WebTransportDataChannel::State — the module-internal per-channel state
// ===========================================================================
//
// Kept out of the public header so that the module-internal frame codec
// (`detail::FrameReader`) does not leak into the installed API surface.
// ===========================================================================

struct WebTransportDataChannel::State {
    /** One queued outbound message (WP-D / WP-E). */
    struct Pending {
        /** The wire bytes (framed or raw). */
        std::vector<std::uint8_t> bytes;
        /** The per-message deadline (PR-TTL mode only). */
        std::chrono::steady_clock::time_point deadline{};
        /** True when `deadline` is meaningful. */
        bool has_deadline = false;
        /** Remaining retransmission budget (PR-count mode);
         *  `kUnlimitedRetries` means "unlimited". */
        std::uint16_t retries_left = 0;
        /** True when `retries_left` is a real budget (PR-count mode).
         *
         *  The budget is *mode-specific*: in the PR-TTL mode the
         *  message lives until its deadline passes, and in the
         *  reliable / unreliable modes there is no budget at all —
         *  a refused message is simply requeued and offered again on
         *  the next flush.  Without this flag the `retries_left == 0`
         *  default of the non-count modes would read as "budget
         *  exhausted" and drop the message on the first refusal —
         *  the v1.1.0 "the channel dies" defect in a new shape. */
        bool has_budget = false;
        /** The ARQ sequence number of the message (WP-D4).  Zero when
         *  the message does not take part in the application-level
         *  retransmission protocol. */
        std::uint32_t seq = 0;
    };

    /** One message that is on the wire but not acknowledged yet
     *  (v1.2.0, WP-D4 — the ARQ in-flight window). */
    struct InFlight {
        /** The wire bytes (the framed message, ready to be resent). */
        std::vector<std::uint8_t> bytes;
        /** The per-message deadline (PR-TTL mode only). */
        std::chrono::steady_clock::time_point deadline{};
        /** True when `deadline` is meaningful. */
        bool has_deadline = false;
        /** Remaining retransmission budget (PR-count mode). */
        std::uint16_t retries_left = 0;
        /** True when `retries_left` is a real budget. */
        bool has_budget = false;
        /** When the message was last handed to the transport. */
        std::chrono::steady_clock::time_point last_sent{};
    };

    /** Per-stream inbound state: the bytes that arrived but were not
     *  consumed yet, the WebTransport stream-type header state and the
     *  frame reader (framed mode). */
    struct InboundStream {
        /** Bytes received but not yet consumed. */
        std::vector<std::uint8_t> pending;
        /** The frame reader (framed mode only). */
        std::unique_ptr<detail::FrameReader> reader;
        /** True once the stream-type header was resolved. */
        bool header_done = false;
        /** The stream's multicast data subscription (framed mode). */
        StreamSubscriptionId data_sub = 0;
    };

    /** The outbound queue (FIFO within the channel). */
    std::deque<Pending> tx;
    /** The inbound state, one entry per stream (std::map: the node
     *  based container keeps the references stable while a user
     *  callback runs). */
    std::map<IBidirectionalStream*, InboundStream> inbound;
    /** Scratch reader for inbound datagrams (framed mode): a datagram
     *  carries exactly one frame, so the reader is reset per datagram. */
    std::unique_ptr<detail::FrameReader> dgram_reader;
    /** The messages that are on the wire but not acknowledged yet
     *  (WP-D4). */
    std::map<std::uint32_t, InFlight> inflight;
    /** The next ARQ sequence number of this channel (WP-D4). */
    std::uint32_t next_seq = 1;
    /** The sequence numbers already received (the de-duplication
     *  window, WP-D4). */
    std::set<std::uint32_t> received_seqs;
    /** The insertion order of `received_seqs` (the FIFO that bounds
     *  the de-duplication window). */
    std::deque<std::uint32_t> received_order;
    /** Counters (test-only surface). */
    std::uint64_t sent = 0;
    std::uint64_t dropped = 0;
    std::uint64_t retransmitted = 0;
};

/** One decoded inbound message, ready to be delivered to the user. */
struct InboundDelivery {
    /** The frame's channel label (the identity). */
    std::string label;
    /** The frame's payload (copied out of the reader's buffer). */
    std::vector<std::uint8_t> payload;
};

// ===========================================================================
// WebTransportDataChannel
// ===========================================================================

WebTransportDataChannel::WebTransportDataChannel(IWebTransportSession* session) noexcept
    : session_(session) {
    try {
        state_ = std::make_unique<State>();
    } catch (...) {
        // A channel without its state cannot queue or decode; the
        // operations below degrade to no-ops.  Never throws across the
        // noexcept seam.
    }
}

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
    closed_.store(false, std::memory_order_release);

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
    closed_.store(true, std::memory_order_release);

    stream_ready_.store(false, std::memory_order_release);

    // Remove this channel's session routes *before* dropping the streams,
    // so a late inbound datagram / peer stream cannot reach a closed
    // channel.  Only this channel's own subscriptions are removed — the
    // siblings keep theirs (v1.2.0, WP-C4).
    remove_session_routes();

    if (stream_raw_) {
        try {
            stream_raw_->close();
        } catch (...) {
        }
        stream_raw_ = nullptr;
    }

    if (peer_stream_raw_) {
        try {
            peer_stream_raw_->set_on_data({});
        } catch (...) {
        }
        peer_stream_raw_ = nullptr;
    }

    if (state_) {
        std::lock_guard<std::mutex> l{tx_mutex_};
        state_->tx.clear();
        // The ARQ state dies with the channel (WP-D4): the in-flight
        // messages are never acknowledged, and the de-duplication
        // window must not outlive the channel.
        state_->inflight.clear();
        state_->received_seqs.clear();
        state_->received_order.clear();
        // The per-stream data subscriptions outlive the channel unless
        // they are removed: the stream is owned by the session and may
        // call the handler after this channel is gone.  Unsubscribe
        // before dropping the map (WP-C4 — only our own routes go).
        for (auto& entry : state_->inbound) {
            if (entry.second.data_sub == 0) continue;
            if (entry.first) {
                try {
                    entry.first->unsubscribe_on_data(entry.second.data_sub);
                } catch (...) {
                }
            }
            entry.second.data_sub = 0;
        }
        state_->inbound.clear();
    }

    if (was_opened && on_state_) {
        on_state_("closed");
    }
}

void WebTransportDataChannel::open(const plugins::DataChannelConfig& cfg) {
    cfg_ = cfg;

    // The framing mode and the priority are only known here, so the
    // session routes (and the outbound priority source) are wired now.
    apply_session_routes();
}

void WebTransportDataChannel::send(plugins::BufferView data) {
    if (!opened_.load(std::memory_order_acquire)) return;
    if (!data.data() || data.size() == 0) return;
    if (!state_) return;

    const auto* bytes = static_cast<const std::uint8_t*>(data.data());
    const std::span<const std::uint8_t> payload{bytes, data.size()};

    State::Pending pending;
    try {
        if (cfg_.framing == plugins::DataChannelFraming::kFramed) {
            // WP-D4: the partial-reliability modes run an
            // application-level retransmission protocol, so the frame
            // carries a sequence number the peer acknowledges.  The
            // reliable mode does not need it (QUIC retransmits the
            // stream itself) and the unreliable mode rides datagrams.
            std::uint32_t seq = 0;
            if (arq_enabled()) {
                std::lock_guard<std::mutex> l{tx_mutex_};
                seq = state_->next_seq++;
                if (seq == 0) seq = state_->next_seq++;  // 0 means "no ARQ"
            }
            pending.seq = seq;
            // WP-B: the frame carries the channel label, so the peer can
            // route the message to the right channel (WP-C3).
            pending.bytes = detail::encode_frame(cfg_.label, payload,
                                                 detail::FrameKind::kData, seq);
        } else {
            pending.bytes.assign(payload.begin(), payload.end());
        }
    } catch (...) {
        return;
    }

    // WP-D: the partial-reliability budget is per *message*.
    if (cfg_.reliability == plugins::DataChannelReliability::kPartialReliableTTL
        && cfg_.max_packet_lifetime_ms > 0) {
        pending.has_deadline = true;
        pending.deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds{cfg_.max_packet_lifetime_ms};
    }
    if (cfg_.reliability == plugins::DataChannelReliability::kPartialReliableCount) {
        // `max_retransmits == 0` means *unlimited* (the documented
        // meaning in `DataChannelConfig`), not "exhausted".
        pending.retries_left = (cfg_.max_retransmits == 0)
            ? kUnlimitedRetries
            : cfg_.max_retransmits;
        pending.has_budget = true;
    }

    {
        std::lock_guard<std::mutex> l{tx_mutex_};
        state_->tx.push_back(std::move(pending));
    }

    // Drain through the session so the pending sends of *all* the
    // channels of this session are offered to the transport in priority
    // order (v1.2.0, WP-E2).  When the session does not implement the
    // priority surface, the channel drains its own queue.
    if (session_ && prio_sub_ != 0) {
        session_->flush_outbound();
    } else {
        flush_outbound();
    }
}

void WebTransportDataChannel::set_on_message(
    plugins::DataMessageCallback cb) noexcept {
    on_msg_ = std::move(cb);
}

void WebTransportDataChannel::set_on_message_ex(
    plugins::DataChannelMessageCallback cb) noexcept {
    on_msg_ex_ = std::move(cb);
}

void WebTransportDataChannel::set_on_state(
    plugins::DataChannelStateCallback cb) noexcept {
    on_state_ = std::move(cb);
}

void WebTransportDataChannel::attach_session_callbacks() noexcept {
    apply_session_routes();
}

// ---------------------------------------------------------------------------
// Session routes (v1.2.0, WP-C2 / WP-C4 / WP-E2)
// ---------------------------------------------------------------------------

void WebTransportDataChannel::apply_session_routes() noexcept {
    if (!session_) return;

    const bool framed = (cfg_.framing == plugins::DataChannelFraming::kFramed);
    const Routes wanted = framed ? Routes::kMulticast : Routes::kLegacy;

    if (routes_ != Routes::kNone && routes_ != wanted) {
        // The framing mode changed (the channel was configured after it
        // was created): drop the old routes first.
        remove_session_routes();
    }
    if (routes_ == Routes::kNone) {
        if (framed) {
            // Multicast: every channel of the session sees the inbound
            // traffic and picks the frames addressed to it.  The old
            // single-slot API made the last channel win and let a
            // closing channel deafen its siblings (WP-C2 / WP-C4).
            dgram_sub_ = session_->subscribe_on_datagram(
                [this](std::span<const std::uint8_t> bytes) noexcept {
                    this->on_session_datagram(bytes);
                });
            peer_sub_ = session_->subscribe_on_peer_stream(
                [this](IBidirectionalStream* stream) noexcept {
                    this->on_peer_stream(stream);
                });
        } else {
            // Raw mode: the legacy single-slot surface (a byte stream
            // carries no channel identity, so there is nothing to
            // demultiplex).
            session_->set_on_datagram(
                [this](std::span<const std::uint8_t> bytes) noexcept {
                    this->on_session_datagram(bytes);
                });
            session_->set_on_peer_stream(
                [this](IBidirectionalStream* stream) noexcept {
                    this->on_peer_stream(stream);
                });
        }
        routes_ = wanted;
    }

    // WP-E2: the outbound priority source.  Lower value = higher
    // priority; the session drains the sources in that order.
    if (prio_sub_ != 0) {
        session_->unregister_priority_source(prio_sub_);
        prio_sub_ = 0;
    }
    prio_sub_ = session_->register_priority_source(
        static_cast<int>(cfg_.priority),
        [this]() noexcept { this->flush_outbound(); });
}

void WebTransportDataChannel::remove_session_routes() noexcept {
    if (!session_) {
        routes_ = Routes::kNone;
        return;
    }
    if (routes_ == Routes::kMulticast) {
        if (dgram_sub_ != 0) {
            session_->unsubscribe_on_datagram(dgram_sub_);
            dgram_sub_ = 0;
        }
        if (peer_sub_ != 0) {
            session_->unsubscribe_on_peer_stream(peer_sub_);
            peer_sub_ = 0;
        }
    } else if (routes_ == Routes::kLegacy) {
        // The legacy slots are shared: only clear them when this
        // channel is the one that installed them.
        session_->set_on_datagram({});
        session_->set_on_peer_stream({});
    }
    if (prio_sub_ != 0) {
        session_->unregister_priority_source(prio_sub_);
        prio_sub_ = 0;
    }
    routes_ = Routes::kNone;
}

// ---------------------------------------------------------------------------
// Outbound path (WP-D / WP-E2)
// ---------------------------------------------------------------------------

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
    // the channel's inbound path.
    try {
        s->set_on_data(
            [this, s](plugins::BufferView data) noexcept {
                this->on_peer_stream_data(s, data);
            });
    } catch (...) {
        return false;
    }

    stream_raw_ = s;
    stream_ready_.store(true, std::memory_order_release);
    return true;
}

bool WebTransportDataChannel::offer_to_transport(
    const std::vector<std::uint8_t>& bytes) noexcept {
    if (bytes.empty()) return true;
    const std::span<const std::uint8_t> payload{bytes.data(), bytes.size()};

    if (cfg_.reliability == plugins::DataChannelReliability::kUnreliable) {
        if (!session_) return false;
        return session_->send_datagram(payload) == kOk;
    }

    if (!stream_ready_.load(std::memory_order_acquire)) {
        if (!ensure_stream_open()) return false;
    }
    if (!stream_raw_) return false;
    return stream_raw_->send(payload) == kOk;
}

void WebTransportDataChannel::flush_outbound() noexcept {
    if (!state_) return;
    if (!opened_.load(std::memory_order_acquire)) return;

    // WP-D4: the ARQ sweep runs first — the retransmission timer must
    // fire even when the outbound queue is empty (a message that is
    // waiting for its acknowledgement *is* pending traffic).
    arq_sweep();

    for (;;) {
        State::Pending pending;
        {
            std::lock_guard<std::mutex> l{tx_mutex_};
            if (state_->tx.empty()) return;
            pending = std::move(state_->tx.front());
            state_->tx.pop_front();
        }

        // WP-D1: the TTL belongs to the message.  An expired message is
        // dropped — the channel and its stream stay alive.
        if (pending.has_deadline
            && std::chrono::steady_clock::now() > pending.deadline) {
            std::lock_guard<std::mutex> l{tx_mutex_};
            ++state_->dropped;
            continue;
        }

        // WP-D4: the message enters the in-flight window *before* it
        // is offered to the transport.  The acknowledgement can arrive
        // while the transport call is still on the stack (a
        // synchronous loopback delivers the frame and the answer
        // straight back), and it must find the message there.
        if (pending.seq != 0) {
            State::InFlight f;
            f.bytes        = pending.bytes;
            f.deadline     = pending.deadline;
            f.has_deadline = pending.has_deadline;
            f.retries_left = pending.retries_left;
            f.has_budget   = pending.has_budget;
            f.last_sent    = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> l{tx_mutex_};
            state_->inflight[pending.seq] = std::move(f);
        }

        if (offer_to_transport(pending.bytes)) {
            std::lock_guard<std::mutex> l{tx_mutex_};
            ++state_->sent;
            continue;
        }

        // The transport refused the message: it never reached the
        // wire, so it leaves the in-flight window and goes back to the
        // queue.
        if (pending.seq != 0) {
            std::lock_guard<std::mutex> l{tx_mutex_};
            state_->inflight.erase(pending.seq);
        }

        // The transport refused the message: requeue it and account one
        // retransmission against the per-message budget (WP-D2).
        //
        // The budget only exists in the PR-count mode.  In the PR-TTL
        // mode the message lives until its deadline passes (checked
        // above); in the reliable / unreliable modes there is no
        // per-message budget at all and the message is offered again
        // on the next flush.
        if (pending.has_budget) {
            const bool unlimited = (pending.retries_left == kUnlimitedRetries);
            if (!unlimited) {
                if (pending.retries_left == 0) {
                    // The budget is exhausted — the message is dropped,
                    // but the channel stays open.
                    std::lock_guard<std::mutex> l{tx_mutex_};
                    ++state_->dropped;
                    continue;
                }
                --pending.retries_left;
            }
        }
        {
            std::lock_guard<std::mutex> l{tx_mutex_};
            state_->tx.push_front(std::move(pending));
        }
        // The transport is not ready — stop here and retry on the next
        // flush (the engine's tick).
        return;
    }
}

// ---------------------------------------------------------------------------
// Application-level ARQ (v1.2.0, WP-D4)
// ---------------------------------------------------------------------------

bool WebTransportDataChannel::arq_enabled() const noexcept {
    // The application-level retransmission protocol needs two things a
    // QUIC stream does not give the application: a message boundary
    // (the frame) and a per-message retry hook (the sequence number
    // and the acknowledgement frame).
    //
    //   - the framed mode carries the boundary and the sequence
    //     number;
    //   - the two partial-reliability modes are the ones that need a
    //     retry hook: the reliable mode rides QUIC's own
    //     retransmission and the unreliable mode rides datagrams.
    if (cfg_.framing != plugins::DataChannelFraming::kFramed) return false;
    return cfg_.reliability == plugins::DataChannelReliability::kPartialReliableTTL
        || cfg_.reliability == plugins::DataChannelReliability::kPartialReliableCount;
}

void WebTransportDataChannel::handle_arq_data(
    std::uint32_t seq,
    std::string_view label,
    std::span<const std::uint8_t> payload) noexcept {
    if (!state_) return;

    bool duplicate = false;
    {
        std::lock_guard<std::mutex> l{tx_mutex_};
        duplicate = state_->received_seqs.find(seq) != state_->received_seqs.end();
        if (!duplicate) {
            state_->received_seqs.insert(seq);
            state_->received_order.push_back(seq);
            // The de-duplication window is bounded: the oldest
            // sequence numbers fall out of it.
            while (state_->received_order.size() > kArqMaxRememberedSeqs) {
                state_->received_seqs.erase(state_->received_order.front());
                state_->received_order.pop_front();
            }
        }
    }

    if (!duplicate) {
        deliver(label, payload);
    }

    // The acknowledgement is sent even for a duplicate: the previous
    // ACK may have been the frame that got lost, and the peer is
    // waiting for it.
    send_ack(seq, label);
}

void WebTransportDataChannel::handle_arq_ack(std::uint32_t seq) noexcept {
    if (!state_) return;
    std::lock_guard<std::mutex> l{tx_mutex_};
    // The message is delivered — it leaves the in-flight window.
    state_->inflight.erase(seq);
}

void WebTransportDataChannel::send_ack(std::uint32_t seq,
                                       std::string_view label) noexcept {
    if (!session_) return;

    std::vector<std::uint8_t> frame;
    try {
        frame = detail::encode_frame(label, {}, detail::FrameKind::kAck, seq);
    } catch (...) {
        return;
    }

    // The ACK travels on the channel's own outbound stream: the peer
    // reads it from the stream it opened for us, which is the mirror
    // image of the stream the acknowledged message came in on.
    if (!stream_ready_.load(std::memory_order_acquire)) {
        if (!ensure_stream_open()) return;
    }
    if (!stream_raw_) return;
    try {
        stream_raw_->send(
            std::span<const std::uint8_t>{frame.data(), frame.size()});
    } catch (...) {
    }
}

void WebTransportDataChannel::arq_sweep() noexcept {
    if (!state_) return;

    const auto now = std::chrono::steady_clock::now();

    // Phase 1 (under the lock): decide what to retransmit and what to
    // drop.  The transport calls happen outside the lock — a
    // synchronous loopback delivers the retransmission straight into
    // the inbound path, which takes the lock itself.
    std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> to_send;
    {
        std::lock_guard<std::mutex> l{tx_mutex_};
        for (auto it = state_->inflight.begin(); it != state_->inflight.end();) {
            State::InFlight& f = it->second;

            // WP-D1: the TTL belongs to the message.  An expired
            // message is dropped — the channel and its stream stay
            // alive.
            if (f.has_deadline && now > f.deadline) {
                ++state_->dropped;
                it = state_->inflight.erase(it);
                continue;
            }

            if (now - f.last_sent < kArqRetransmitTimeout) {
                ++it;
                continue;
            }

            // WP-D2: the retransmission budget belongs to the
            // message.  An exhausted budget drops the message — the
            // channel stays open.
            if (f.has_budget && f.retries_left == 0) {
                ++state_->dropped;
                it = state_->inflight.erase(it);
                continue;
            }
            if (f.has_budget && f.retries_left != kUnlimitedRetries) {
                --f.retries_left;
            }

            f.last_sent = now;
            to_send.emplace_back(it->first, f.bytes);
            // The retransmission is counted when it is *scheduled*:
            // the transport call happens outside the lock, and the
            // acknowledgement it triggers may already have removed the
            // message from the in-flight window.
            ++state_->retransmitted;
            ++it;
        }
    }

    // Phase 2 (outside the lock): hand the retransmissions to the
    // transport.
    for (const auto& entry : to_send) {
        if (!offer_to_transport(entry.second)) continue;
    }
}

// ---------------------------------------------------------------------------
// Inbound path (WP-B / WP-C3)
// ---------------------------------------------------------------------------

void WebTransportDataChannel::on_session_datagram(
    std::span<const std::uint8_t> bytes) noexcept {
    // The guard is `closed_`, not `opened_`: the session routes are
    // wired by `open(cfg)`, which the engine calls *before* `open()`.
    // A datagram that arrives in that window must not be lost.
    if (closed_.load(std::memory_order_acquire)) return;
    if (bytes.empty()) return;
    // Stream modes ignore datagrams — only kUnreliable delivers them.
    if (cfg_.reliability != plugins::DataChannelReliability::kUnreliable) {
        return;
    }

    if (cfg_.framing != plugins::DataChannelFraming::kFramed) {
        deliver(cfg_.label, bytes);
        return;
    }

    // Framed mode: a datagram carries exactly one frame.
    if (!state_) return;
    if (!state_->dgram_reader) {
        try {
            state_->dgram_reader =
                std::make_unique<detail::FrameReader>(detail::kDefaultMaxFramePayload);
        } catch (...) {
            return;
        }
    }
    auto& reader = *state_->dgram_reader;
    reader.reset();
    if (!reader.feed(bytes)) {
        fail_protocol(reader.error());
        return;
    }
    if (!reader.has_frame()) {
        // A partial frame in a datagram is a protocol error: the frame
        // boundaries must not span datagrams.
        fail_protocol("incomplete data-channel frame in a datagram");
        return;
    }
    const std::string_view label{reader.label()};
    // The frame is copied out and consumed *before* it is handled: the
    // handling (the user callback, the acknowledgement) can re-enter
    // this path — a synchronous loopback delivers straight back into
    // the reader — and a frame that is still "current" would be
    // delivered twice.
    const std::string label_copy{label};
    std::vector<std::uint8_t> payload_copy(reader.payload().begin(),
                                           reader.payload().end());
    const detail::FrameKind kind = reader.kind();
    const std::uint32_t seq = reader.seq();
    reader.consume();
    if (label_copy == cfg_.label) {
        // WP-D4: the frame kind decides what the frame is — a user
        // message, an ARQ message or an acknowledgement.
        if (kind == detail::FrameKind::kAck) {
            handle_arq_ack(seq);
        } else if (seq != 0) {
            handle_arq_data(seq, label_copy, payload_copy);
        } else {
            deliver(label_copy, payload_copy);
        }
    }
}

void WebTransportDataChannel::on_peer_stream(IBidirectionalStream* stream) noexcept {
    // See `on_session_datagram`: the peer-stream route is wired before
    // `open()`, so a peer stream that arrives in that window must be
    // adopted (and subscribed to) — not dropped.
    if (closed_.load(std::memory_order_acquire)) return;
    if (stream == nullptr) return;
    if (!state_) return;

    if (cfg_.reliability == plugins::DataChannelReliability::kUnreliable) {
        // Datagram mode has no stream surface — ignore peer streams.
        return;
    }

    if (cfg_.framing != plugins::DataChannelFraming::kFramed) {
        // Raw mode: the legacy single-owner path.  The stream is
        // adopted by this channel and the payload is delivered as it
        // arrives (no message boundary).
        if (peer_stream_raw_ && peer_stream_raw_ != stream) {
            try {
                peer_stream_raw_->set_on_data({});
            } catch (...) {
            }
        }
        peer_stream_raw_ = stream;
        try {
            stream->set_on_data(
                [this, stream](plugins::BufferView data) noexcept {
                    this->on_peer_stream_data(stream, data);
                });
        } catch (...) {
            peer_stream_raw_ = nullptr;
        }
        return;
    }

    // Framed mode: every channel of the session watches the stream and
    // picks the frames addressed to it.  `subscribe_on_data` adds a
    // handler — it does not replace the other channels' handlers.
    State::InboundStream& in = state_->inbound[stream];
    if (in.data_sub != 0) return;  // already subscribed
    if (!in.reader) {
        try {
            in.reader = std::make_unique<detail::FrameReader>(
                detail::kDefaultMaxFramePayload);
        } catch (...) {
            return;
        }
    }
    in.header_done = false;
    in.pending.clear();

    const StreamSubscriptionId sub = stream->subscribe_on_data(
        [this, stream](std::span<const std::uint8_t> bytes) noexcept {
            this->on_peer_stream_data(stream,
                                      plugins::BufferView{bytes.data(), bytes.size()});
        });
    if (sub == 0) {
        // The stream does not implement the multicast surface — fall
        // back to the primary slot (single owner semantics).
        try {
            stream->set_on_data(
                [this, stream](plugins::BufferView data) noexcept {
                    this->on_peer_stream_data(stream, data);
                });
        } catch (...) {
        }
    } else {
        in.data_sub = sub;
    }
}

void WebTransportDataChannel::on_peer_stream_data(IBidirectionalStream* stream,
                                                  plugins::BufferView data) noexcept {
    if (closed_.load(std::memory_order_acquire)) return;
    if (!state_) return;
    if (data.data() == nullptr || data.size() == 0) return;

    // std::map keeps the reference stable while a user callback runs.
    State::InboundStream& in = state_->inbound[stream];

    // Accumulate the bytes that were not consumed yet.
    try {
        const auto* base = reinterpret_cast<const std::uint8_t*>(data.data());
        in.pending.insert(in.pending.end(), base, base + data.size());
    } catch (...) {
        return;
    }

    // ---- WebTransport stream-type header ---------------------------------
    //
    // A Chrome peer starts every data stream with the signal 0x41 (the
    // "WebTransport data stream" type) followed by the session-id
    // varint.  A NimRTC peer starts the stream directly with the frame
    // header, whose first byte is the frame kind (0x01 for a data
    // frame) — so a first byte that is not the signal means "no
    // WebTransport header", not "not a data stream".
    // Both are handled here.
    //
    // `skip` is the number of bytes to drop from *this* chunk: the
    // header can only appear at the front of the first chunk, so the
    // offset must not be carried over to the following chunks (that
    // would silently drop the head of every subsequent payload).
    std::size_t skip = 0;
    if (!in.header_done) {
        std::size_t consumed = 0;
        const HeaderProbe probe = probe_webtransport_header(in.pending, consumed);
        if (probe == HeaderProbe::kIncomplete) {
            return;  // split header — wait for more bytes
        }
        if (probe == HeaderProbe::kHeader) {
            skip = consumed;
        }
        // kNoHeader: the bytes are payload as-is.
        in.header_done = true;
    }

    // ---- The payload ------------------------------------------------------
    if (in.pending.size() <= skip) {
        in.pending.clear();
        return;
    }
    const std::span<const std::uint8_t> body{
        in.pending.data() + skip,
        in.pending.size() - skip};
    in.pending.clear();

    if (cfg_.framing != plugins::DataChannelFraming::kFramed) {
        // Raw mode: the payload is delivered as it arrives.
        deliver(cfg_.label, body);
        return;
    }

    if (!in.reader) return;
    if (!in.reader->feed(body)) {
        fail_protocol(in.reader->error());
        return;
    }
    while (in.reader->has_frame()) {
        const std::string label_copy{in.reader->label()};
        // See `on_session_datagram`: the frame is copied out and
        // consumed before it is handled, so that a re-entrant delivery
        // (the synchronous loopback) cannot deliver it twice.
        std::vector<std::uint8_t> payload_copy(in.reader->payload().begin(),
                                               in.reader->payload().end());
        const detail::FrameKind kind = in.reader->kind();
        const std::uint32_t seq = in.reader->seq();
        in.reader->consume();
        if (label_copy == cfg_.label) {
            // WP-D4: the frame kind decides what the frame is — a user
            // message, an ARQ message or an acknowledgement.
            if (kind == detail::FrameKind::kAck) {
                handle_arq_ack(seq);
            } else if (seq != 0) {
                handle_arq_data(seq, label_copy, payload_copy);
            } else {
                deliver(label_copy, payload_copy);
            }
        }
    }
}

void WebTransportDataChannel::deliver(std::string_view label,
                                      std::span<const std::uint8_t> payload) noexcept {
    if (on_msg_) {
        try {
            on_msg_(plugins::BufferView{payload.data(), payload.size()});
        } catch (...) {
        }
    }
    if (on_msg_ex_) {
        try {
            plugins::DataChannelMessage msg;
            msg.label = label;
            msg.channel_id = 0;  // the engine assigns the id
            msg.data = plugins::BufferView{payload.data(), payload.size()};
            on_msg_ex_(msg);
        } catch (...) {
        }
    }
}

void WebTransportDataChannel::fail_protocol(const char* reason) noexcept {
    core::log::Logger::instance().error(
        std::string("web_transport: data channel protocol error: ")
            .append(reason ? reason : "unknown"));
    if (on_state_) {
        on_state_("error");
    }
}

// ---------------------------------------------------------------------------
// Test-only surface
// ---------------------------------------------------------------------------

std::uint64_t WebTransportDataChannel::sent_messages() const noexcept {
    if (!state_) return 0;
    std::lock_guard<std::mutex> l{tx_mutex_};
    return state_->sent;
}

std::uint64_t WebTransportDataChannel::dropped_messages() const noexcept {
    if (!state_) return 0;
    std::lock_guard<std::mutex> l{tx_mutex_};
    return state_->dropped;
}

std::size_t WebTransportDataChannel::pending_sends() const noexcept {
    if (!state_) return 0;
    std::lock_guard<std::mutex> l{tx_mutex_};
    return state_->tx.size();
}

std::size_t WebTransportDataChannel::in_flight_messages() const noexcept {
    if (!state_) return 0;
    std::lock_guard<std::mutex> l{tx_mutex_};
    return state_->inflight.size();
}

std::uint64_t WebTransportDataChannel::retransmitted_messages() const noexcept {
    if (!state_) return 0;
    std::lock_guard<std::mutex> l{tx_mutex_};
    return state_->retransmitted;
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
