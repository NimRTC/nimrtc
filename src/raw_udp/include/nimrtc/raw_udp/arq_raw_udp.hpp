/**
 * @file nimrtc/raw_udp/arq_raw_udp.hpp
 * @brief ArqRawUdp concrete implementation.
 *
 * Slice 6 (transport-selection §6.3) with the real-socket follow-up
 * landed in 2026-09 (Slice 6.5):
 *   - the previous "skeleton" only had an in-process mutex/queue path,
 *     driven by `arq_test_inject_inbound`.  This version actually:
 *       1. opens a UDP socket on `open()` (POSIX / Winsock via the
 *          UdpSocket wrapper) when `enable_real_socket` is true;
 *       2. frames each send() as a [type|seq|payload] packet and
 *          `sendto()`s it to the configured peer;
 *       3. runs a recv-thread that `recvfrom()`s the socket, demuxes
 *          DATA vs ACK frames, applies the selective-repeat reception
 *          state machine, and `sendto()`s ACK frames back;
 *       4. drives a retransmit thread that walks `tx_pending_` and
 *          retransmits anything whose RTO has elapsed, with exponential
 *          backoff capped at `rto_max_ms`, up to `max_retransmits`.
 *
 * Backward compatibility:
 *   - The original test hooks (`arq_test_inject_inbound` /
 *     `arq_test_tick_retransmit`) still work when
 *     `enable_real_socket` is false (default), so the existing
 *     `tests/test_raw_udp_arq.cpp` (5 subtests) is unaffected.
 *   - A new `test_raw_udp_real_loopback` exercises the
 *     `enable_real_socket=true` path end-to-end on 127.0.0.1.
 *
 * Threading model:
 *   - send()  — caller-thread safe (mutex on tx_pending_ only).
 *   - recv()  — caller-thread; drains `rx_delivered_` and fires the
 *               registered `on_recv_` callback synchronously.
 *   - recv thread — owned by ArqRawUdp; blocks on recvfrom().
 *   - retransmit thread — owned by ArqRawUdp; sleeps in rto ticks.
 *
 * @note Frame format (slice 6.5):
 *
 *     0                   1                   2                   3
 *     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |     type      |            sequence number                    |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |                                                               |
 *    +                          payload ...                          +
 *    |                                                               |
 *
 *   type:
 *     0x01 = DATA — 4-byte header (above) + payload.
 *     0x02 = ACK  — 4-byte header + 4-byte cumulative_ack +
 *                    4-byte bitmap (32 selective bits = up to 32 seq
 *                    ahead of cumulative_ack).  Total 12 bytes.
 */
#pragma once

#include <nimrtc/core/log.hpp>
#include <nimrtc/raw_udp/raw_udp_datagram.hpp>
#include <nimrtc/raw_udp/udp_socket.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace nimrtc::raw_udp {

// ===========================================================================
// Frame types (must match the on-wire byte in arq_raw_udp.cpp).
// ===========================================================================
enum class FrameType : std::uint8_t {
    Data = 0x01,
    Ack  = 0x02,
};

// ===========================================================================
// PendingPacket — one in-flight DATA packet on the tx side.
// ===========================================================================
struct PendingPacket {
    std::uint16_t seq = 0;
    std::vector<std::uint8_t> framed;        // full on-wire bytes
    std::chrono::steady_clock::time_point sent_at{};
    std::chrono::milliseconds rto{50};       // current RTO (doubles on loss)
    std::uint16_t retransmit_count = 0;
    bool in_flight = false;
};

// ===========================================================================
// DTLS-PSK stub — kept as namespace-level free functions for compatibility
// with the slice-6 factory / test contract.  Replace with real DTLS 1.3 PSK
// record wrapping once Slice 4 lands the IDtlsSession PSK extension.
// ===========================================================================
std::vector<std::uint8_t> dtls_psk_encrypt(
    const std::vector<std::uint8_t>& pt, std::string_view psk_hint);
std::vector<std::uint8_t> dtls_psk_decrypt(
    const std::vector<std::uint8_t>& ct, std::string_view psk_hint);

// ===========================================================================
// ArqRawUdp — selective-repeat ARQ on top of a real UDP socket.
// ===========================================================================
class ArqRawUdp final : public IRawUdpDatagram {
public:
    explicit ArqRawUdp(RawUdpConfig cfg) : cfg_(std::move(cfg)) {}
    ~ArqRawUdp() override { close(); }

    ArqRawUdp(const ArqRawUdp&)            = delete;
    ArqRawUdp& operator=(const ArqRawUdp&) = delete;

    // ---- Test hooks (friends) — keep the old slice-6 contract alive. -----
    friend void arq_test_inject_inbound(ArqRawUdp*, std::vector<std::uint8_t>);
    friend void arq_test_tick_retransmit(ArqRawUdp*);

