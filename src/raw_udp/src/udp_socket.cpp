// SPDX-License-Identifier: MIT
//
// @file src/raw_udp/src/udp_socket.cpp
// @brief Cross-platform UDP socket impl for the ArqRawUdp layer.

#include <nimrtc/raw_udp/udp_socket.hpp>

#include <nimrtc/core/log.hpp>

#include <algorithm>
#include <cstring>
#include <span>
#include <string>

#ifdef _WIN32
// Include winsock BEFORE windows.h to avoid the winsock1/v2 conflict
// trap.  Winsock2.h pulls in windows.h on its own.
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
// ws2_32.lib is linked via pragma here so a consumer that only links
// nimrtc_raw_udp (rare) does not have to remember it.  MSVC respects
// this even in pure-C++ compilation; GCC/Clang on Windows ignore it.
#  pragma comment(lib, "ws2_32.lib")
using socklen_t_ = int;
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <netdb.h>      // addrinfo, getaddrinfo, freeaddrinfo (POSIX)
#  include <sys/types.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <cerrno>
using socklen_t_ = socklen_t;
#endif

namespace nimrtc::raw_udp {

namespace {

// Parse a "host:port" endpoint (or just "host") into sockaddr_in.
// Returns true on success.  Empty / malformed inputs default to INADDR_ANY:0.
bool parse_endpoint_v4(const std::string& s, sockaddr_in* out) {
    if (!out) return false;
    std::memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_addr.s_addr = htonl(INADDR_ANY);
    out->sin_port = 0;

    if (s.empty()) return true;

    // Split at the LAST ':' so IPv6-style "::1" doesn't trip us, even
    // though we don't support v6 — this is a forward-compat nicety.
    auto colon = s.rfind(':');
    std::string host = (colon == std::string::npos) ? s : s.substr(0, colon);
    std::string port = (colon == std::string::npos) ? std::string{}
                                                     : s.substr(colon + 1);

    if (!host.empty()) {
        // Try inet_pton first (handles "127.0.0.1", "192.168.1.1", etc).
        // Fall back to getaddrinfo for "localhost" and DNS names.
        if (::inet_pton(AF_INET, host.c_str(), &out->sin_addr) != 1) {
#ifdef _WIN32
            // getaddrinfo on Windows wants a wsa-aware setup; we already
            // have WSAStartup done at open() so it's safe.
            addrinfo hints{};
            hints.ai_family = AF_INET;
            addrinfo* res = nullptr;
            if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0
                || !res) {
                return false;
            }
            std::memcpy(&out->sin_addr,
                        &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr,
                        sizeof(out->sin_addr));
            ::freeaddrinfo(res);
#else
            addrinfo hints{};
            hints.ai_family = AF_INET;
            addrinfo* res = nullptr;
            if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0
                || !res) {
                return false;
            }
            std::memcpy(&out->sin_addr,
                        &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr,
                        sizeof(out->sin_addr));
            ::freeaddrinfo(res);
#endif
        }
    }
    if (!port.empty()) {
        long p = std::strtol(port.c_str(), nullptr, 10);
        if (p <= 0 || p > 65535) return false;
        out->sin_port = htons(static_cast<std::uint16_t>(p));
    }
    return true;
}

// Format sockaddr_in back into "ip:port" string (max ~22 bytes).
std::string format_endpoint_v4(const sockaddr_in& sa) {
    char ip[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof(ip));
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s:%u", ip, ntohs(sa.sin_port));
    return std::string(buf);
}

} // namespace

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : sock_(other.sock_)
#if defined(_WIN32)
    , winsock_initialized_(other.winsock_initialized_)
#endif
{
    other.sock_ = kInvalid;
#if defined(_WIN32)
    other.winsock_initialized_ = false;
#endif
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        close();
        sock_ = other.sock_;
#if defined(_WIN32)
        winsock_initialized_ = other.winsock_initialized_;
#endif
        other.sock_ = kInvalid;
#if defined(_WIN32)
        other.winsock_initialized_ = false;
#endif
    }
    return *this;
}

bool UdpSocket::open(const std::string& host, std::uint16_t port) noexcept {
    if (sock_ != kInvalid) {
        // Already open; treat as success (idempotent).
        return true;
    }

#ifdef _WIN32
    if (!winsock_initialized_) {
        WSADATA wsa{};
        int rc = ::WSAStartup(MAKEWORD(2, 2), &wsa);
        if (rc != 0) {
            NIMRTC_LOG_ERROR("raw_udp: WSAStartup failed rc=" << rc);
            return false;
        }
        winsock_initialized_ = true;
    }
#endif

    socket_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
#ifdef _WIN32
    if (s == kInvalid) {
        NIMRTC_LOG_ERROR("raw_udp: socket() failed err="
                          << ::WSAGetLastError());
        return false;
    }
#else
    if (s < 0) {
        NIMRTC_LOG_ERROR("raw_udp: socket() failed errno=" << errno);
        return false;
    }
#endif

    sockaddr_in addr{};
    if (!parse_endpoint_v4(host, &addr)) {
        NIMRTC_LOG_ERROR("raw_udp: invalid host='" << host << "'");
#ifdef _WIN32
        ::closesocket(static_cast<SOCKET>(s));
#else
        ::close(s);
#endif
        return false;
    }
    addr.sin_port = htons(port);

    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
#ifdef _WIN32
        int e = ::WSAGetLastError();
        ::closesocket(static_cast<SOCKET>(s));
        NIMRTC_LOG_ERROR("raw_udp: bind(" << host << ":" << port
                          << ") failed err=" << e);
#else
        int e = errno;
        ::close(s);
        NIMRTC_LOG_ERROR("raw_udp: bind(" << host << ":" << port
                          << ") failed errno=" << e);
#endif
        return false;
    }

    sock_ = s;
    NIMRTC_LOG_INFO("raw_udp: UDP socket bound to "
                     << host << ":" << local_port());
    return true;
}

