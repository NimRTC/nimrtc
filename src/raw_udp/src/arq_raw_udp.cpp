// SPDX-License-Identifier: MIT
//
// @file src/raw_udp/src/arq_raw_udp.cpp
// @brief ArqRawUdp — selective-repeat ARQ over a real UDP socket.
//
// Slice 6.5 (transport-selection §6.3 follow-up). The full class is
// declared in arq_raw_udp.hpp; this file provides the out-of-class
// definitions and the recv-thread / retransmit-thread bodies.
//
// Frame format is documented in arq_raw_udp.hpp (4-byte header
// [type|seq|hi|reserved] + payload for DATA; 12 bytes for ACK).
//
#include <nimrtc/raw_udp/arq_raw_udp.hpp>

#include <nimrtc/core/log.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace nimrtc::raw_udp {

// ===========================================================================
// Frame constants — keep in sync with arq_raw_udp.hpp documentation.
// ===========================================================================
namespace {

constexpr std::size_t kDataHeaderBytes  = 4;   // type + seq(2) + reserved
constexpr std::size_t kAckHeaderBytes   = 4;   // type + seq(2) + reserved
constexpr std::size_t kAckExtraBytes    = 8;   // cumulative_ack(4) + bitmap(4)
constexpr std::size_t kAckTotalBytes    = kAckHeaderBytes + kAckExtraBytes;
constexpr std::uint32_t kSelectiveAckBits = 32u;

// Encode the DATA frame.  Returns false if `data.size()` won't fit.
bool build_data_frame(std::vector<std::uint8_t>& out,
                      std::uint16_t seq,
                      std::span<const std::uint8_t> data) {
    out.clear();
    out.reserve(kDataHeaderBytes + data.size());
    out.push_back(static_cast<std::uint8_t>(FrameType::Data));
    out.push_back(static_cast<std::uint8_t>(seq & 0xFF));
    out.push_back(static_cast<std::uint8_t>((seq >> 8) & 0xFF));
    out.push_back(0);
    out.insert(out.end(), data.begin(), data.end());
    return true;
}

// Encode the ACK frame: 4-byte header + 4-byte cumulative_ack +
// 4-byte selective-ack bitmap.
//
// Wire layout (12 bytes total):
//   [0]      type = FrameType::Ack (0x02)
//   [1..4]   cumulative_ack, little-endian (low byte first)
//   [5..8]   selective-ack bitmap, little-endian
//   [9..11]  reserved (must be zero on transmit; ignored on receive)
//
// `cumulative_ack` is typed as std::uint32_t even though our current
// seq space is 16 bits — this leaves room for future seq-space
// expansion (RFC 1982 "Serial Number Arithmetic" can be promoted to a
// 32-bit "epoch" counter without breaking the wire format).
void build_ack_frame(std::vector<std::uint8_t>& out,
                     std::uint32_t cumulative_ack,
                     std::uint32_t bitmap) {
    out.assign(kAckTotalBytes, 0);
    out[0] = static_cast<std::uint8_t>(FrameType::Ack);
    out[1] = static_cast<std::uint8_t>(cumulative_ack & 0xFF);
    out[2] = static_cast<std::uint8_t>((cumulative_ack >> 8) & 0xFF);
    out[3] = static_cast<std::uint8_t>((cumulative_ack >> 16) & 0xFF);
    out[4] = static_cast<std::uint8_t>((cumulative_ack >> 24) & 0xFF);
    out[5] = static_cast<std::uint8_t>(bitmap & 0xFF);
    out[6] = static_cast<std::uint8_t>((bitmap >> 8) & 0xFF);
    out[7] = static_cast<std::uint8_t>((bitmap >> 16) & 0xFF);
    out[8] = static_cast<std::uint8_t>((bitmap >> 24) & 0xFF);
    // bytes 9..11 left as zero (reserved).
}

// 16-bit modular difference `a - b` modulo 65536.  Returns true if
// `a` is "after" `b` in the same 32k half — true positive direction.
inline bool seq_after(std::uint16_t a, std::uint16_t b) noexcept {
    return static_cast<std::int32_t>(
        (static_cast<std::int32_t>(a) - static_cast<std::int32_t>(b)) & 0xFFFF
    ) < 0x8000;
}

inline bool seq_in_window(std::uint16_t seq, std::uint16_t base,
                          std::uint16_t window) noexcept {
    // seq is in [base, base + window)
    auto diff = static_cast<std::int32_t>(
        (static_cast<std::int32_t>(seq) - static_cast<std::int32_t>(base)) & 0xFFFF);
    return diff >= 0 && diff < window;
}

} // namespace

