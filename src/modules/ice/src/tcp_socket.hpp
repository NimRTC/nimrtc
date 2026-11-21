// tcp_socket.hpp — a tiny cross-platform TCP socket for the ICE-over-TCP
// path (RFC 6544).
//
// Modelled on src/modules/raw_udp/include/nimrtc/raw_udp/udp_socket.hpp
// (ADR-028, Decision 1): the same two fd-lifetime disciplines are copied
// verbatim, because they are the lessons of real data races:
//
//   1. Retire, don't close.  close() takes the handle out of the atomic,
//      marks it invalid and defers the physical ::close() to the
//      destructor.  Closing an fd while a peer thread is inside a syscall
//      on it is a data race on the kernel's descriptor state.
//
//   2. Poll before syscall, with a bounded timeout.  recv/accept/connect/
//      send all wait on a bounded poll first, so a caller gets a
//      wake-up at least every kRecvPollTimeoutMs to observe its stop
//      flag without the socket ever being closed out from under it.
//
// IPv4 only and blocking-but-bounded I/O, matching the UdpSocket shape.
// The socket is non-blocking underneath; every syscall is preceded by a
// bounded poll, so the "blocking" the caller sees is never unbounded.

#ifndef NIMRTC_ICE_TCP_SOCKET_HPP
#define NIMRTC_ICE_TCP_SOCKET_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nimrtc::ice {

/// A TCP socket (SOCK_STREAM, IPv4) with a bounded-wait I/O surface.
///
/// Roles:
///   - passive: open() + bind() + listen() + accept() → connected stream
///   - active:  open() + connect()
///   - either:  send_all() / recv_some() on the connected stream
///
/// Threading: intended to be driven by exactly one I/O thread.  close()
/// is safe from another thread (the retire-not-close discipline).
class TcpSocket {
public:
    TcpSocket() noexcept = default;
    ~TcpSocket() noexcept;

    TcpSocket(const TcpSocket&)            = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    /// Create the socket.  Idempotent; returns true when already open.
    bool open() noexcept;

    /// Bind to host:port (host "" or "0.0.0.0" = any).  Call before
    /// listen() for a passive candidate.  port 0 = OS-assigned; query
    /// local_port() after binding.
    bool bind(const std::string& host, std::uint16_t port) noexcept;

    /// Start listening (passive role).  Returns true on success.
    bool listen(int backlog = 16) noexcept;

    /// Wait up to `timeout_ms` for an inbound connection and accept it.
    /// Returns the connected stream, or std::nullopt on timeout / error.
    std::optional<TcpSocket> accept(int timeout_ms) noexcept;

    /// Connect to host:port (active role).  Bounded by `timeout_ms`; the
    /// caller sees the stop flag at least every kRecvPollTimeoutMs.
    bool connect(const std::string& host, std::uint16_t port,
                 int timeout_ms) noexcept;

    /// Write the whole span.  Returns true when every byte was written,
    /// false on error or when `timeout_ms` elapsed mid-write.
    bool send_all(std::span<const std::uint8_t> bytes, int timeout_ms) noexcept;

    /// Receive up to `size` bytes.  Returns > 0 bytes received, 0 on a
    /// bounded-wait timeout (nothing pending), -1 on error / orderly
    /// close by the peer / close().
    std::ptrdiff_t recv_some(void* buf, std::size_t size) noexcept;

    /// Release the socket.  Idempotent.  The handle is published as
    /// invalid immediately; the physical close runs in the destructor.
    void close() noexcept;

    /// Local port (0 before bind / on error).
    std::uint16_t local_port() const noexcept;

    /// The socket's own bound address as "ip:port".  A wildcard bind is
    /// resolved to a concrete routable IPv4 address, like UdpSocket, so
    /// the advertised candidate is a destination a peer can connect to.
    std::string local_endpoint() const noexcept;

    /// The connected peer's address as "ip:port".  Empty on a socket that
    /// is not connected.  Used by the ICE driver to match an accepted
    /// connection's source to a remote candidate (RFC 6544 §4.2.1).
    std::string peer_endpoint() const noexcept;

    /// True iff open() succeeded and the socket is usable.
    bool is_open() const noexcept;

private:
#if defined(_WIN32)
    using socket_t = std::uintptr_t;
    static constexpr socket_t kInvalid = static_cast<socket_t>(~0ull);
#else
    using socket_t = int;
    static constexpr socket_t kInvalid = -1;
#endif
    /// How long a bounded wait lasts before the caller gets to check a
    /// stop flag (same value as UdpSocket::kRecvPollTimeoutMs).
    static constexpr int kRecvPollTimeoutMs = 50;

    /// Wait until `sock` is readable, or `timeout_ms` elapses.
    bool wait_readable(socket_t sock, int timeout_ms) const noexcept;
    /// Wait until `sock` is writable, or `timeout_ms` elapses.
    bool wait_writable(socket_t sock, int timeout_ms) const noexcept;

    /// The socket handle.  Atomic so close() can publish "gone" to a
    /// peer thread blocked in a bounded wait on the same handle.
    std::atomic<socket_t> sock_{kInvalid};

    /// Handles whose logical close has happened but whose physical close
    /// is deferred to the destructor (see class comment, discipline 1).
    mutable std::mutex    retired_mu_;
    std::vector<socket_t> retired_;

#if defined(_WIN32)
    bool winsock_initialized_ = false;
#endif
};

}  // namespace nimrtc::ice

#endif  // NIMRTC_ICE_TCP_SOCKET_HPP
