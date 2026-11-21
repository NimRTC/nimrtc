// tcp_ice.cpp — the ICE-over-TCP connectivity-check driver (RFC 6544).
//
// Two jobs live here:
//
//   1. The *second* STUN implementation in this codebase (RFC 8489 §6):
//      building and verifying authenticated ICE connectivity-check
//      messages.  libjuice has the first one, for UDP; the TCP path needs
//      its own because the two transports share no wire code.
//
//   2. The TcpIceDriver: gather the two host TCP candidates (passive
//      listener + active connector), exchange them over the seam's trickle
//      surface, run the connectivity checks over the framed TCP stream,
//      and nominate exactly one pair.
//
// The driver is driven by one background check thread.  It first tries the
// outbound check (our active socket → the peer's passive candidate), then
// falls back to accepting inbound checks on our passive listener.  Either
// path nominates at most once, so `selected_pair()` never flaps between
// "udp" and "tcp" (slice risk 4).

#include "tcp_ice.hpp"

#ifdef _WIN32
#define _WIN32_WINNT 0x0601
#endif

// wolfSSL headers use anonymous structs (a GNU extension); suppress
// -Wpedantic for these third-party headers, like turn_tls.cpp does.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/hmac.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <utility>

namespace nimrtc::ice {

namespace {

// ---------------------------------------------------------------------------
// STUN wire constants (RFC 8489 §6, RFC 8445 §7.1.2)
// ---------------------------------------------------------------------------

constexpr std::size_t kStunHeaderSize = 20;
constexpr std::uint32_t kStunMagicCookie = 0x2112A442u;
constexpr std::uint16_t kBindingRequest = 0x0001;
constexpr std::uint16_t kBindingSuccess = 0x0101;

constexpr std::uint16_t kAttrUsername = 0x0006;
constexpr std::uint16_t kAttrMessageIntegrity = 0x0008;
constexpr std::uint16_t kAttrXorMappedAddress = 0x0020;
constexpr std::uint16_t kAttrPriority = 0x0024;
constexpr std::uint16_t kAttrUseCandidate = 0x0025;
constexpr std::uint16_t kAttrIceControlling = 0x802A;

// The MESSAGE-INTEGRITY attribute on the wire: 4-byte attribute header +
// 20-byte HMAC-SHA1 digest (RFC 5389 §15.4).  Its length is not padded
// (20 is already a multiple of 4).
constexpr std::size_t kMiAttrWireSize = 24;
constexpr std::size_t kHmacSha1Size = 20;

// ---------------------------------------------------------------------------
// Network-order read/write on the STUN message buffer
// ---------------------------------------------------------------------------

void put_u16(std::uint8_t* v, std::size_t off, std::uint16_t x) {
    v[off]     = static_cast<std::uint8_t>(x >> 8);
    v[off + 1] = static_cast<std::uint8_t>(x & 0xFF);
}

void put_u32(std::uint8_t* v, std::size_t off, std::uint32_t x) {
    v[off]     = static_cast<std::uint8_t>((x >> 24) & 0xFF);
    v[off + 1] = static_cast<std::uint8_t>((x >> 16) & 0xFF);
    v[off + 2] = static_cast<std::uint8_t>((x >> 8) & 0xFF);
    v[off + 3] = static_cast<std::uint8_t>(x & 0xFF);
}

std::uint16_t get_u16(std::span<const std::uint8_t> v, std::size_t off) {
    return static_cast<std::uint16_t>((v[off] << 8) | v[off + 1]);
}

std::uint32_t get_u32(std::span<const std::uint8_t> v, std::size_t off) {
    return (static_cast<std::uint32_t>(v[off]) << 24) |
           (static_cast<std::uint32_t>(v[off + 1]) << 16) |
           (static_cast<std::uint32_t>(v[off + 2]) << 8) |
           static_cast<std::uint32_t>(v[off + 3]);
}

// Append a TLV attribute, padded so the next attribute starts on a 4-byte
// boundary (RFC 8489 §6.2).
void append_attr(std::vector<std::uint8_t>& msg, std::uint16_t type,
                 std::span<const std::uint8_t> value) {
    const std::size_t header_off = msg.size();
    msg.push_back(0);
    msg.push_back(0);
    msg.push_back(0);
    msg.push_back(0);
    put_u16(msg.data(), header_off, type);
    put_u16(msg.data(), header_off + 2, static_cast<std::uint16_t>(value.size()));
    msg.insert(msg.end(), value.begin(), value.end());
    while (msg.size() % 4 != 0) {
        msg.push_back(0);
    }
}

// HMAC-SHA1 (RFC 5389 §15.4) via wolfcrypt.  Returns false only on an
// API-level failure; a mismatch is a boolean comparison by the caller.
bool hmac_sha1(const std::string& key, std::span<const std::uint8_t> data,
               std::array<std::uint8_t, kHmacSha1Size>& out) {
    Hmac hmac;
    if (wc_HmacSetKey(&hmac, WC_SHA, reinterpret_cast<const byte*>(key.data()),
                      static_cast<word32>(key.size())) != 0) {
        return false;
    }
    if (wc_HmacUpdate(&hmac, reinterpret_cast<const byte*>(data.data()),
                      static_cast<word32>(data.size())) != 0) {
        return false;
    }
    return wc_HmacFinal(&hmac, reinterpret_cast<byte*>(out.data())) == 0;
}

std::array<std::uint8_t, 12> random_transaction_id() {
    std::array<std::uint8_t, 12> txn{};
    std::random_device rd;
    for (auto& b : txn) {
        b = static_cast<std::uint8_t>(rd());
    }
    return txn;
}

// Split "ip:port" into its parts (IPv4 only — the last colon is the port
// separator, which is unambiguous for IPv4 literals).
bool split_endpoint(const std::string& endpoint, std::string* host,
                    std::uint16_t* port) {
    const auto colon = endpoint.rfind(':');
    if (colon == std::string::npos) return false;
    *host = endpoint.substr(0, colon);
    const long p = std::strtol(endpoint.c_str() + colon + 1, nullptr, 10);
    if (p <= 0 || p > 65535) return false;
    *port = static_cast<std::uint16_t>(p);
    return true;
}

// The address to advertise for a candidate bound to `bind_host`: a
// wildcard bind resolves to a concrete IP via the socket, anything else is
// used verbatim.
std::string candidate_host(const TcpSocket& sock, const std::string& bind_host) {
    if (bind_host.empty() || bind_host == "0.0.0.0") {
        const std::string endpoint = sock.local_endpoint();
        const auto colon = endpoint.rfind(':');
        return colon == std::string::npos ? endpoint : endpoint.substr(0, colon);
    }
    return bind_host;
}

// ---------------------------------------------------------------------------
// Driver time budgets.  All bounded so the check thread observes the stop
// flag at least every kRecvPollTimeoutMs of I/O (tcp_socket discipline 2).
// ---------------------------------------------------------------------------

constexpr int kConnectTimeoutMs = 1500;
constexpr int kSendTimeoutMs = 1000;
constexpr int kResponseTimeoutMs = 1500;
constexpr int kRequestTimeoutMs = 1500;
constexpr int kAcceptTimeoutMs = 200;

}  // namespace

// ---------------------------------------------------------------------------
// STUN building / verification
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> stun_build_binding_request(
    const std::string& username, const std::string& password,
    std::uint32_t priority, std::uint64_t tiebreaker, bool use_candidate,
    std::span<const std::uint8_t> transaction_id) {
    std::vector<std::uint8_t> msg(kStunHeaderSize, 0);
    put_u16(msg.data(),0, kBindingRequest);
    // Length is set once the attributes are in (it must account for the
    // MESSAGE-INTEGRITY about to be appended).
    put_u32(msg.data(),4, kStunMagicCookie);
    std::copy(transaction_id.begin(), transaction_id.end(), msg.begin() + 8);

    append_attr(msg, kAttrUsername,
                std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(username.data()),
                    username.size()));