// ===========================================================================
// DTLS-PSK stub (Slice 6) — passthrough encrypt/decrypt.
// ===========================================================================
std::vector<std::uint8_t> dtls_psk_encrypt(const std::vector<std::uint8_t>& pt,
                                           std::string_view /*psk_hint*/) {
    return pt;
}
std::vector<std::uint8_t> dtls_psk_decrypt(const std::vector<std::uint8_t>& ct,
                                           std::string_view /*psk_hint*/) {
    return ct;
}

// ===========================================================================
// ArqRawUdp private helpers
// ===========================================================================
std::uint16_t ArqRawUdp::next_tx_seq() noexcept {
    return tx_seq_.fetch_add(1, std::memory_order_acq_rel);
}

plugins::Endpoint ArqRawUdp::encode_endpoint(const std::string& host,
                                              std::uint16_t port) noexcept {
    plugins::Endpoint ep{};
    if (host.empty() && port == 0) return ep;
    std::string s = host + ":" + std::to_string(port);
    const std::size_t n = std::min(s.size(), sizeof(ep.data));
    std::memcpy(ep.data, s.data(), n);
    ep.len = static_cast<std::uint32_t>(n);
    return ep;
}

// ===========================================================================
// Recv path — selective-repeat reception state machine.
// ===========================================================================
void ArqRawUdp::advance_rx_window_after_insert(std::uint16_t inserted_seq) noexcept {
    std::lock_guard<std::mutex> lk(rx_mu_);
    if (!rx_initialized_) {
        // First DATA we ever see — this becomes the base.
        rx_initialized_ = true;
        rx_base_seq_.store(inserted_seq, std::memory_order_release);
        rx_buffer_.clear();
    }
    // Insert into rx_buffer_ if it's not already there.  We deliberately
    // ignore duplicates (the recv thread already de-dups on the wire
    // path, but defending here makes the slice-6 test hook path safe).
    if (inserted_seq == rx_base_seq_.load(std::memory_order_acquire)) {
        // In-order: just deliver this one and walk the buffer.
    } else if (!seq_in_window(inserted_seq, rx_base_seq_.load(),
                               cfg_.rx_window_packets)) {
        // Out of window — drop on the floor (counter incremented in
        // recv path before calling here).
        return;
    } else {
        // Out-of-order: buffer for later delivery.
        rx_buffer_[inserted_seq] = std::vector<std::uint8_t>{};
    }

    // Drain contiguous received seqs starting at base.
    std::uint16_t base = rx_base_seq_.load(std::memory_order_acquire);
    while (true) {
        std::vector<std::uint8_t> payload;
        if (base == inserted_seq || rx_buffer_.count(base)) {
            if (base == inserted_seq) {
                // The caller already moved the payload into the queue
                // (see recv_main), but we don't have it here — the
                // caller is responsible.  We just advance base.
                payload.clear();
            } else {
                payload = std::move(rx_buffer_[base]);
                rx_buffer_.erase(base);
            }
            if (!payload.empty() || base == inserted_seq) {
                rx_delivered_.push_back(std::move(payload));
                ++stats_.packets_recv;
            }
            base = static_cast<std::uint16_t>(base + 1);
            rx_base_seq_.store(base, std::memory_order_release);
        } else {
            break;
        }
    }
}

void ArqRawUdp::send_ack_to(plugins::Endpoint peer) noexcept {
    if (!socket_ || peer.len == 0) return;
    std::uint32_t bitmap = 0;
    std::uint16_t base = rx_base_seq_.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lk(rx_mu_);
        // Walk the buffer for seqs within [base+1, base+32] and set bits.
        for (auto& kv : rx_buffer_) {
            std::uint16_t seq = kv.first;
            auto diff = static_cast<std::int32_t>(
                (static_cast<std::int32_t>(seq) - static_cast<std::int32_t>(base))
                & 0xFFFF);
            if (diff > 0 && diff <= static_cast<std::int32_t>(kSelectiveAckBits)) {
                bitmap |= (1u << (diff - 1));
            }
        }
    }
    std::vector<std::uint8_t> frame;
    build_ack_frame(frame, base, bitmap);
    socket_->send_to(peer, frame);
    ++stats_.acks_sent;
}

