// tcp_socket.cpp — cross-platform TCP socket for the ICE-over-TCP path.
//
// See tcp_socket.hpp for the contract.  The two fd-lifetime disciplines
// (retire-not-close, bounded-poll-before-syscall) are copied from
// UdpSocket (src/modules/raw_udp/src/udp_socket.cpp), per ADR-028.

#include "tcp_socket.hpp"

#include <nimrtc/core/log.hpp>

#include <cstring>
#include <string>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
using socklen_t_ = int;
#else
#  include <arpa/inet.h>
#  include <cerrno>
#  include <fcntl.h>
#  include <ifaddrs.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
using socklen_t_ = socklen_t;
#endif

namespace nimrtc::ice {

namespace {

// Parse "host:port" (or just "host") into sockaddr_in.  Same shape as
// UdpSocket's parse_endpoint_v4: empty / malformed → INADDR_ANY:0.
bool parse_endpoint_v4(const std::string& s, sockaddr_in* out) {
    if (!out) return false;
    std::memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_addr.s_addr = htonl(INADDR_ANY);
    out->sin_port = 0;

    if (s.empty()) return true;

    const auto colon = s.rfind(':');
    const std::string host = (colon == std::string::npos) ? s : s.substr(0, colon);
    const std::string port = (colon == std::string::npos) ? std::string{}
                                                           : s.substr(colon + 1);

    if (!host.empty()) {
        if (::inet_pton(AF_INET, host.c_str(), &out->sin_addr) != 1) {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            addrinfo* res = nullptr;
            if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
                return false;
            }
            std::memcpy(&out->sin_addr,
                        &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr,
                        sizeof(out->sin_addr));
            ::freeaddrinfo(res);
        }
    }
    if (!port.empty()) {
        const long p = std::strtol(port.c_str(), nullptr, 10);
        if (p <= 0 || p > 65535) return false;
        out->sin_port = htons(static_cast<std::uint16_t>(p));
    }
    return true;
}

std::string format_endpoint_v4(const sockaddr_in& sa) {
    char ip[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof(ip));
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s:%u", ip, ntohs(sa.sin_port));
    return std::string(buf);
}

// The first non-loopback IPv4 address, falling back to the loopback.
// A candidate advertised as "0.0.0.0" is not connectable, so a wildcard
// bind reports a concrete local address instead.
std::string find_routable_local_ipv4() {
#ifdef _WIN32
    char host[256] = {0};
    if (::gethostname(host, sizeof(host)) == 0) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        addrinfo* res = nullptr;
        if (::getaddrinfo(host, nullptr, &hints, &res) == 0 && res) {
            for (addrinfo* p = res; p; p = p->ai_next) {
                const auto* sa = reinterpret_cast<const sockaddr_in*>(p->ai_addr);
                const std::uint32_t ip = ntohl(sa->sin_addr.s_addr);
                if (ip == 0) continue;
                if ((ip & 0xFF000000u) == 0x7F000000u) continue;  // 127/8
                char buf[INET_ADDRSTRLEN] = {0};
                ::inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf));
                ::freeaddrinfo(res);
                return std::string(buf);
            }
            ::freeaddrinfo(res);
        }
    }
#else
    ifaddrs* ifs = nullptr;
    if (::getifaddrs(&ifs) == 0) {
        for (ifaddrs* p = ifs; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            const auto* sa = reinterpret_cast<const sockaddr_in*>(p->ifa_addr);
            const std::uint32_t ip = ntohl(sa->sin_addr.s_addr);
            if (ip == 0) continue;
            if ((ip & 0xFF000000u) == 0x7F000000u) continue;  // 127/8
            char buf[INET_ADDRSTRLEN] = {0};
            ::inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf));
            ::freeifaddrs(ifs);
            return std::string(buf);
        }
        ::freeifaddrs(ifs);
    }
#endif
    return "127.0.0.1";
}

#ifdef _WIN32
bool wait_readable_impl(std::uintptr_t sock, int timeout_ms) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(static_cast<SOCKET>(sock), &readfds);
    timeval tv{};
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return ::select(0, &readfds, nullptr, nullptr, &tv) > 0;
}

