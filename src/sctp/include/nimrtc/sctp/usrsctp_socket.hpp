/**
 * @file nimrtc/sctp/usrsctp_socket.hpp
 * @brief UsrsctpSocket — production implementation of ISctpSocket
 *        backed by upstream usrsctp 0.9.5.0 (Transport PAL Slice 5 /
 *        v0.11.0).
 *
 * TPAL-5 Stage 2 (this revision): adds the SCTP association
 * handshake surface (`listen()` / `connect()`) and a state machine
 * that tracks SCTP_COMM_UP / SCTP_COMM_LOST notifications so
 * `send_*` after `connect()` actually delivers and `set_on_recv`
 * fires on the receiver.
 *
 * ## Backing transport
 *
 * usrsctp 0.9.5.0 in userspace mode delivers AF_INET packets via
 * a single process-wide UDP socket whose fd is stored in
 * `SCTP_BASE_VAR(userspace_udpsctp)` (see `sctp_userspace_ip_output`
 * in upstream `user_socket.c`). For NimRTC's loopback test, two
 * `UsrsctpSocket` instances on `127.0.0.1` route packets through
 * the shared UDP listener opened lazily by the first instance
 * (`s_acquire_shared_udp()` in the .cpp). SCTP_REMOTE_UDP_ENCAPS_PORT
 * (RFC 6951) is wired to the shared listener port so usrsctp
 * knows where to send outbound SCTP-over-UDP datagrams.
 *
 * No external UDP socket pair is required.
 *
 * ## Thread model
 *
 * - `send_*` / `set_on_recv` / `listen` / `connect` are called from
 *   a single thread (the engine thread) per the IDataChannel contract.
 * - usrsctp fires the receive callback on its own internal worker
 *   thread. The recv callback routes data payloads to `on_recv_`
 *   under `recv_mu_` and routes SCTP_ASSOC_CHANGE notifications to
 *   `handle_notification()` which mutates `state_` under `state_mu_`.
 *   The test layer is expected to push payloads into a thread-safe
 *   queue.
 *
 * ## Process-wide lifetime
 *
 * `usrsctp_init()` is called once per process (guarded by an atomic
 * once-flag in `UsrsctpSocket::s_acquire_global_init()`). We deliberately
 * do NOT call `usrsctp_finish()` when the last socket is destroyed — doing
 * so would make a subsequent `usrsctp_init()` call (in a later test or
 * reconnect) return `ENOBUFS` on Windows because the buffer-pool
 * allocator's free-list isn't fully rebuilt.
 *
 * The shared UDP listener is similarly process-wide — its fd lives inside
 * usrsctp's `userspace_udpsctp` global and persists across all
 * `UsrsctpSocket` lifetimes.
 *
 * @note P0 scaffold — interface stable; binary layout TBD P4.
 */

#ifndef NIMRTC_SCTP_USRSCTP_SOCKET_HPP
#define NIMRTC_SCTP_USRSCTP_SOCKET_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <nimrtc/sctp/sctp_socket_factory.hpp>   // SctpConfig
#include <nimrtc/sctp/sctp_socket_iface.hpp>      // ISctpSocket, plugins::*

// Forward-declare the usrsctp types that the static method signatures
// reference. The full definitions live in usrsctp's user_socketvar.h —
// we don't pull that into this public header to keep the include
// surface narrow. The .cpp includes usrsctp.h (which transitively
// pulls in user_socketvar.h) before defining the methods.
//
// IMPORTANT: these declarations are in the GLOBAL namespace to match
// where usrsctp.h declares them (not `nimrtc::sctp` — the compiler
// would otherwise see them as `nimrtc::sctp::socket` etc. and reject
// usrsctp_socket()'s argument types).
struct socket;
union  sctp_sockstore;
struct sctp_rcvinfo;

namespace nimrtc::sctp {

// ---------------------------------------------------------------------------
// UsrsctpSocketState — lifecycle state of one SCTP association.
// ---------------------------------------------------------------------------
//
// TPAL-5 Stage 2: tracks the listen/connect handshake so callers can
// wait on SCTP_COMM_UP without polling. The transitions are:
//
//   kUnbound
//     │  listen(local_port)
//     ▼
//   kListening                     (usrsctp_listen succeeded)
//     │  peer connect arrives + SCTP_COMM_UP
//     ▼
//   kEstablished
//     │  SCTP_COMM_LOST / abort / shutdown
//     ▼
//   kClosed
//
//   kUnbound
//     │  connect(remote_addr, remote_port)
//     ▼
//   kConnecting                    (usrsctp_connect returned)
//     │  SCTP_COMM_UP
//     ▼
//   kEstablished
//     │  SCTP_COMM_LOST / abort / shutdown
//     ▼
//   kClosed
//
// Guarded by `state_mu_` and exposed to waiters via `state_cv_`.
// ---------------------------------------------------------------------------
enum class UsrsctpSocketState : std::uint8_t {
    kUnbound     = 0,
    kListening   = 1,
    kConnecting  = 2,
    kEstablished = 3,
    kClosed      = 4,
};

/** Convert state to a human-readable label (for logs). */
inline const char* to_string(UsrsctpSocketState s) noexcept {
    switch (s) {
        case UsrsctpSocketState::kUnbound:     return "kUnbound";
        case UsrsctpSocketState::kListening:   return "kListening";
        case UsrsctpSocketState::kConnecting:  return "kConnecting";
        case UsrsctpSocketState::kEstablished: return "kEstablished";
        case UsrsctpSocketState::kClosed:      return "kClosed";
    }
    return "?";
}

class UsrsctpSocket final : public ISctpSocket {
public:
    explicit UsrsctpSocket(const SctpConfig& cfg);
    ~UsrsctpSocket() override;