    // ---- IRawUdpDatagram --------------------------------------------------
    const char* name() const noexcept override { return "ArqRawUdp"; }

    plugins::Status open() noexcept override {
        bool expected = false;
        if (!opened_.compare_exchange_strong(expected, true)) {
            return plugins::kOk;  // idempotent
        }

        if (cfg_.enable_real_socket) {
            // Open the UDP socket.  If this fails, flip `opened_` back so
            // a subsequent close() is a no-op.
            socket_ = std::make_unique<UdpSocket>();
            if (!socket_->open(cfg_.local_host, cfg_.local_port)) {
                socket_.reset();
                opened_.store(false, std::memory_order_release);
                return plugins::kErrInternal;
            }
            recv_thread_should_stop_.store(false, std::memory_order_release);
            recv_thread_ = std::thread([this] { recv_main(); });
        }

        retransmit_thread_should_stop_.store(false, std::memory_order_release);
        retransmit_thread_ = std::thread([this] { retransmit_main(); });

        NIMRTC_LOG_INFO("raw_udp: ArqRawUdp opened "
                        << "(local=" << cfg_.local_host
                        << ":" << (socket_ ? socket_->local_port()
                                            : cfg_.local_port)
                        << " peer_set=" << (cfg_.peer_endpoint.len != 0)
                        << " real_socket=" << cfg_.enable_real_socket
                        << " rto=" << cfg_.rto_base_ms << "ms"
                        << " window=" << cfg_.rx_window_packets
                        << " psk_hint=" << cfg_.psk_identity_hint << ")");
        return plugins::kOk;
    }

    void close() noexcept override {
        bool expected = true;
        if (!opened_.compare_exchange_strong(expected, false)) {
            return;  // idempotent
        }

        // Order matters here:
        //   1. Close the socket FIRST so the recv thread's blocking
        //      recv_from() returns (with -1) and it can notice
        //      should_stop_ and exit.
        //   2. THEN signal stop + join both threads.
        // The previous order (signal-then-join-then-close) deadlocked
        // because the recv thread was stuck in recv_from() with no
        // way to observe should_stop_ until the socket was closed.
        if (socket_) {
            socket_->close();
            socket_.reset();
        }

        recv_thread_should_stop_.store(true, std::memory_order_release);
        if (recv_thread_.joinable()) recv_thread_.join();
        retransmit_thread_should_stop_.store(true, std::memory_order_release);
        if (retransmit_thread_.joinable()) retransmit_thread_.join();

        {
            std::lock_guard<std::mutex> lk(rx_mu_);
            rx_delivered_.clear();
            rx_buffer_.clear();
        }
        {
            std::lock_guard<std::mutex> lk(tx_mu_);
            tx_pending_.clear();
        }
        NIMRTC_LOG_INFO("raw_udp: ArqRawUdp closed");
    }

    plugins::Status send(plugins::Endpoint dst,
                         std::span<const std::uint8_t> data) noexcept override {
        if (!opened_.load(std::memory_order_acquire)) {
            return plugins::kErrNotReady;
        }
        // Header is 4 bytes (type + seq); cap payload so the total frame
        // fits in a typical Ethernet MTU minus IP/UDP overhead.
        constexpr std::size_t kHeaderBytes = 4;
        if (data.size() + kHeaderBytes > kMaxDatagramBytes) {
            return plugins::kErrBufferTooSmall;
        }

        if (dst.len != 0) {
            std::lock_guard<std::mutex> lk(peer_mu_);
            peer_endpoint_ = dst;
            cfg_.peer_endpoint = dst;
        } else {
            std::lock_guard<std::mutex> lk(peer_mu_);
            if (peer_endpoint_.len == 0) {
                // No peer configured and no peer in this call.
                if (!cfg_.enable_real_socket) {
                    // Test path — allow no-peer so the slice-6 in-process
                    // tests keep working unchanged.
                } else {
                    return plugins::kErrNotReady;
                }
            }
            cfg_.peer_endpoint = peer_endpoint_;
        }

        std::uint16_t seq = next_tx_seq();
        std::vector<std::uint8_t> framed;
        framed.reserve(kHeaderBytes + data.size());
        framed.push_back(static_cast<std::uint8_t>(FrameType::Data));
        framed.push_back(static_cast<std::uint8_t>(seq & 0xFF));
        framed.push_back(static_cast<std::uint8_t>((seq >> 8) & 0xFF));
        framed.push_back(0);  // reserved
        framed.insert(framed.end(), data.begin(), data.end());
        // (DTLS-PSK wrapping will go here once Slice 4 lands.)

        // framed_for_send is the on-the-wire byte sequence we hand to
        // the socket.  The retransmit thread mutates p (rto, sent_at,
        // retransmit_count) but never touches the framed bytes, so a
        // single copy is sufficient.
        std::vector<std::uint8_t> framed_for_send = framed;

        PendingPacket p;
        p.seq = seq;
        p.framed = std::move(framed);
        p.sent_at = std::chrono::steady_clock::now();
        p.rto = std::chrono::milliseconds(cfg_.rto_base_ms);
        p.in_flight = true;

        {
            std::lock_guard<std::mutex> lk(tx_mu_);
            if (tx_pending_.size() >= cfg_.rx_window_packets) {
                ++stats_.packets_dropped;
                return plugins::kErrBufferTooSmall;
            }
            tx_pending_.push_back(std::move(p));
        }

        // Send on the wire (no-op when socket_ is null = test path).
        // We send AFTER releasing the tx lock; framed_for_send is a
        // stable copy that is unaffected by the retransmit thread.
        if (socket_) {
            std::lock_guard<std::mutex> lk(peer_mu_);
            if (peer_endpoint_.len != 0) {
                socket_->send_to(peer_endpoint_, framed_for_send);
            }
        }
        stats_.packets_sent++;
        return plugins::kOk;
    }