bool wait_writable_impl(std::uintptr_t sock, int timeout_ms) {
    fd_set writefds;
    FD_ZERO(&writefds);
    FD_SET(static_cast<SOCKET>(sock), &writefds);
    timeval tv{};
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return ::select(0, nullptr, &writefds, nullptr, &tv) > 0;
}
#else
bool wait_readable_impl(int sock, int timeout_ms) {
    pollfd pfd{};
    pfd.fd     = sock;
    pfd.events = POLLIN;
    return ::poll(&pfd, 1, timeout_ms) > 0;
}

bool wait_writable_impl(int sock, int timeout_ms) {
    pollfd pfd{};
    pfd.fd     = sock;
    pfd.events = POLLOUT;
    return ::poll(&pfd, 1, timeout_ms) > 0;
}
#endif

void set_nonblocking(std::uintptr_t sock) {
#ifdef _WIN32
    u_long mode = 1;
    ::ioctlsocket(static_cast<SOCKET>(sock), FIONBIO, &mode);
#else
    const int flags = ::fcntl(static_cast<int>(sock), F_GETFL, 0);
    if (flags != -1) ::fcntl(static_cast<int>(sock), F_SETFL, flags | O_NONBLOCK);
#endif
}

}  // namespace

TcpSocket::TcpSocket(TcpSocket&& other) noexcept
    : sock_(other.sock_.load(std::memory_order_acquire))
#if defined(_WIN32)
    , winsock_initialized_(other.winsock_initialized_)
#endif
{
    other.sock_.store(kInvalid, std::memory_order_release);
#if defined(_WIN32)
    other.winsock_initialized_ = false;
#endif
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close();
        sock_.store(other.sock_.load(std::memory_order_acquire),
                    std::memory_order_release);
#if defined(_WIN32)
        winsock_initialized_ = other.winsock_initialized_;
#endif
        other.sock_.store(kInvalid, std::memory_order_release);
#if defined(_WIN32)
        other.winsock_initialized_ = false;
#endif
    }
    return *this;
}

bool TcpSocket::wait_readable(socket_t sock, int timeout_ms) const noexcept {
    return wait_readable_impl(sock, timeout_ms);
}

bool TcpSocket::wait_writable(socket_t sock, int timeout_ms) const noexcept {
    return wait_writable_impl(sock, timeout_ms);
}

bool TcpSocket::open() noexcept {
    if (sock_.load(std::memory_order_acquire) != kInvalid) {
        return true;  // already open
    }

#ifdef _WIN32
    if (!winsock_initialized_) {
        WSADATA wsa{};
        if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            core::log::Logger::instance().error("tcp_socket: WSAStartup failed");
            return false;
        }
        winsock_initialized_ = true;
    }
#endif

    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
    if (s == kInvalid) {
        core::log::Logger::instance().error(
            "tcp_socket: socket() failed err=" + std::to_string(::WSAGetLastError()));
        return false;
    }
#else
    if (s < 0) {
        core::log::Logger::instance().error(
            "tcp_socket: socket() failed errno=" + std::to_string(errno));
        return false;
    }
#endif

    set_nonblocking(s);
    sock_.store(s, std::memory_order_release);
    return true;
}

bool TcpSocket::bind(const std::string& host, std::uint16_t port) noexcept {
    const socket_t sock = sock_.load(std::memory_order_acquire);
    if (sock == kInvalid) return false;

    sockaddr_in addr{};
    if (!parse_endpoint_v4(host, &addr)) {
        core::log::Logger::instance().error("tcp_socket: invalid host='" + host + "'");
        return false;
    }
    addr.sin_port = htons(port);

    // SO_REUSEADDR lets the ICE driver rebind the advertised port after a
    // failed outbound connect, even while the old socket lingers in
    // TIME_WAIT (the active candidate's port must stay the same for the
    // peer to match the connection's source address).
#ifdef _WIN32
    const BOOL reuse = TRUE;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
    const int reuse = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

    if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
#ifdef _WIN32
        core::log::Logger::instance().error("tcp_socket: bind(" + host + ":" +
            std::to_string(port) + ") failed err=" + std::to_string(::WSAGetLastError()));
#else
        core::log::Logger::instance().error("tcp_socket: bind(" + host + ":" +
            std::to_string(port) + ") failed errno=" + std::to_string(errno));
#endif
        return false;
    }
    return true;
}