void UdpSocket::close() noexcept {
    if (sock_ == kInvalid) return;
#ifdef _WIN32
    ::closesocket(static_cast<SOCKET>(sock_));
    // Intentionally do NOT call WSACleanup() — winsock is process-wide
    // refcounted and other ArqRawUdp instances / future sockets in the
    // same process still need it.  WSACleanup at process exit is
    // automatic.
#else
    ::close(sock_);
#endif
    sock_ = kInvalid;
}

bool UdpSocket::send_to(plugins::Addr dst,
                        std::span<const std::uint8_t> bytes) noexcept {
    if (sock_ == kInvalid) return false;
    sockaddr_in addr{};
    // Endpoint format is "ip:port" ASCII; convert to sockaddr_in via
    // parse_endpoint_v4 (which understands empty / null cases).
    std::string s(reinterpret_cast<const char*>(dst.data),
                  std::min<std::size_t>(dst.len, sizeof(dst.data)));
    if (!parse_endpoint_v4(s, &addr)) {
        NIMRTC_LOG_ERROR("raw_udp: send_to invalid dst='" << s << "'");
        return false;
    }
    auto* buf = reinterpret_cast<const char*>(bytes.data());
#ifdef _WIN32
    int rc = ::sendto(static_cast<SOCKET>(sock_), buf,
                      static_cast<int>(bytes.size()), 0,
                      reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc < 0) {
        NIMRTC_LOG_ERROR("raw_udp: sendto failed err=" << ::WSAGetLastError());
        return false;
    }
#else
    ssize_t rc = ::sendto(sock_, buf, bytes.size(), 0,
                          reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc < 0) {
        NIMRTC_LOG_ERROR("raw_udp: sendto failed errno=" << errno);
        return false;
    }
#endif
    return true;
}

std::ptrdiff_t UdpSocket::recv_from(void* buf, std::size_t buf_size,
                                     plugins::Addr* src_addr_out) noexcept {
    if (sock_ == kInvalid) return -1;
    sockaddr_in addr{};
    socklen_t_ addr_len = sizeof(addr);
#ifdef _WIN32
    int rc = ::recvfrom(static_cast<SOCKET>(sock_),
                        reinterpret_cast<char*>(buf),
                        static_cast<int>(buf_size), 0,
                        reinterpret_cast<sockaddr*>(&addr), &addr_len);
    if (rc == SOCKET_ERROR) {
        int e = ::WSAGetLastError();
        // WSAEINTR (cancelled) and WSAECONNRESET (port-unreachable ICMP
        // echo on Windows) are recoverable — return 0 and let caller
        // loop.  Other errors are fatal.
        if (e == WSAEINTR || e == WSAECONNRESET) {
            return 0;
        }
        NIMRTC_LOG_ERROR("raw_udp: recvfrom failed err=" << e);
        return -1;
    }
#else
    ssize_t rc = ::recvfrom(sock_, buf, buf_size, 0,
                            reinterpret_cast<sockaddr*>(&addr), &addr_len);
    if (rc < 0) {
        if (errno == EINTR) return 0;
        NIMRTC_LOG_ERROR("raw_udp: recvfrom failed errno=" << errno);
        return -1;
    }
#endif
    if (src_addr_out) {
        std::string s = format_endpoint_v4(addr);
        const std::size_t n = (s.size() < sizeof(src_addr_out->data))
                                 ? s.size() : sizeof(src_addr_out->data);
        std::memcpy(src_addr_out->data, s.data(), n);
        src_addr_out->len = static_cast<std::uint32_t>(n);
    }
    return static_cast<std::ptrdiff_t>(rc);
}

std::uint16_t UdpSocket::local_port() const noexcept {
    if (sock_ == kInvalid) return 0;
    sockaddr_in addr{};
    socklen_t_ addr_len = sizeof(addr);
#ifdef _WIN32
    if (::getsockname(static_cast<SOCKET>(sock_),
                      reinterpret_cast<sockaddr*>(&addr), &addr_len) < 0) {
        return 0;
    }
#else
    if (::getsockname(sock_,
                      reinterpret_cast<sockaddr*>(&addr), &addr_len) < 0) {
        return 0;
    }
#endif
    return ntohs(addr.sin_port);
}

bool UdpSocket::is_open() const noexcept { return sock_ != kInvalid; }

plugins::Addr UdpSocket::make_endpoint_v4(const std::string& host,
                                               std::uint16_t port) noexcept {
    plugins::Addr ep{};
    sockaddr_in addr{};
    if (!parse_endpoint_v4(host, &addr)) return ep;
    addr.sin_port = htons(port);
    std::string s = format_endpoint_v4(addr);
    const std::size_t n = (s.size() < sizeof(ep.data))
                              ? s.size() : sizeof(ep.data);
    std::memcpy(ep.data, s.data(), n);
    ep.len = static_cast<std::uint32_t>(n);
    return ep;
}

} // namespace nimrtc::raw_udp