    std::array<std::uint8_t, 4> priority_bytes{};
    put_u32(priority_bytes.data(), 0, priority);
    append_attr(msg, kAttrPriority, priority_bytes);

    std::array<std::uint8_t, 8> tiebreaker_bytes{};
    for (int i = 0; i < 8; ++i) {
        tiebreaker_bytes[static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((tiebreaker >> (56 - 8 * i)) & 0xFF);
    }
    append_attr(msg, kAttrIceControlling, tiebreaker_bytes);

    if (use_candidate) {
        append_attr(msg, kAttrUseCandidate, {});
    }

    // RFC 5389 §15.4: the header length must include the MESSAGE-INTEGRITY
    // attribute; the HMAC covers everything up to (not including) it.
    put_u16(msg.data(),2, static_cast<std::uint16_t>(msg.size() - kStunHeaderSize +
                                               kMiAttrWireSize));

    std::array<std::uint8_t, kHmacSha1Size> digest{};
    if (!hmac_sha1(password, msg, digest)) {
        return {};
    }
    append_attr(msg, kAttrMessageIntegrity, digest);
    return msg;
}

StunRequestInfo stun_verify_binding_request(std::span<const std::uint8_t> msg,
                                            const std::string& password) {
    StunRequestInfo info;
    if (msg.size() < kStunHeaderSize) return info;
    if (get_u16(msg, 0) != kBindingRequest) return info;
    if (get_u32(msg, 4) != kStunMagicCookie) return info;

    const std::size_t length = get_u16(msg, 2);
    if (msg.size() < kStunHeaderSize + length) return info;

    std::optional<std::size_t> mi_off;
    std::size_t pos = kStunHeaderSize;
    const std::size_t end = kStunHeaderSize + length;
    while (pos + 4 <= end) {
        const std::uint16_t type = get_u16(msg, pos);
        const std::uint16_t alen = get_u16(msg, pos + 2);
        if (pos + 4 + alen > end) return info;  // attribute past the end
        if (type == kAttrMessageIntegrity) {
            if (alen != kHmacSha1Size) return info;
            mi_off = pos;
            break;
        }
        if (type == kAttrUsername) {
            info.username.assign(
                reinterpret_cast<const char*>(msg.data() + pos + 4), alen);
        }
        pos += 4 + ((alen + 3) & ~std::size_t{3});
    }
    if (!mi_off) return info;

    std::array<std::uint8_t, kHmacSha1Size> digest{};
    if (!hmac_sha1(password, std::span<const std::uint8_t>(msg.data(), *mi_off),
                   digest)) {
        return info;
    }
    if (!std::equal(digest.begin(), digest.end(), msg.begin() + *mi_off + 4)) {
        return info;
    }

    info.valid = true;
    std::copy(msg.begin() + 8, msg.begin() + kStunHeaderSize,
              info.transaction_id.begin());
    return info;
}

std::vector<std::uint8_t> stun_build_binding_response(
    const std::string& username, const std::string& password,
    std::span<const std::uint8_t> transaction_id, const std::string& mapped_host,
    std::uint16_t mapped_port) {
    std::vector<std::uint8_t> msg(kStunHeaderSize, 0);
    put_u16(msg.data(),0, kBindingSuccess);
    put_u32(msg.data(),4, kStunMagicCookie);
    std::copy(transaction_id.begin(), transaction_id.end(), msg.begin() + 8);

    append_attr(msg, kAttrUsername,
                std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(username.data()),
                    username.size()));