bool TcpSocket::listen(int backlog) noexcept {
    const socket_t sock = sock_.load(std::memory_order_acquire);
    if (sock == kInvalid) return false;
    return ::listen(sock, backlog) == 0;
}

std::optional<TcpSocket> TcpSocket::accept(int timeout_ms) noexcept {
    const socket_t sock = sock_.load(std::memory_order_acquire);
    if (sock == kInvalid) return std::nullopt;

    // Poll before accept: the accept must never be the unbounded wait
    // (discipline 2) — a bounded poll first gives the caller a stop-flag
    // check every kRecvPollTimeoutMs.
    if (!wait_readable(sock, timeout_ms)) {
        return std::nullopt;
    }
    if (sock_.load(std::memory_order_acquire) != sock) {
        return std::nullopt;  // closed while waiting
    }

    sockaddr_in addr{};
    socklen_t_ addr_len = sizeof(addr);
#ifdef _WIN32
    const SOCKET accepted = ::accept(sock, reinterpret_cast<sockaddr*>(&addr), &addr_len);
    if (accepted == INVALID_SOCKET) {
        return std::nullopt;
    }
    set_nonblocking(static_cast<std::uintptr_t>(accepted));
#else
    const int accepted = ::accept(sock, reinterpret_cast<sockaddr*>(&addr), &addr_len);
    if (accepted < 0) {
        return std::nullopt;
    }
#endif

    TcpSocket stream;
    stream.sock_.store(static_cast<socket_t>(accepted), std::memory_order_release);
#if defined(_WIN32)
    stream.winsock_initialized_ = winsock_initialized_;
#endif
    return stream;
}

bool TcpSocket::connect(const std::string& host, std::uint16_t port,
                        int timeout_ms) noexcept {
    const socket_t sock = sock_.load(std::memory_order_acquire);
    if (sock == kInvalid) return false;

    sockaddr_in addr{};
    if (!parse_endpoint_v4(host, &addr)) return false;
    addr.sin_port = htons(port);

    // The socket is non-blocking, so connect() returns immediately with
    // "in progress"; the bounded wait on writability is what bounds the
    // overall connect (discipline 2).
    if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
#ifdef _WIN32
        const int e = ::WSAGetLastError();
        if (e != WSAEWOULDBLOCK) {
            return false;
        }
#else
        if (errno != EINPROGRESS && errno != EINTR) {
            return false;
        }
#endif
    }

    if (!wait_writable(sock, timeout_ms)) {
        return false;
    }
    if (sock_.load(std::memory_order_acquire) != sock) {
        return false;  // closed while waiting
    }

    // A successful non-blocking connect is reported via SO_ERROR.
    int so_error = 0;
    socklen_t_ err_len = sizeof(so_error);
    if (::getsockopt(sock, SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char*>(&so_error), &err_len) < 0) {
        return false;
    }
    return so_error == 0;
}

bool TcpSocket::send_all(std::span<const std::uint8_t> bytes, int timeout_ms) noexcept {
    const socket_t sock = sock_.load(std::memory_order_acquire);
    if (sock == kInvalid) return false;

    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto* data = reinterpret_cast<const char*>(bytes.data() + sent);
        const std::size_t remaining = bytes.size() - sent;
#ifdef _WIN32
        const int rc = ::send(sock, data, static_cast<int>(remaining), 0);
        if (rc < 0) {
            const int e = ::WSAGetLastError();
            if (e != WSAEWOULDBLOCK) return false;
            if (!wait_writable(sock, timeout_ms)) return false;
            if (sock_.load(std::memory_order_acquire) != sock) return false;
            continue;  // retry the same window
        }
#else
        const ssize_t rc = ::send(sock, data, remaining, 0);
        if (rc < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) return false;
            if (!wait_writable(sock, timeout_ms)) return false;
            if (sock_.load(std::memory_order_acquire) != sock) return false;
            continue;
        }
#endif
        sent += static_cast<std::size_t>(rc);
    }
    return true;
}

