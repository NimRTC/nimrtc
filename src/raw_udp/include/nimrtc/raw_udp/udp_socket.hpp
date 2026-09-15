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

#include <cstddef>
#include <cstdint>
#include <span>

#include <nimrtc/plugins/base.hpp>

namespace nimrtc::raw_udp {

/**
 * @brief Tiny RAII UDP socket (IPv4 only, blocking I/O).
 *
 * Threading: each instance is intended to be driven by exactly one
 * recv thread + one or more send threads (send_to() is thread-safe
 * by virtue of UDP being message-atomic at the kernel level).
 */
class UdpSocket {
public:
    UdpSocket() noexcept = default;
    ~UdpSocket() noexcept { close(); }

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
     * @return number of bytes received, or -1 on error / after close().
     *
     * @note This blocks forever on a healthy socket.  Use a non-blocking
     *       variant (added in Slice 7.5) if the caller wants a poll loop.
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
    socket_t sock_ = kInvalid;

#if defined(_WIN32)
    bool winsock_initialized_ = false;
#endif
};

} // namespace nimrtc::raw_udp