    // XOR-MAPPED-ADDRESS (RFC 5389 §15.2): family 0x01 (IPv4), then port
    // and address XORed byte-wise with the magic cookie, all in network
    // byte order.
    std::array<std::uint8_t, 4> ip{};
    if (::inet_pton(AF_INET, mapped_host.c_str(), ip.data()) != 1) {
        return {};
    }
    std::array<std::uint8_t, 8> xma{};
    xma[0] = 0x00;
    xma[1] = 0x01;
    xma[2] = static_cast<std::uint8_t>((mapped_port ^ 0x2112) >> 8);
    xma[3] = static_cast<std::uint8_t>(mapped_port ^ 0x2112);
    xma[4] = ip[0] ^ 0x21;
    xma[5] = ip[1] ^ 0x12;
    xma[6] = ip[2] ^ 0xA4;
    xma[7] = ip[3] ^ 0x42;
    append_attr(msg, kAttrXorMappedAddress, xma);

    put_u16(msg.data(),2, static_cast<std::uint16_t>(msg.size() - kStunHeaderSize +
                                               kMiAttrWireSize));

    std::array<std::uint8_t, kHmacSha1Size> digest{};
    if (!hmac_sha1(password, msg, digest)) {
        return {};
    }
    append_attr(msg, kAttrMessageIntegrity, digest);
    return msg;
}

