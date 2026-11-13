/**
 * @file nimrtc/raw_udp/udp_socket.hpp
 * @brief Tiny cross-platform UDP socket wrapper for the ArqRawUdp layer.
 *
 * Slice 6 follow-up — the previous "skeleton" only had an in-process
 * mutex/queue path (driven by `arq_test_inject_inbound`). To make the
 * raw-UDP bypass actually usable for real data transfer, this header
 * exposes a thin RAII socket around the underlying POSIX / Winsock
 * primitives that ArqRawUdp drives from its recv thread + send path.
 *
 * Design constraints:
 *  - No IPv6 / dual-stack yet — IPv4 only (matches Slice 6 scope).
 *  - No async I/O (overlapped I/O on Windows / epoll on Linux). The
 *    recv path runs on a dedicated background thread that blocks on
 *    `recv_from()`.  Good enough for 50–200 Hz control traffic.
 *  - No socket options tuning (SO_RCVBUF / SO_SNDBUF / QoS) — the
 *    caller can wrap this if needed; Slice 6 wants the *minimum
 *    surface* that proves the seam is real.
 *  - `Endpoint` here means `plugins::Endpoint` — same opaque address
 *    encoding ArqRawUdp uses.  The current encoding is "ip:port" in
 *    ASCII (see ArqRawUdp::encode_endpoint); this wrapper round-trips
 *    that format with sockaddr_in.
 *
 * @note P2 follow-up (Slice 6.5) — once Slice 7's stack integrates
 *       with ICE's selected-pair, this layer should be replaced with a
 *       thin call into IICETransport::send_to() / recv_from() so we
 *       don't keep a second socket open.  Until then this is the path
 *       that proves the ARQ state machine wires up end-to-end.
 */
#pragma once


// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

#include <nimrtc/plugins/base.hpp>

namespace nimrtc::raw_udp {

/**
 * @brief Tiny RAII UDP socket (IPv4 only, blocking I/O).
 *
 * Threading: each instance is intended to be driven by exactly one
 * recv thread + one or more send threads (send_to() is thread-safe
 * by virtue of UDP being message-atomic at the kernel level).
 */
class NIMRTC_API_STABLE UdpSocket {
public:
    UdpSocket() noexcept = default;
    ~UdpSocket() noexcept;

    UdpSocket(const UdpSocket&)            = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    /**
     * @brief Bind to host:port (host = "" or "0.0.0.0" = any).
     * @param port 0 = let OS pick (typical for client side; query
     *             `local_port()` after open() to discover it).
     * @return true on success; false on any error (errno / WSAGetLastError
     *         logged but not retained).
     */
    bool open(const std::string& host, std::uint16_t port) noexcept;

    /**
     * @brief Release the socket.  Idempotent.
     *
     * The handle is published as invalid immediately, so no new
     * send/recv starts on it.  The *physical* close (the ::close()
     * syscall) is deferred to the destructor: closing an fd while a
     * peer thread is inside recvfrom()/sendto() on it is a data race
     * on the kernel's descriptor state.
     */
    void close() noexcept;

    /**
     * @brief Send `bytes` to `dst`.  Thread-safe (UDP is atomic at the
     * kernel layer for datagrams ≤ MTU).
     *
     * `dst` is the canonical plugin-layer address (plugins::Addr ==
     * plugins::Endpoint).  We accept Addr here directly so this header
     * has no upstream dependency on the slice-6 alias.
     * @return true on success, false on error.
     */
    bool send_to(plugins::Addr dst,
                 std::span<const std::uint8_t> bytes) noexcept;

    /**
     * @brief Blocking recvfrom into `buf`.  Sets `from` on success.
     * @return number of bytes received, -1 on error / after close(),
     *         or 0 when the wait timed out without a datagram arriving.
     *
     * @note The wait is bounded (see `kRecvPollTimeoutMs`): the socket
     *       is polled before every recvfrom, so a caller that wants a
     *       periodic wake-up (to check a stop flag) gets one at least
     *       every timeout period.  Returning 0 on timeout is what lets
     *       `ArqRawUdp::recv_main()` notice its stop flag without the
     *       socket ever having to be closed out from under it.
     */
    std::ptrdiff_t recv_from(void* buf, std::size_t buf_size,
                              plugins::Addr* src_addr_out) noexcept;

    /** Local port after open(); 0 before open() / on error. */
    std::uint16_t local_port() const noexcept;

    /** True iff open() succeeded and the socket is usable. */
    bool is_open() const noexcept;

    /**
     * @brief Convert a sockaddr_in (host byte order) to plugins::Addr.
     * Used internally; exposed for tests that want to forge endpoints.
     */
    static plugins::Addr make_endpoint_v4(const std::string& host,
                                           std::uint16_t port) noexcept;

private:
#if defined(_WIN32)
    // SOCKET is a UINT_PTR (unsigned __int3264).  uintptr_t is the
    // closest standard-portable type.  ~0ull == INVALID_SOCKET.
    using socket_t = std::uintptr_t;
    static constexpr socket_t kInvalid = static_cast<socket_t>(~0ull);
#else
    using socket_t = int;
    static constexpr socket_t kInvalid = -1;
#endif
    // How long recv_from() waits for a datagram before it gives the
    // caller a chance to observe a stop flag.  See recv_from().
    static constexpr int kRecvPollTimeoutMs = 50;

    /** Wait until the socket is readable, or `kRecvPollTimeoutMs`
     *  elapses.  Returns true when a datagram is pending. */
    bool wait_readable(socket_t sock) const noexcept;

    // The socket handle.
    //
    // It is atomic because close() runs on the owner thread while a
    // recv thread may be blocked in recv_from() on the same handle:
    // close() has to publish "the handle is gone" so the blocked
    // recv_from() notices.  A plain `socket_t` was a data race
    // (TSan: read of size 4 in UdpSocket::recv_from() vs. write in
    // UdpSocket::close()).
    std::atomic<socket_t> sock_{kInvalid};

    // Handles whose *logical* close has happened but whose *physical*
    // close (the ::close() syscall) is deferred to the destructor.
    //
    // The physical close must not happen while another thread may still
    // be inside a syscall on that fd: closing an fd that a peer thread
    // is inside recvfrom()/sendto() on is a data race on the kernel's
    // file-descriptor state (TSan: "data race ... in UdpSocket::close()"
    // against the fd read by recvfrom()), and the fd number can be
    // recycled while the kernel is still using it.
    //
    // The destructor is the safe place for the physical close: by then
    // every user of the socket holds (or has released) a shared_ptr to
    // this object, so no thread can be inside a syscall on the fd.
    mutable std::mutex            retired_mu_;
    std::vector<socket_t>         retired_;

#if defined(_WIN32)
    bool winsock_initialized_ = false;
#endif
};

} // namespace nimrtc::raw_udp