void ArqRawUdp::apply_ack_frame(std::uint32_t cumulative_ack,
                                  std::uint32_t bitmap) noexcept {
    std::lock_guard<std::mutex> lk(tx_mu_);
    // Walk tx_pending_; ack anything <= cumulative_ack (cumulative) or
    // matching a bit in the selective bitmap (seq = cumulative_ack+1+N).
    auto it = tx_pending_.begin();
    while (it != tx_pending_.end()) {
        std::uint16_t seq = it->seq;
        bool acked = false;
        if (seq == cumulative_ack) {
            acked = true;
        } else if (seq_after(seq, static_cast<std::uint16_t>(cumulative_ack))) {
            auto diff = static_cast<std::int32_t>(
                (static_cast<std::int32_t>(seq) - static_cast<std::int32_t>(cumulative_ack))
                & 0xFFFF);
            if (diff > 0 && diff <= static_cast<std::int32_t>(kSelectiveAckBits)) {
                if (bitmap & (1u << (diff - 1))) acked = true;
            }
        } else if (static_cast<std::uint16_t>(cumulative_ack - seq) != 0) {
            // seq "before" cumulative_ack in the same half — already
            // counted in the cumulative ack.
            acked = true;
        }
        if (acked) {
            it->in_flight = false;
            ++stats_.acks_recv;
            it = tx_pending_.erase(it);
        } else {
            ++it;
        }
    }
}

// ===========================================================================
// Recv thread — recvfrom loop + frame demux.
// ===========================================================================
void ArqRawUdp::recv_main() {
    std::vector<std::uint8_t> buf(kMaxDatagramBytes);
    while (!recv_thread_should_stop_.load(std::memory_order_acquire)) {
        if (!socket_) break;
        plugins::Endpoint from{};
        auto n = socket_->recv_from(buf.data(), buf.size(), &from);
        if (n < 0) break;                          // fatal socket error
        if (n == 0) continue;                       // recoverable
        if (static_cast<std::size_t>(n) < kDataHeaderBytes) {
            ++stats_.packets_dropped;
            continue;
        }
        FrameType t = static_cast<FrameType>(buf[0]);
        std::uint16_t seq = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(buf[1])
            | (static_cast<std::uint16_t>(buf[2]) << 8));

        if (t == FrameType::Ack) {
            if (static_cast<std::size_t>(n) < kAckTotalBytes) {
                ++stats_.packets_dropped;
                continue;
            }
            // Wire layout (see build_ack_frame):
            //   [0]      type = FrameType::Ack (0x02)
            //   [1..4]   cumulative_ack, little-endian (4 bytes)
            //   [5..8]   selective-ack bitmap, little-endian (4 bytes)
            //   [9..11]  reserved (zero on transmit; ignored on receive)
            // Note: for ACK frames the [1..2] "seq" bytes read above are
            // actually the low 2 bytes of cumulative_ack — we don't use
            // `seq` in the ACK branch so this is harmless.
            std::uint32_t cum = static_cast<std::uint32_t>(buf[1])
                | (static_cast<std::uint32_t>(buf[2]) << 8)
                | (static_cast<std::uint32_t>(buf[3]) << 16)
                | (static_cast<std::uint32_t>(buf[4]) << 24);
            std::uint32_t bitmap = static_cast<std::uint32_t>(buf[5])
                | (static_cast<std::uint32_t>(buf[6]) << 8)
                | (static_cast<std::uint32_t>(buf[7]) << 16)
                | (static_cast<std::uint32_t>(buf[8]) << 24);
            apply_ack_frame(cum, bitmap);
            continue;
        }

        if (t != FrameType::Data) {
            ++stats_.packets_dropped;
            continue;
        }

        // DATA: extract payload, hand off to selective-repeat logic.
        std::vector<std::uint8_t> payload(
            buf.begin() + kDataHeaderBytes,
            buf.begin() + static_cast<std::ptrdiff_t>(n));
        // Decrypt (DTLS-PSK stub: passthrough).
        payload = dtls_psk_decrypt(payload, cfg_.psk_identity_hint);

        std::uint16_t base = rx_base_seq_.load(std::memory_order_acquire);
        if (!rx_initialized_) {
            // First DATA: install as base and deliver.
            {
                std::lock_guard<std::mutex> lk(rx_mu_);
                rx_initialized_ = true;
                rx_base_seq_.store(seq, std::memory_order_release);
                rx_delivered_.push_back(std::move(payload));
                ++stats_.packets_recv;
                // Wake any waiters: callers can poll recv() at leisure.
            }
            send_ack_to(from);
            continue;
        }

        if (seq == base) {
            // Already-delivered duplicate — re-ACK and move on.
            send_ack_to(from);
            ++stats_.packets_dropped;
            continue;
        }
        if (!seq_in_window(seq, base, cfg_.rx_window_packets)) {
            // Out of window — drop.
            ++stats_.packets_dropped;
            send_ack_to(from);  // best-effort: peer may catch up via cumulative
            continue;
        }

        // In-window DATA: deliver in-order or buffer for later.
        if (seq_after(seq, base)) {
            auto diff = static_cast<std::uint16_t>(seq - base);
            if (diff == 1) {
                // Direct in-order delivery.
                std::lock_guard<std::mutex> lk(rx_mu_);
                rx_delivered_.push_back(std::move(payload));
                ++stats_.packets_recv;
                rx_base_seq_.store(static_cast<std::uint16_t>(base + 1),
                                    std::memory_order_release);
                // Walk any buffered contig seqs.
                std::uint16_t new_base = rx_base_seq_.load();
                while (rx_buffer_.count(new_base)) {
                    auto& v = rx_buffer_[new_base];
                    rx_delivered_.push_back(std::move(v));
                    rx_buffer_.erase(new_base);
                    ++stats_.packets_recv;
                    new_base = static_cast<std::uint16_t>(new_base + 1);
                    rx_base_seq_.store(new_base, std::memory_order_release);
                }
            } else {
                // Out-of-order: buffer.
                std::lock_guard<std::mutex> lk(rx_mu_);
                rx_buffer_[seq] = std::move(payload);
            }
        } else {
            // seq "before" base — duplicate, re-ACK and skip.
            ++stats_.packets_dropped;
        }
        send_ack_to(from);
    }
}