    UsrsctpSocket(const UsrsctpSocket&)            = delete;
    UsrsctpSocket& operator=(const UsrsctpSocket&) = delete;

    // ---- ISctpSocket -----------------------------------------------------

    plugins::Status send_datagram(std::uint16_t stream,
                                  plugins::BufferView data) noexcept override;

    plugins::Status send_stream(std::uint16_t stream,
                                plugins::BufferView data) noexcept override;

    plugins::Status send_partial_reliable(
        std::uint16_t stream,
        plugins::BufferView data,
        std::chrono::milliseconds ttl) noexcept override;

    void set_on_recv(plugins::OnSctpRecvCb cb) noexcept override;

    // ---- Stage 2: association handshake API -----------------------------
    //
    // These methods are concrete UsrsctpSocket operations (NOT part of
    // the ISctpSocket seam). The seam per
    // `docs/plan/transport-selection.md` §5.2 doesn't model the
    // listen/connect ceremony because the stub backend (id="stub")
    // doesn't need it. Production callers (engine, ICE-bound
    // DataChannel setup) drive the handshake through these methods
    // after constructing the socket from the factory.

    /** Mark this socket as a passive (listening) endpoint bound to
     *  127.0.0.1:local_port. Calls usrsctp_listen with backlog=1.
     *  local_port=0 lets usrsctp pick an ephemeral SCTP port.
     *  @return kOk on success; kErrInvalidParam if state != kUnbound;
     *          kErrInternal if usrsctp_listen fails. */
    plugins::Status listen(std::uint16_t local_port) noexcept;

    /** Connect this socket to the peer at `remote_addr:remote_port`
     *  (IPv4 dotted-quad). Currently only 127.0.0.1 is exercised by
     *  the Stage-2 tests; the implementation routes the SCTP packet
     *  through the per-process shared UDP listener
     *  (`g_usrsctp_udp_*`) so any IP in 127.0.0.0/8 should "just
     *  work". Calls usrsctp_connect after wiring
     *  SCTP_REMOTE_UDP_ENCAPS_PORT to the shared UDP listener port.
     *  @return kOk if connect was issued; kErrInvalidParam for bad
     *          address / wrong state; kErrInternal for connect failure. */
    plugins::Status connect(std::string_view remote_addr,
                            std::uint16_t remote_port) noexcept;

    /** Block until state transitions to kEstablished, until `timeout`
     *  elapses, or until the socket transitions to kClosed.
     *  @return true on kEstablished; false on timeout or kClosed. */
    bool wait_for_established(std::chrono::milliseconds timeout) noexcept;

    // ---- Test helpers (public so the smoke test can probe state) -------

    /** Returns true if the socket was successfully created and bound. */
    bool is_ready() const noexcept { return sock_ != nullptr; }

    /** Returns the local SCTP port usrsctp bound to (0 if not bound). */
    std::uint16_t local_port() const noexcept;

    /** Current state (atomic snapshot — guarded by state_mu_). */
    UsrsctpSocketState state() const noexcept;

private:
    /** Per-instance receive callback (usrsctp's worker thread fires it).
     *  The recv-dispatch trampoline holds `recv_mu_` while calling it. */
    plugins::OnSctpRecvCb on_recv_;
    /** Mutex guarding `on_recv_` access from the usrsctp worker thread
     *  and the engine thread (set_on_recv). */
    mutable std::mutex recv_mu_;

    /** State machine: guarded by `state_mu_`. Transitions are driven
     *  by the recv trampoline (which fires on usrsctp's worker
     *  thread when an SCTP_ASSOC_CHANGE notification arrives) and
     *  by listen()/connect() on the engine thread. */
    UsrsctpSocketState state_ = UsrsctpSocketState::kUnbound;
    mutable std::mutex state_mu_;
    std::condition_variable state_cv_;

    /** Opaque usrsctp socket handle (forward-declared to avoid leaking
     *  the C `struct socket` into this header). */
    void* sock_;   // `struct socket*` in the .cpp

    /** Configuration we were built from. */
    SctpConfig cfg_;

    // ---- usrsctp receive callback trampoline ----------------------------