std::ptrdiff_t TcpSocket::recv_some(void* buf, std::size_t size) noexcept {
    const socket_t sock = sock_.load(std::memory_order_acquire);
    if (sock == kInvalid) return -1;

    // Poll before recv (discipline 2): the syscall only ever runs when a
    // byte (or an error) is known to be pending, and the caller gets a
    // wake-up at least every kRecvPollTimeoutMs to observe a stop flag.
    if (!wait_readable(sock, kRecvPollTimeoutMs)) {
        return 0;  // timed out, nothing pending
    }
    if (sock_.load(std::memory_order_acquire) != sock) {
        return -1;  // closed while waiting
    }

#ifdef _WIN32
    const int rc = ::recv(sock, reinterpret_cast<char*>(buf),
                          static_cast<int>(size), 0);
    if (rc == SOCKET_ERROR) {
        const int e = ::WSAGetLastError();
        if (e == WSAEINTR) return 0;
        return -1;
    }
#else
    const ssize_t rc = ::recv(sock, reinterpret_cast<char*>(buf), size, 0);
    if (rc < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }
#endif
    if (rc == 0) {
        // Orderly close by the peer (EOF).  This path never half-closes,
        // so EOF means the connection is dead.
        return -1;
    }
    return static_cast<std::ptrdiff_t>(rc);
}

void TcpSocket::close() noexcept {
    // Take the handle out of the atomic first, then retire it.  The
    // physical close is deferred to the destructor (discipline 1) — a
    // peer thread may be inside a bounded wait on the same handle.
    const socket_t sock = sock_.exchange(kInvalid, std::memory_order_acq_rel);
    if (sock == kInvalid) return;

    // shutdown() wakes a thread blocked in a bounded wait with an error
    // instead of leaving it parked until the poll timeout.
#ifdef _WIN32
    ::shutdown(sock, SD_BOTH);
#else
    ::shutdown(sock, SHUT_RDWR);
#endif

    {
        std::lock_guard<std::mutex> lk(retired_mu_);
        retired_.push_back(sock);
    }
}

TcpSocket::~TcpSocket() noexcept {
    close();

    std::vector<socket_t> to_close;
    {
        std::lock_guard<std::mutex> lk(retired_mu_);
        to_close.swap(retired_);
    }
    for (const socket_t sock : to_close) {
#ifdef _WIN32
        ::closesocket(sock);
#else
        ::close(sock);
#endif
    }
}

std::uint16_t TcpSocket::local_port() const noexcept {
    const socket_t sock = sock_.load(std::memory_order_acquire);
    if (sock == kInvalid) return 0;
    sockaddr_in addr{};
    socklen_t_ addr_len = sizeof(addr);
#ifdef _WIN32
    if (::getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &addr_len) < 0) {
        return 0;
    }
#else
    if (::getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &addr_len) < 0) {
        return 0;
    }
#endif
    return ntohs(addr.sin_port);
}

std::string TcpSocket::local_endpoint() const noexcept {
    const socket_t sock = sock_.load(std::memory_order_acquire);
    if (sock == kInvalid) return {};
    sockaddr_in addr{};
    socklen_t_ addr_len = sizeof(addr);
#ifdef _WIN32
    if (::getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &addr_len) < 0) {
        return {};
    }
#else
    if (::getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &addr_len) < 0) {
        return {};
    }
#endif
    if (ntohl(addr.sin_addr.s_addr) == 0u) {
        const std::string routable = find_routable_local_ipv4();
        if (::inet_pton(AF_INET, routable.c_str(), &addr.sin_addr) != 1) {
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        }
    }
    return format_endpoint_v4(addr);
}

std::string TcpSocket::peer_endpoint() const noexcept {
    const socket_t sock = sock_.load(std::memory_order_acquire);
    if (sock == kInvalid) return {};
    sockaddr_in addr{};
    socklen_t_ addr_len = sizeof(addr);
#ifdef _WIN32
    if (::getpeername(sock, reinterpret_cast<sockaddr*>(&addr), &addr_len) < 0) {
        return {};
    }
#else
    if (::getpeername(sock, reinterpret_cast<sockaddr*>(&addr), &addr_len) < 0) {
        return {};
    }
#endif
    return format_endpoint_v4(addr);
}

bool TcpSocket::is_open() const noexcept {
    return sock_.load(std::memory_order_acquire) != kInvalid;
}

}  // namespace nimrtc::ice