// ===========================================================================
// Retransmit thread — walks tx_pending_, retransmits anything whose RTO
// has elapsed.  Doubles the per-packet RTO on each retransmit, capped at
// cfg_.rto_max_ms.  Drops packets after cfg_.max_retransmits.
// ===========================================================================
void ArqRawUdp::retransmit_main() {
    using namespace std::chrono_literals;
    const auto tick = std::max<std::int64_t>(1, cfg_.rto_base_ms);
    const auto cap  = std::chrono::milliseconds(cfg_.rto_max_ms);

    while (!retransmit_thread_should_stop_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(tick));

        auto now = std::chrono::steady_clock::now();
        std::vector<std::pair<std::uint16_t, std::vector<std::uint8_t>>> to_resend;
        std::vector<std::uint16_t> to_drop;
        {
            std::lock_guard<std::mutex> lk(tx_mu_);
            for (auto& p : tx_pending_) {
                if (!p.in_flight) continue;
                if (now - p.sent_at < p.rto) continue;
                if (p.retransmit_count >= cfg_.max_retransmits) {
                    to_drop.push_back(p.seq);
                    continue;
                }
                ++p.retransmit_count;
                ++stats_.packets_retransmit;
                p.sent_at = now;
                p.rto = std::min(std::chrono::milliseconds(p.rto.count() * 2),
                                  cap);
                to_resend.emplace_back(p.seq, p.framed);
            }
            for (auto seq : to_drop) {
                for (auto it = tx_pending_.begin(); it != tx_pending_.end(); ++it) {
                    if (it->seq == seq) {
                        it->in_flight = false;
                        ++stats_.packets_dropped;
                        tx_pending_.erase(it);
                        break;
                    }
                }
            }
        }

        // Send outside the lock so socket sendto latency doesn't stall
        // the timer thread for other packets' retransmits.
        if (socket_) {
            std::lock_guard<std::mutex> plk(peer_mu_);
            if (peer_endpoint_.len != 0) {
                for (auto& [seq, framed] : to_resend) {
                    socket_->send_to(peer_endpoint_, framed);
                    (void)seq;
                }
            }
        }
    }
}

// ===========================================================================
// Slice-6 test hooks (NOT part of IRawUdpDatagram).
//
// These let the original slice-6 in-process tests (which never opened a
// real socket) drive ArqRawUdp without spinning up Winsock / Berkeley
// sockets.  They preserve the old contract:
//   - arq_test_inject_inbound pushes a DATA payload (no header) into the
//     recv queue directly, mirroring a recvfrom() result post-strip.
//   - arq_test_tick_retransmit manually walks tx_pending_ and counts
//     retransmits, mirroring one tick of retransmit_main().
// ===========================================================================
void arq_test_inject_inbound(ArqRawUdp* impl,
                             std::vector<std::uint8_t> bytes) {
    if (!impl) return;
    bytes = dtls_psk_decrypt(bytes, impl->cfg_.psk_identity_hint);
    {
        std::lock_guard<std::mutex> lk(impl->rx_mu_);
        impl->rx_delivered_.push_back(std::move(bytes));
        impl->stats_.packets_recv++;
    }
}

void arq_test_tick_retransmit(ArqRawUdp* impl) {
    if (!impl) return;
    std::lock_guard<std::mutex> lk(impl->tx_mu_);
    for (auto& p : impl->tx_pending_) {
        if (p.in_flight) {
            ++p.retransmit_count;
            ++impl->stats_.packets_retransmit;
            if (p.retransmit_count >= impl->cfg_.max_retransmits) {
                p.in_flight = false;
                ++impl->stats_.packets_dropped;
            }
        }
    }
}

} // namespace nimrtc::raw_udp