std::optional<std::string> stun_verify_binding_response(
    std::span<const std::uint8_t> msg, const std::string& password,
    std::span<const std::uint8_t> transaction_id) {
    if (msg.size() < kStunHeaderSize) return std::nullopt;
    if (get_u16(msg, 0) != kBindingSuccess) return std::nullopt;
    if (get_u32(msg, 4) != kStunMagicCookie) return std::nullopt;
    if (!std::equal(transaction_id.begin(), transaction_id.end(),
                    msg.begin() + 8)) {
        return std::nullopt;
    }

    const std::size_t length = get_u16(msg, 2);
    if (msg.size() < kStunHeaderSize + length) return std::nullopt;

    std::optional<std::size_t> mi_off;
    std::optional<std::string> mapped;
    std::size_t pos = kStunHeaderSize;
    const std::size_t end = kStunHeaderSize + length;
    while (pos + 4 <= end) {
        const std::uint16_t type = get_u16(msg, pos);
        const std::uint16_t alen = get_u16(msg, pos + 2);
        if (pos + 4 + alen > end) return std::nullopt;
        if (type == kAttrMessageIntegrity) {
            if (alen != kHmacSha1Size) return std::nullopt;
            mi_off = pos;
            break;
        }
        if (type == kAttrXorMappedAddress) {
            if (alen != 8 || msg[pos + 4 + 1] != 0x01) return std::nullopt;
            const std::uint16_t xport = get_u16(msg, pos + 4 + 2);
            const std::uint16_t port = xport ^ 0x2112;
            std::array<std::uint8_t, 4> ip{};
            ip[0] = msg[pos + 4 + 4] ^ 0x21;
            ip[1] = msg[pos + 4 + 5] ^ 0x12;
            ip[2] = msg[pos + 4 + 6] ^ 0xA4;
            ip[3] = msg[pos + 4 + 7] ^ 0x42;
            char buf[INET_ADDRSTRLEN] = {0};
            if (::inet_ntop(AF_INET, ip.data(), buf, sizeof(buf)) == nullptr) {
                return std::nullopt;
            }
            mapped = std::string(buf) + ":" + std::to_string(port);
        }
        pos += 4 + ((alen + 3) & ~std::size_t{3});
    }
    if (!mi_off || !mapped) return std::nullopt;

    std::array<std::uint8_t, kHmacSha1Size> digest{};
    if (!hmac_sha1(password, std::span<const std::uint8_t>(msg.data(), *mi_off),
                   digest)) {
        return std::nullopt;
    }
    if (!std::equal(digest.begin(), digest.end(), msg.begin() + *mi_off + 4)) {
        return std::nullopt;
    }
    return mapped;
}

// ---------------------------------------------------------------------------
// TcpIceDriver
// ---------------------------------------------------------------------------

TcpIceDriver::TcpIceDriver(Config config) : config_(std::move(config)) {
    std::random_device rd;
    for (int i = 0; i < 8; ++i) {
        tiebreaker_ = (tiebreaker_ << 8) | (static_cast<std::uint64_t>(rd()) & 0xFF);
    }
}

TcpIceDriver::~TcpIceDriver() {
    stop();
}

bool TcpIceDriver::gather(std::vector<TcpCandidate>& out) {
    TcpSocket listener;
    if (!listener.open()) return false;
    if (!listener.bind(config_.bind_host, 0)) return false;
    if (!listener.listen()) return false;
    const std::uint16_t passive_port = listener.local_port();
    if (passive_port == 0) return false;

    TcpSocket active;
    if (!active.open()) return false;
    if (!active.bind(config_.bind_host, 0)) return false;
    const std::uint16_t active_port = active.local_port();
    if (active_port == 0) return false;

    TcpCandidate passive;
    passive.foundation = "tcp-1";
    passive.host = candidate_host(listener, config_.bind_host);
    passive.port = passive_port;
    passive.tcptype = TcpCandidateRole::Passive;

    TcpCandidate active_candidate;
    active_candidate.foundation = "tcp-2";
    active_candidate.host = candidate_host(active, config_.bind_host);
    active_candidate.port = active_port;
    active_candidate.tcptype = TcpCandidateRole::Active;

    {
        std::lock_guard<std::mutex> lk(mu_);
        passive_listener_ = std::move(listener);
        active_socket_ = std::move(active);
        local_.clear();
        local_.push_back(passive);
        local_.push_back(active_candidate);
    }
    out.push_back(std::move(passive));
    out.push_back(std::move(active_candidate));
    return true;
}

