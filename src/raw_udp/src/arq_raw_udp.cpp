/**
 * @file src/raw_udp/src/arq_raw_udp.cpp
 * @brief ArqRawUdp — skeleton selective-repeat ARQ.
 *
 * Slice 6 (transport-selection §6.3). See arq_raw_udp.hpp for the full
 * class definition. This file provides the out-of-class definitions for:
 *   - static dtls_psk_encrypt / dtls_psk_decrypt (internal linkage)
 *   - PendingPacket struct (internal linkage)
 *   - private helpers: next_tx_seq, encode_endpoint, retransmit_main
 *   - test hooks: arq_test_inject_inbound, arq_test_tick_retransmit
 */
#include <nimrtc/raw_udp/arq_raw_udp.hpp>

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
// -----------------------------------------------------------------------------
// DTLS-PSK stub — passthrough encrypt/decrypt (declared in arq_raw_udp.hpp).
// TODO (Slice 4 + Slice 7.5): replace with real DTLS 1.3 PSK record wrapping.
// -----------------------------------------------------------------------------
std::vector<std::uint8_t> dtls_psk_encrypt(const std::vector<std::uint8_t>& pt,
                                           std::string_view /*psk_hint*/) {
    return pt;  // passthrough
}
std::vector<std::uint8_t> dtls_psk_decrypt(const std::vector<std::uint8_t>& ct,
                                           std::string_view /*psk_hint*/) {
    return ct;  // passthrough
}

namespace {

// (anonymous namespace for internal-linkage helpers only — XOR-stub helpers
// can stay here since they're only used inside arq_raw_udp.cpp)

}

// -----------------------------------------------------------------------------
// ArqRawUdp private helpers (out-of-class definitions)
// -----------------------------------------------------------------------------
std::uint16_t ArqRawUdp::next_tx_seq() noexcept {
    return tx_seq_.fetch_add(1, std::memory_order_acq_rel);
}

plugins::Endpoint ArqRawUdp::encode_endpoint(const std::string& host,
                                             std::uint16_t port) const noexcept {
    plugins::Endpoint ep{};
    if (host.empty() && port == 0) return ep;
    std::string s = host + ":" + std::to_string(port);
    const std::size_t n = std::min(s.size(), sizeof(ep.data));
    std::memcpy(ep.data, s.data(), n);
    ep.len = static_cast<std::uint32_t>(n);
    return ep;
}

void ArqRawUdp::start_retransmit_thread_if_needed() {
    bool expected = false;
    if (!retransmit_thread_running_.compare_exchange_strong(expected, true)) {
        return;
    }
    retransmit_thread_should_stop_.store(false);
    retransmit_thread_ = std::thread([this] { retransmit_main(); });
}

void ArqRawUdp::stop_retransmit_thread() {
    if (!retransmit_thread_running_.exchange(false)) return;
    retransmit_thread_should_stop_.store(true);
    if (retransmit_thread_.joinable()) {
        retransmit_thread_.join();
    }
}

void ArqRawUdp::retransmit_main() {
    using namespace std::chrono_literals;
    const auto tick = std::chrono::milliseconds(cfg_.rto_base_ms);
    while (!retransmit_thread_should_stop_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(tick);
        // TODO (production): iterate tx_pending_ and retransmit any
        // packet whose age > rto_for_packet(p).
    }
}

// -----------------------------------------------------------------------------
// Slice-6 test hooks (NOT part of IRawUdpDatagram).
// These free functions let test_raw_udp_arq.cpp drive the recv path without
// a real UDP socket. Declared as friends of ArqRawUdp in the header so they
// can reach private state.
// -----------------------------------------------------------------------------
void arq_test_inject_inbound(ArqRawUdp* impl,
                             std::vector<std::uint8_t> bytes) {
    if (!impl) return;
    bytes = dtls_psk_decrypt(bytes, impl->cfg_.psk_identity_hint);
    {
        std::lock_guard<std::mutex> lk(impl->rx_mu_);
        impl->rx_queue_.push_back(std::move(bytes));
    }
    impl->stats_.packets_recv++;
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

}  // namespace nimrtc::raw_udp