    void on_recv(plugins::OnDatagramCb cb) noexcept override {
        std::lock_guard<std::mutex> lk(rx_mu_);
        on_recv_ = std::move(cb);
    }

    int recv() noexcept override {
        plugins::OnDatagramCb cb;
        std::deque<std::vector<std::uint8_t>> drained;
        {
            std::lock_guard<std::mutex> lk(rx_mu_);
            cb = on_recv_;
            drained.swap(rx_delivered_);
        }
        if (!cb) return static_cast<int>(drained.size());
        for (auto& pkt : drained) {
            plugins::BufferView bv{pkt.data(), pkt.size()};
            try { cb(bv); } catch (...) {}
        }
        return static_cast<int>(drained.size());
    }

    plugins::Endpoint local_endpoint() const noexcept override {
        if (socket_ && socket_->is_open()) {
            return UdpSocket::make_endpoint_v4(cfg_.local_host,
                                               socket_->local_port());
        }
        return encode_endpoint(cfg_.local_host, cfg_.local_port);
    }

    plugins::Endpoint remote_endpoint() const noexcept override {
        std::lock_guard<std::mutex> lk(peer_mu_);
        return peer_endpoint_;
    }

    RawUdpStats stats() const noexcept override { return stats_; }

    // ---- Accessors used by the recv thread ---------------------------------
    UdpSocket* socket_for_recv() noexcept { return socket_.get(); }
    std::uint16_t rx_base_seq() const noexcept { return rx_base_seq_.load(); }

    // Advance rx_base_seq_ to deliver contiguous received seqs to
    // rx_delivered_ and fire on_recv_cb_ if set.  Called by the recv
    // thread after queueing a new out-of-order DATA.
    void advance_rx_window_after_insert(std::uint16_t inserted_seq) noexcept;

    // Build + send an ACK frame for the current rx state.  Called by
    // the recv thread after each accepted DATA.
    void send_ack_to(plugins::Endpoint peer) noexcept;

    // Called by the recv thread when an ACK is parsed.
    void apply_ack_frame(std::uint32_t cumulative_ack,
                         std::uint32_t bitmap) noexcept;

private:
    static constexpr std::size_t kMaxDatagramBytes = 1500;

    // ---- helpers ----
    std::uint16_t next_tx_seq() noexcept;
    static plugins::Endpoint encode_endpoint(const std::string& host,
                                              std::uint16_t port) noexcept;

    void recv_main();
    void retransmit_main();

    // ---- state ----
    RawUdpConfig cfg_;
    std::atomic<bool> opened_{false};
    std::unique_ptr<UdpSocket> socket_;

    plugins::Endpoint peer_endpoint_;
    mutable std::mutex peer_mu_;       // guards peer_endpoint_ reads/writes
    plugins::OnDatagramCb on_recv_;

    mutable std::mutex rx_mu_;
    std::deque<std::vector<std::uint8_t>> rx_delivered_;
    // Out-of-order buffer, keyed by seq; only ever grows up to
    // cfg_.rx_window_packets entries before we drop on the floor.
    std::map<std::uint16_t, std::vector<std::uint8_t>> rx_buffer_;
    std::atomic<std::uint16_t> rx_base_seq_{0};
    bool rx_initialized_ = false;            // guarded by rx_mu_

    std::mutex tx_mu_;
    std::deque<PendingPacket> tx_pending_;
    std::atomic<std::uint16_t> tx_seq_{0};

    std::thread recv_thread_;
    std::atomic<bool> recv_thread_should_stop_{false};

    std::thread retransmit_thread_;
    std::atomic<bool> retransmit_thread_should_stop_{false};

    RawUdpStats stats_;
};

// ---- Test hooks (free functions in nimrtc::raw_udp namespace). -------------
void arq_test_inject_inbound(ArqRawUdp*, std::vector<std::uint8_t>);
void arq_test_tick_retransmit(ArqRawUdp*);

} // namespace nimrtc::raw_udp