std::vector<TcpCandidate> TcpIceDriver::local_candidates() const {
    std::lock_guard<std::mutex> lk(mu_);
    return local_;
}

void TcpIceDriver::set_remote_credentials(std::string remote_ufrag,
                                          std::string remote_password) {
    std::lock_guard<std::mutex> lk(mu_);
    remote_ufrag_ = std::move(remote_ufrag);
    remote_password_ = std::move(remote_password);
}

void TcpIceDriver::add_remote(TcpCandidate candidate) {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& c : remote_) {
        if (c.host == candidate.host && c.port == candidate.port &&
            c.tcptype == candidate.tcptype) {
            return;  // trickle may re-deliver; ignore duplicates
        }
    }
    remote_.push_back(std::move(candidate));
}

void TcpIceDriver::start() {
    std::lock_guard<std::mutex> lk(mu_);
    if (thread_.joinable()) return;
    stop_.store(false);
    thread_ = std::thread([this] { thread_main(); });
}

void TcpIceDriver::stop() {
    std::thread t;
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_.store(true);
        // Retire (not close) from under the check thread's bounded waits;
        // the physical close runs in the sockets' destructors after join.
        passive_listener_.close();
        active_socket_.close();
        if (thread_.joinable()) {
            t = std::move(thread_);
        }
    }
    if (t.joinable()) {
        t.join();
    }
}

std::optional<std::pair<TcpCandidate, TcpCandidate>>
TcpIceDriver::nominated() const {
    std::lock_guard<std::mutex> lk(mu_);
    return nominated_;
}

void TcpIceDriver::thread_main() {
    // The outbound check needs the peer's credentials and at least one
    // remote candidate (both arrive via the seam, possibly asynchronously).
    for (;;) {
        if (stop_.load()) return;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (nominated_ || (!remote_ufrag_.empty() && !remote_.empty())) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (stop_.load()) return;

    // Outbound first (our active → the peer's passive); the accepting side
    // is caught by the inbound loop below.
    if (try_outbound()) return;

    while (!stop_.load()) {
        if (try_inbound()) return;
    }
}

bool TcpIceDriver::try_outbound() {
    std::vector<TcpCandidate> passives;
    std::string remote_password;
    std::string remote_ufrag;
    TcpCandidate local_active;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (nominated_) return true;
        for (const auto& c : remote_) {
            if (c.tcptype == TcpCandidateRole::Passive) {
                passives.push_back(c);
            }
        }
        remote_password = remote_password_;
        remote_ufrag = remote_ufrag_;
        if (local_.size() > 1) {
            local_active = local_[1];
        }
    }
    if (passives.empty() || remote_ufrag.empty() || local_active.port == 0) {
        return false;
    }

    for (const auto& remote : passives) {
        if (stop_.load()) return false;
        if (outbound_check(remote, local_active, remote_ufrag, remote_password)) {
            return true;
        }
    }
    return false;
}

bool TcpIceDriver::outbound_check(const TcpCandidate& remote,
                                  const TcpCandidate& local_active,
                                  const std::string& remote_ufrag,
                                  const std::string& remote_password) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!active_socket_.is_open() && !rebind_active()) {
            return false;
        }
    }

    if (!active_socket_.connect(remote.host, remote.port, kConnectTimeoutMs)) {
        std::lock_guard<std::mutex> lk(mu_);
        active_socket_.close();  // unusable after a failed connect
        return false;
    }

    const std::string username = config_.local_ufrag + ":" + remote_ufrag;
    const auto transaction = random_transaction_id();
    const auto request = stun_build_binding_request(
        username, remote_password, local_active.priority(), tiebreaker_,
        /*use_candidate=*/true, transaction);
    if (request.empty()) return false;

    const auto framed = frame_message(request);
    if (framed.empty() || !active_socket_.send_all(framed, kSendTimeoutMs)) {
        return false;
    }

    std::vector<std::uint8_t> response;
    if (!read_one_frame(active_socket_, response, kResponseTimeoutMs)) {
        std::lock_guard<std::mutex> lk(mu_);
        active_socket_.close();
        return false;
    }
    if (!stun_verify_binding_response(response, remote_password, transaction)) {
        std::lock_guard<std::mutex> lk(mu_);
        active_socket_.close();
        return false;
    }

    nominate(local_active, remote);
    return true;
}

