/**
 * @file nimrtc/raw_udp/arq_raw_udp.hpp
 * @brief ArqRawUdp concrete implementation.
 *
 * Slice 6. Exposes ArqRawUdp to arq_raw_udp_factory.cpp and
 * test_raw_udp_arq.cpp without making it part of the public SDK.
 *
 * Design: this header defines the full class (all member functions inline)
 * so that factory.cpp can `std::make_unique<ArqRawUdp>()` and test code
 * can `dynamic_cast<ArqRawUdp*>()`. The header is internal to the
 * nimrtc_raw_udp static library and is NOT installed as part of the SDK.
 *
 * Private helpers (dtls_psk_stub, PendingPacket, retransmit thread) stay
 * in arq_raw_udp.cpp where they have internal linkage.
 */
#pragma once

#include <nimrtc/core/log.hpp>
#include <nimrtc/raw_udp/raw_udp_datagram.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace nimrtc::raw_udp {

// ---- PendingPacket (must precede ArqRawUdp which uses it in send()) -----
struct PendingPacket {
    std::uint16_t seq = 0;
    std::vector<std::uint8_t> data;
    std::chrono::steady_clock::time_point sent_at{};
    std::uint16_t retransmit_count = 0;
    bool in_flight = false;
};

// DTLS-PSK stub — namespace-level free functions (defined in arq_raw_udp.cpp).
// Forward-declared BEFORE ArqRawUdp so the inline send() can call them.
// TODO (Slice 4 + Slice 7.5): replace with real DTLS 1.3 PSK record wrapping.
std::vector<std::uint8_t> dtls_psk_encrypt(
    const std::vector<std::uint8_t>& pt, std::string_view psk_hint);
std::vector<std::uint8_t> dtls_psk_decrypt(
    const std::vector<std::uint8_t>& ct, std::string_view psk_hint);

// =============================================================================
// ArqRawUdp — skeleton selective-repeat ARQ
// =============================================================================
class ArqRawUdp final : public IRawUdpDatagram {
public:
    explicit ArqRawUdp(RawUdpConfig cfg) : cfg_(std::move(cfg)) {}

    ~ArqRawUdp() override { close(); }

    ArqRawUdp(const ArqRawUdp&)            = delete;
    ArqRawUdp& operator=(const ArqRawUdp&) = delete;

    // ---- Test hooks (friends) — NOT part of IRawUdpDatagram. Tests use
    //     these to drive the recv path without a real UDP socket.
    friend void arq_test_inject_inbound(ArqRawUdp*, std::vector<std::uint8_t>);
    friend void arq_test_tick_retransmit(ArqRawUdp*);

    // ---- IRawUdpDatagram --------------------------------------------------
    const char* name() const noexcept override { return "ArqRawUdp"; }

    plugins::Status open() noexcept override {
        bool expected = false;
        if (!opened_.compare_exchange_strong(expected, true)) {
            return plugins::kOk;
        }
        start_retransmit_thread_if_needed();
        NIMRTC_LOG_INFO("raw_udp: ArqRawUdp opened (local="
                        << cfg_.local_host << ":" << cfg_.local_port
                        << " psk_hint=" << cfg_.psk_identity_hint << ")");
        return plugins::kOk;
    }

    void close() noexcept override {
        bool expected = true;
        if (!opened_.compare_exchange_strong(expected, false)) {
            return;
        }
        stop_retransmit_thread();
        std::lock_guard<std::mutex> lk(rx_mu_);
        rx_queue_.clear();
        std::lock_guard<std::mutex> lk2(tx_mu_);
        tx_pending_.clear();
        NIMRTC_LOG_INFO("raw_udp: ArqRawUdp closed");
    }

    plugins::Status send(plugins::Endpoint dst,
                         std::span<const std::uint8_t> data) noexcept override {
        if (!opened_.load(std::memory_order_acquire)) {
            return plugins::kErrNotReady;
        }
        if (data.size() > kMaxDatagramBytes) {
            return plugins::kErrBufferTooSmall;
        }
        if (dst.len != 0) {
            peer_endpoint_ = dst;
        }
        cfg_.peer_endpoint = peer_endpoint_;

        PendingPacket p;
        p.seq = next_tx_seq();
        p.data.assign(data.begin(), data.end());
        p.data = dtls_psk_encrypt(p.data, cfg_.psk_identity_hint);
        p.sent_at = std::chrono::steady_clock::now();
        p.in_flight = true;
        {
            std::lock_guard<std::mutex> lk(tx_mu_);
            if (tx_pending_.size() >= cfg_.rx_window_packets) {
                ++stats_.packets_dropped;
                return plugins::kErrBufferTooSmall;
            }
            tx_pending_.push_back(std::move(p));
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
            drained.swap(rx_queue_);
        }
        if (!cb) return static_cast<int>(drained.size());
        for (auto& pkt : drained) {
            plugins::BufferView bv{pkt.data(), pkt.size()};
            try {
                cb(bv);
            } catch (...) {
                // User callback must not kill the recv loop.
            }
        }
        return static_cast<int>(drained.size());
    }

    plugins::Endpoint local_endpoint() const noexcept override {
        return encode_endpoint(cfg_.local_host, cfg_.local_port);
    }

    plugins::Endpoint remote_endpoint() const noexcept override {
        return peer_endpoint_;
    }

    RawUdpStats stats() const noexcept override { return stats_; }

private:
    // ---- Forward to helpers defined in arq_raw_udp.cpp (internal linkage) --
    static constexpr std::size_t kMaxDatagramBytes = 1500;

    std::uint16_t next_tx_seq() noexcept;
    plugins::Endpoint encode_endpoint(const std::string& host,
                                      std::uint16_t port) const noexcept;
    void start_retransmit_thread_if_needed();
    void stop_retransmit_thread();
    void retransmit_main();

    // ---- Private state ----------------------------------------------------
    RawUdpConfig cfg_;
    std::atomic<bool> opened_{false};

    plugins::Endpoint peer_endpoint_;
    plugins::OnDatagramCb on_recv_;

    mutable std::mutex rx_mu_;
    std::deque<std::vector<std::uint8_t>> rx_queue_;

    std::mutex tx_mu_;
    std::deque<PendingPacket> tx_pending_;
    std::atomic<std::uint16_t> tx_seq_{0};

    std::atomic<bool> retransmit_thread_running_{false};
    std::atomic<bool> retransmit_thread_should_stop_{false};
    std::thread retransmit_thread_;

    RawUdpStats stats_;
};

// Test hooks — free functions in nimrtc::raw_udp namespace.
// Defined in arq_raw_udp.cpp. Declared here so other TUs can call them
// without ADL gymnastics.
void arq_test_inject_inbound(ArqRawUdp*, std::vector<std::uint8_t>);
void arq_test_tick_retransmit(ArqRawUdp*);

}  // namespace nimrtc::raw_udp
