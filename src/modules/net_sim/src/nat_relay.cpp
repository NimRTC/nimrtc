/**
 * @file src/nat_relay.cpp
 * @brief The simulated NAT's implementation.
 *
 * The relay is one thread, two sockets and a small scheduler.  The
 * interesting part is the *translation*, and it is worth spelling out
 * here because the header only says what it does:
 *
 *   - peer A advertises `endpoint_a()` (the relay's **first** socket),
 *     so peer B's traffic arrives on socket 0;
 *   - peer B advertises `endpoint_b()` (the relay's **second** socket),
 *     so peer A's traffic arrives on socket 1;
 *   - a packet that arrives on socket `i` is forwarded to the peer
 *     learned on socket `1 - i`, **through socket `1 - i`** — so the
 *     packet's source address is the relay's other address, which is
 *     the address the receiving peer was told to expect.
 *
 * The two peers' real addresses are *learned* from the traffic (each
 * socket only ever sees one peer), so the relay needs no configuration
 * and no signalling of its own.
 */

#include "nimrtc/net_sim/nat_relay.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <cerrno>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace nimrtc::net_sim {
namespace {

#ifdef _WIN32
using socket_t   = SOCKET;
using addr_len_t = int;
using send_buf_t = const char*;
using recv_buf_t = char*;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
void close_socket(socket_t s) noexcept { ::closesocket(s); }
#else
using socket_t   = int;
using addr_len_t = ::socklen_t;
using send_buf_t = const void*;
using recv_buf_t = void*;
constexpr socket_t kInvalidSocket = -1;
void close_socket(socket_t s) noexcept { (void)::close(s); }
#endif

/**
 * @brief The length argument of `sendto()` / `recvfrom()`.
 *
 * Winsock declares it `int`; the BSDs (Linux, macOS) declare it
 * `size_t`.  A single spelling cannot satisfy both: the `int` cast
 * Winsock needs is a narrowing conversion on the BSDs, and
 * `-Wsign-conversion` reports it there.  The cast is therefore made
 * where the declaration requires it, and the length type follows the
 * platform.
 */
#ifdef _WIN32
using io_len_t = int;
#else
using io_len_t = std::size_t;
#endif

/**
 * @brief Put a socket into non-blocking mode.
 *
 * The relay reads its sockets with a non-blocking `recvfrom()` and
 * treats "would block" as "nothing yet".  That is the only portable
 * way to ask a socket "do you have a datagram?" on Windows:
 *
 *   - `select()` **and** `WSAPoll()` are both subject to
 *     `FD_SETSIZE` (64) — Microsoft's own `WSAPoll` documentation says
 *     it uses the same underlying mechanism as `select()`.  A Winsock
 *     handle is *not* a small integer: the sockets this relay gets are
 *     routinely numbered above 64, and both then **silently ignore**
 *     them, so the relay would wait forever for traffic that has in
 *     fact arrived.
 *   - `ioctlsocket(FIONREAD)` has no such limit but does not report
 *     pending *datagrams* reliably on a UDP socket.
 *
 * A non-blocking `recvfrom()` has neither problem: it either hands
 * over the datagram or says there is none, for any handle.
 */
bool set_nonblocking(socket_t s) noexcept {
#ifdef _WIN32
    u_long on = 1;
    return ::ioctlsocket(s, FIONBIO, &on) == 0;
#else
    const int flags = ::fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

/** True when the last socket error means "nothing to read right now". */
bool would_block() noexcept {
#ifdef _WIN32
    const int err = ::WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

/** The largest datagram the relay will read or write. */
constexpr int kMaxDatagram = 65535;

/** How long the relay's thread sleeps between polls, in milliseconds.
 *
 *  The thread is a poll loop rather than a blocking reader so that the
 *  delayed (reordered / rate-limited) packets can be released on time
 *  and so that `stop()` returns promptly. */
constexpr int kPollIntervalMs = 2;

/** The relay's socket buffer, in bytes. */
constexpr int kSocketBufferBytes = 1 << 20;

/** The most packets the relay will hold while it waits for the other
 *  peer to show up. */
constexpr std::size_t kMaxBufferedPackets = 512;

/** The most packets the relay will hold for a delayed send. */
constexpr std::size_t kMaxPendingPackets = 4096;

/** The monotonic clock, in microseconds. */
std::int64_t now_us() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/** Bring up Winsock once, on Windows.  A no-op elsewhere. */
bool ensure_sockets_ready(std::string* error) {
#ifdef _WIN32
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA data{};
        ok = (::WSAStartup(MAKEWORD(2, 2), &data) == 0);
    });
    if (!ok) {
        if (error) *error = "WSAStartup failed";
        return false;
    }
#endif
    (void)error;
    return true;
}

/** Two socket addresses, compared for equality. */
bool same_address(const sockaddr_in& a, const sockaddr_in& b) noexcept {
    return a.sin_family == b.sin_family && a.sin_port == b.sin_port &&
           a.sin_addr.s_addr == b.sin_addr.s_addr;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// NatRelay::Impl
// ---------------------------------------------------------------------------

struct NatRelay::Impl {
    Config config;

    socket_t sock[2] = {kInvalidSocket, kInvalidSocket};
    std::uint16_t port[2] = {0, 0};

    // The learned peers: the address of the peer whose traffic arrives
    // on socket `i`.  The relay never needs to be told them.
    sockaddr_in peer[2] = {};
    bool peer_known[2] = {false, false};

    // The relay's own address each socket is bound to.  A datagram the
    // relay sends through socket 0 must not come back into socket 0, so
    // the relay can recognise its own traffic by source address.
    sockaddr_in local[2] = {};

    // The source addresses of the relay's own sockets, used to drop any
    // packet that loops back (a send through socket 0 to a peer that is
    // itself socket 1 on the same host would otherwise be read as fresh
    // input on socket 1 and forwarded again, forever).
    bool is_own_address(const sockaddr_in& src) const noexcept {
        for (int i = 0; i < 2; ++i) {
            if (local[i].sin_family != 0 && same_address(local[i], src)) return true;
        }
        return false;
    }

    // One shaper per direction: the two directions are independent
    // streams, and sharing a generator between them would couple their
    // draws.
    ImpairmentShaper shaper0;
    ImpairmentShaper shaper1;

    // The packets waiting for their turn (delayed sends).
    struct Pending {
        std::int64_t due_us   = 0;
        int          socket   = 0;   // the socket to send through
        sockaddr_in  dest     = {};
        std::vector<std::uint8_t> payload;
    };
    std::vector<Pending> pending;

    // The packets that arrived before their destination peer was known.
    struct Buffered {
        int          socket = 0;
        sockaddr_in  src    = {};
        std::vector<std::uint8_t> payload;
    };
    std::vector<Buffered> buffered;

    std::thread worker;
    std::atomic<bool> running{false};

    mutable std::mutex mutex;
    Stats stats;

    explicit Impl(const Config& cfg)
        : config(cfg),
          shaper0(cfg.impairment),
          shaper1([&] {
              ImpairmentConfig copy = cfg.impairment;
              // The two directions use different generator seeds, so
              // they do not draw the same numbers in the same order.
              copy.seed = cfg.impairment.seed ? (cfg.impairment.seed ^ 0x5DEECE66DULL)
                                              : 0x5DEECE66DULL;
              return copy;
          }()) {}

    ~Impl() { shutdown(); }

    void shutdown() noexcept {
        if (running.exchange(false)) {
            if (worker.joinable()) worker.join();
        }
        for (auto& s : sock) {
            if (s != kInvalidSocket) {
                close_socket(s);
                s = kInvalidSocket;
            }
        }
    }

    /** Bind the two sockets; returns false and fills `error` on failure. */
    bool bind(std::string* error) {
        for (int i = 0; i < 2; ++i) {
            sock[i] = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock[i] == kInvalidSocket) {
                if (error) {
                    *error = "socket() failed for the relay's socket " +
                             std::to_string(i);
                }
                return false;
            }
            int buffer = kSocketBufferBytes;
            (void)::setsockopt(sock[i], SOL_SOCKET, SO_RCVBUF,
                               reinterpret_cast<const char*>(&buffer),
                               sizeof(buffer));
            (void)::setsockopt(sock[i], SOL_SOCKET, SO_SNDBUF,
                               reinterpret_cast<const char*>(&buffer),
                               sizeof(buffer));

            // Non-blocking: the relay asks the socket whether a datagram
            // is waiting by attempting a read and accepting "would block"
            // as the answer.
            if (!set_nonblocking(sock[i])) {
                if (error) {
                    *error = "the relay's socket " + std::to_string(i) +
                             " could not be put into non-blocking mode";
                }
                return false;
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port   = 0;   // the OS picks an ephemeral port
            if (::inet_pton(AF_INET, config.bind_host.c_str(), &addr.sin_addr) != 1) {
                if (error) {
                    *error = "the relay's bind address '" + config.bind_host +
                             "' is not a valid IPv4 address";
                }
                return false;
            }
            if (::bind(sock[i], reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                if (error) {
                    *error = "bind(" + config.bind_host + ":0) failed for the "
                             "relay's socket " + std::to_string(i);
                }
                return false;
            }
            sockaddr_in bound{};
            addr_len_t len = sizeof(bound);
            if (::getsockname(sock[i], reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
                port[i] = ntohs(bound.sin_port);
                local[i] = bound;
            }
        }
        return true;
    }

    /** The relay's main loop. */
    void run() {
        std::vector<std::uint8_t> buffer(kMaxDatagram);

        while (running.load(std::memory_order_relaxed)) {
            // ---- Read whatever arrived ------------------------------------
            // The sockets are non-blocking, so a `recvfrom()` that finds
            // nothing returns immediately with "would block".
            for (int i = 0; i < 2; ++i) {
                if (sock[i] == kInvalidSocket) continue;
                for (;;) {
                    sockaddr_in src{};
                    addr_len_t src_len = sizeof(src);
                    const auto n = ::recvfrom(sock[i],
                                              reinterpret_cast<recv_buf_t>(buffer.data()),
                                              static_cast<io_len_t>(buffer.size()), 0,
                                              reinterpret_cast<sockaddr*>(&src), &src_len);
                    if (n > 0) {
                        handle(i, src, buffer.data(), static_cast<std::size_t>(n));
                        continue;
                    }
                    if (n == 0) continue;   // an empty datagram
                    if (would_block()) break;
                    break;                // a real error: give up on this socket
                }
            }

            // ---- Release the buffered packets whose peer showed up --------
            flush_buffered();

            // ---- Release the packets whose delay has expired ----------------
            flush_pending();

            // ---- Wait a little before looking again ------------------------
            // A short sleep keeps the loop responsive to `stop()` and to
            // the delayed packets' due times without spinning the CPU.
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
        }
    }

    /** One packet arrived on socket `i`. */
    void handle(int i, const sockaddr_in& src, const std::uint8_t* data,
                std::size_t len) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++stats.packets_received;
        }

        // A packet the relay sent itself.  Both sockets are bound to the
        // same loopback address, so a datagram sent through one of them
        // can be delivered back into the other; forwarding it again
        // would make the relay talk to itself forever.
        if (is_own_address(src)) return;

        if (peer_known[i]) {
            if (!same_address(peer[i], src)) {
                // A source the relay did not expect.  The two-socket
                // design means each socket only ever sees one peer, so
                // this is either a stray packet or a peer that moved;
                // either way it is not part of the session.
                std::lock_guard<std::mutex> lock(mutex);
                ++stats.packets_unknown_source;
                return;
            }
        } else {
            peer[i]      = src;
            peer_known[i] = true;
        }

        if (!peer_known[1 - i]) {
            // The other peer has not shown up yet.  The packet is held
            // (bounded) rather than dropped: the peers start their
            // connectivity checks at slightly different moments, and
            // the first packets of the session are exactly the ones
            // that must not be lost.
            if (buffered.size() >= kMaxBufferedPackets) {
                std::lock_guard<std::mutex> lock(mutex);
                ++stats.packets_dropped_buffer_full;
                return;
            }
            Buffered item;
            item.socket  = i;
            item.src     = src;
            item.payload.assign(data, data + len);
            buffered.push_back(std::move(item));
            std::lock_guard<std::mutex> lock(mutex);
            ++stats.packets_buffered;
            return;
        }

        enqueue(i, peer[1 - i], data, len);
    }

    /** Apply the profile and schedule the packet. */
    void enqueue(int arrived_on, const sockaddr_in& dest, const std::uint8_t* data,
                 std::size_t len) {
        PacketDecision decision{};
        if (config.impaired) {
            decision = (arrived_on == 0) ? shaper0.shape(len, now_us())
                                         : shaper1.shape(len, now_us());
        }

        if (decision.dropped) {
            std::lock_guard<std::mutex> lock(mutex);
            ++stats.packets_dropped;
            return;
        }

        if (pending.size() >= kMaxPendingPackets) {
            std::lock_guard<std::mutex> lock(mutex);
            ++stats.packets_dropped_buffer_full;
            return;
        }

        Pending item;
        item.due_us  = now_us() + decision.delay_us;
        item.socket  = 1 - arrived_on;   // the source the peer expects
        item.dest    = dest;
        item.payload.assign(data, data + len);
        pending.push_back(std::move(item));

        if (decision.reordered) {
            std::lock_guard<std::mutex> lock(mutex);
            ++stats.packets_reordered;
        }
    }

    /** The buffered packets can go out now that their peer is known. */
    void flush_buffered() {
        if (buffered.empty()) return;
        if (!peer_known[0] || !peer_known[1]) return;

        std::vector<Buffered> ready;
        ready.swap(buffered);
        for (const Buffered& item : ready) {
            enqueue(item.socket, peer[1 - item.socket], item.payload.data(),
                    item.payload.size());
        }
    }

    /** Send the packets whose delay has expired. */
    void flush_pending() {
        if (pending.empty()) return;
        const std::int64_t now = now_us();

        // The queue is small (a few hundred entries at the soak's
        // rates), so a linear scan for the earliest due time is
        // cheaper than maintaining a heap.
        std::size_t index = pending.size();
        for (std::size_t i = 0; i < pending.size(); ++i) {
            if (pending[i].due_us <= now) {
                index = i;
                break;
            }
        }
        if (index == pending.size()) return;

        // Send every packet that is due in this pass.  Sending one per
        // poll cycle would cap the relay at a few hundred packets per
        // second, which the soak would read as loss.
        while (index != pending.size()) {
            Pending item = std::move(pending[index]);
            pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(index));

            if (item.socket < 0 || item.socket > 1 ||
                sock[item.socket] == kInvalidSocket) {
                // Skip to the next due packet.
            } else {
                const auto sent =
                    ::sendto(sock[item.socket],
                             reinterpret_cast<send_buf_t>(item.payload.data()),
                             static_cast<io_len_t>(item.payload.size()), 0,
                             reinterpret_cast<const sockaddr*>(&item.dest),
                             static_cast<addr_len_t>(sizeof(item.dest)));
                if (sent > 0) {
                    std::lock_guard<std::mutex> lock(mutex);
                    ++stats.packets_forwarded;
                    stats.bytes_forwarded += static_cast<std::uint64_t>(sent);
                }
            }

            index = pending.size();
            for (std::size_t i = 0; i < pending.size(); ++i) {
                if (pending[i].due_us <= now) {
                    index = i;
                    break;
                }
            }
        }
    }
};

// ---------------------------------------------------------------------------
// NatRelay
// ---------------------------------------------------------------------------

NatRelay::NatRelay(const Config& config) : impl_(new Impl(config)) {}

NatRelay::~NatRelay() { delete impl_; }

bool NatRelay::start(std::string* error) {
    if (impl_->running.load()) {
        if (error) *error = "the relay is already running";
        return false;
    }
    if (!ensure_sockets_ready(error)) return false;
    if (!impl_->bind(error)) {
        impl_->shutdown();
        return false;
    }
    impl_->running.store(true);
    impl_->worker = std::thread([impl = impl_] { impl->run(); });
    return true;
}

void NatRelay::stop() noexcept { impl_->shutdown(); }

bool NatRelay::running() const noexcept {
    return impl_->running.load(std::memory_order_relaxed);
}

std::string NatRelay::endpoint_a() const {
    if (impl_->port[0] == 0) return {};
    return impl_->config.bind_host + ":" + std::to_string(impl_->port[0]);
}

std::string NatRelay::endpoint_b() const {
    if (impl_->port[1] == 0) return {};
    return impl_->config.bind_host + ":" + std::to_string(impl_->port[1]);
}

NatRelay::Stats NatRelay::stats() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->stats;
}

} // namespace nimrtc::net_sim