    /** Per-process packet-output trampoline (usrsctp_init() second
     *  arg, also called `conn_output`). For our AF_INET loopback the
     *  actual delivery is done by `sctp_userspace_ip_output` via
     *  `SCTP_BASE_VAR(userspace_udpsctp)`, so this callback is a
     *  no-op stub. We keep it as a real function pointer (not NULL)
     *  because usrsctp's `usrsctp_socket()` asserts
     *  `SCTP_BASE_VAR(conn_output) != NULL` even for AF_INET (see
     *  user_socket.c:1342). */
    static int s_packet_output(void* addr,
                               void* buffer,
                               std::size_t length,
                               std::uint8_t tos,
                               std::uint8_t set_df);

    /** Per-instance usrsctp-socket receive callback. The signature is
     *  fixed by usrsctp.h's `receive_cb` typedef (matches the type
     *  passed to `usrsctp_socket()`):
     *
     *    int (*)(struct socket*, union sctp_sockstore, void*,
     *            size_t, struct sctp_rcvinfo, int, void*);
     *
     *  We extract `this` from `ulp_info` and forward to the user's
     *  `on_recv_` under `recv_mu_`. SCTP notifications
     *  (SCTP_ASSOC_CHANGE etc.) arrive through the same callback
     *  with `(flags & MSG_NOTIFICATION)` set; we route them to
     *  `handle_notification()` BEFORE calling `on_recv_` so the
     *  user callback only sees DATA payloads.
     *
     *  Note: `union sctp_sockstore` and `struct sctp_rcvinfo` are
     *  passed BY VALUE (not as pointers). The struct sizes are larger
     *  than `void*`, so a wrapper with a `(void*, ..., void*)`
     *  signature has an incompatible ABI — see usrsctp_socket.cpp's
     *  constructor for the rationale. */
    static int s_instance_recv_cb(struct socket* sock,
                                  union sctp_sockstore addr,
                                  void* data,
                                  std::size_t datalen,
                                  struct sctp_rcvinfo rcvinfo,
                                  int flags,
                                  void* ulp_info);

    /** Per-instance send_cb — usrsctp fires this when its send buffer
     *  has free space. We don't use it; the no-op stub satisfies
     *  usrsctp_socket()'s null-check. */
    static int s_instance_send_cb(struct socket* sock,
                                  std::uint32_t sb_free,
                                  void* ulp_info);

    // ---- Process-wide usrsctp_init guard --------------------------------
    // usrsctp_init() is called ONCE per process (guarded by an atomic
    // once-flag). We deliberately do NOT call usrsctp_finish() even
    // when the last socket is destroyed — see Process-wide lifetime
    // note in the file header.

    /** Acquire the global usrsctp library (calls usrsctp_init on first
     *  call only). Thread-safe. */
    static void s_acquire_global_init() noexcept;

    /** Release the global usrsctp reference (no-op — usrsctp stays
     *  initialized for process lifetime). Thread-safe. */
    static void s_release_global_init() noexcept;

    // ---- Per-process shared UDP listener (RFC 6951 encapsulation) ------
    //
    // Stage 2: usrsctp in userspace mode delivers AF_INET packets via
    // a single process-wide UDP socket whose fd is stored in
    // `SCTP_BASE_VAR(userspace_udpsctp)`. The socket is created and
    // owned by usrsctp internals when usrsctp_init() is called. It
    // persists across all UsrsctpSocket lifetimes — we never open or
    // close it from NimRTC code. We track acquire/release only for
    // API symmetry; the actual port is queried from usrsctp via
    // `nimrtc_get_userspace_udp_port()`.

    /** Acquire the shared UDP listener reference (no-op — socket is
     *  owned by usrsctp internals and persists for process lifetime). */
    static void s_acquire_shared_udp() noexcept;

    /** Release the shared UDP listener reference (no-op). */
    static void s_release_shared_udp() noexcept;

    /** Return the shared UDP listener port (in network byte order, as
     *  needed by SCTP_REMOTE_UDP_ENCAPS_PORT). Queried from usrsctp's
     *  internal socket via getsockname(). Falls back to 9899 (the
     *  usrsctp_init() port) if the socket is not accessible. */
    static std::uint16_t s_shared_udp_port_network() noexcept;

    /** Helper used by both listen() and connect() to bind the SCTP
     *  socket to 127.0.0.1:local_port (or :0 for ephemeral) and
     *  configure non-blocking mode + the SCTP_ASSOC_CHANGE
     *  notification subscription. */
    plugins::Status s_bind_and_configure(std::uint16_t local_port) noexcept;

    /** Transition state under state_mu_ and notify waiters. */
    void s_transition_to(UsrsctpSocketState next) noexcept;

    /** Helper for s_instance_recv_cb: parse an SCTP_ASSOC_CHANGE
     *  notification and update state_. Called with state_mu_ held by
     *  the trampoline. */
    void handle_notification(const void* data, std::size_t datalen) noexcept;
};

} // namespace nimrtc::sctp

#endif // NIMRTC_SCTP_USRSCTP_SOCKET_HPP