bool TcpIceDriver::try_inbound() {
    auto stream = passive_listener_.accept(kAcceptTimeoutMs);
    if (!stream) return false;

    std::vector<std::uint8_t> request;
    if (!read_one_frame(*stream, request, kRequestTimeoutMs)) return false;

    const StunRequestInfo info =
        stun_verify_binding_request(request, config_.local_password);
    if (!info.valid) return false;

    // The peer built the username as "<their ufrag>:<our ufrag>"; the part
    // after the colon must be our own ufrag (RFC 8445 §7.1.2.3).
    const std::string suffix = ":" + config_.local_ufrag;
    if (info.username.size() < suffix.size() ||
        info.username.compare(info.username.size() - suffix.size(),
                              suffix.size(), suffix) != 0) {
        return false;
    }

    const std::string peer = stream->peer_endpoint();
    std::string peer_host;
    std::uint16_t peer_port = 0;
    if (!split_endpoint(peer, &peer_host, &peer_port)) return false;

    const auto remote_match = find_remote(peer_host, peer_port);
    if (!remote_match) return false;

    const auto response =
        stun_build_binding_response(info.username, config_.local_password,
                                    info.transaction_id, peer_host, peer_port);
    if (response.empty()) return false;
    const auto framed = frame_message(response);
    if (framed.empty() || !stream->send_all(framed, kSendTimeoutMs)) {
        return false;
    }

    TcpCandidate local_passive;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!local_.empty()) {
            local_passive = local_[0];
        }
    }
    if (local_passive.port == 0) return false;

    nominate(local_passive, *remote_match);
    return true;
}

std::optional<TcpCandidate> TcpIceDriver::find_remote(const std::string& host,
                                                      std::uint16_t port) {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& c : remote_) {
        if (c.port != port) continue;
        if (c.host.empty() || c.host == "0.0.0.0" || c.host == host) {
            return c;
        }
    }
    return std::nullopt;
}

void TcpIceDriver::nominate(const TcpCandidate& local,
                            const TcpCandidate& remote) {
    std::function<void(const TcpCandidate&, const TcpCandidate&)> callback;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (nominated_) return;  // nominate once — the winner never flaps
        nominated_ = std::make_pair(local, remote);
        callback = on_nominated;
    }
    if (callback) {
        callback(local, remote);
    }
}

bool TcpIceDriver::rebind_active() {
    // Caller holds mu_.  A fresh socket bound to the same advertised port:
    // after a failed connect the old socket is unusable, but the candidate
    // port must stay the same or the peer cannot match our connection's
    // source address.
    const std::uint16_t advertised = (local_.size() > 1) ? local_[1].port : 0;
    if (advertised == 0) return false;
    TcpSocket fresh;
    if (!fresh.open()) return false;
    if (!fresh.bind(config_.bind_host, advertised)) return false;
    if (fresh.local_port() != advertised) return false;
    active_socket_.close();
    active_socket_ = std::move(fresh);
    return true;
}

bool TcpIceDriver::read_one_frame(TcpSocket& sock, std::vector<std::uint8_t>& msg,
                                  int budget_ms) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(budget_ms);
    TcpUnframer unframer;
    std::array<std::uint8_t, 4096> buf{};
    std::vector<std::vector<std::uint8_t>> frames;
    for (;;) {
        if (stop_.load()) return false;
        if (clock::now() >= deadline) return false;
        const std::ptrdiff_t n = sock.recv_some(buf.data(), buf.size());
        if (n < 0) return false;  // error or orderly close by the peer
        if (n == 0) continue;     // bounded-wait slice; re-check the deadline
        frames.clear();
        if (!unframer.push(std::span<const std::uint8_t>(
                               buf.data(), static_cast<std::size_t>(n)),
                           frames)) {
            return false;  // framing error — the stream is unusable
        }
        if (!frames.empty()) {
            msg = std::move(frames.front());
            return true;
        }
    }
}

}  // namespace nimrtc::ice
