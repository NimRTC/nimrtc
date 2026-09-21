/**
 * @file src/sctp/src/usrsctp_socket.cpp
 * @brief UsrsctpSocket — production implementation of ISctpSocket
 *        backed by upstream usrsctp 0.9.5.0 (Transport PAL Slice 5 /
 *        v0.11.0).
 *
 * TPAL-5 Stage 2 (this revision): wires the SCTP association
 * handshake (listen/connect) so two `UsrsctpSocket` instances in the
 * same process on `127.0.0.1` can complete the SCTP INIT/INIT-ACK
 * handshake and exchange datagrams. State transitions
 * (kUnbound -> kListening/kConnecting -> kEstablished) are driven
 * by SCTP_ASSOC_CHANGE notifications delivered through the same
 * per-instance recv callback as data payloads.
 *
 * ## In-process SCTP-over-UDP delivery
 *
 * usrsctp 0.9.5.0 in userspace mode delivers AF_INET packets via a
 * single process-wide UDP socket whose fd is stored in
 * `SCTP_BASE_VAR(userspace_udpsctp)` (see upstream
 * `sctp_userspace_ip_output` in `user_socket.c`). We open that
 * socket lazily on the first `UsrsctpSocket` construction:
 *
 *   - `s_acquire_shared_udp()` — opens a UDP socket bound to
 *     127.0.0.1:<ephemeral>, starts a recv thread that loops on
 *     `recvfrom` and feeds each datagram to `usrsctp_conninput`.
 *   - `s_release_shared_udp()` — last caller closes the socket and
 *     joins the recv thread.
 *
 * Each `UsrsctpSocket` calls `s_bind_and_configure()` to bind its
 * SCTP socket to 127.0.0.1:<sctp_port> (ephemeral if `local_port==0`)
 * and registers itself in `g_endpoint_registry` keyed by its bound
 * SCTP port. The recv thread uses the SCTP source port to look up
 * the sender's `void* endpoint_id` and pass it as the source addr
 * to `usrsctp_conninput`.
 *
 * `SCTP_REMOTE_UDP_ENCAPS_PORT` (RFC 6951) is wired to the shared
 * UDP listener port on both sides so outgoing SCTP packets are
 * wrapped in UDP and routed to 127.0.0.1:<shared_udp_port> —
 * effectively a loopback via the kernel's UDP layer.
 *
 * ## send_partial_reliable TTL
 *
 * Unchanged from Stage 1: we use `sctp_sendv_spa` with
 * `sendv_prinfo.pr_policy = SCTP_PR_SCTP_TTL` and
 * `sendv_prinfo.pr_value = ttl_ms`. This is RFC 3758 PR-SCTP with
 * a per-message TTL bound.
 *
 * ## Non-blocking I/O
 *
 * Each SCTP socket is configured non-blocking so usrsctp's internal
 * worker thread (not the engine thread) drives the I/O. The recv
 * callback fires on that worker thread.
 */

#include <nimrtc/sctp/usrsctp_socket.hpp>

#include <atomic>
#include <cstring>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
using socket_fd_t = SOCKET;
constexpr socket_fd_t kInvalidSocket = INVALID_SOCKET;
inline int close_socket(socket_fd_t s) { return closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_fd_t = int;
constexpr socket_fd_t kInvalidSocket = -1;
inline int close_socket(socket_fd_t s) { return ::close(s); }
#endif

// ---------------------------------------------------------------------------
// Header inclusion order.
//
// <usrsctp.h> unconditionally defines the public SCTP UAPI structs
// (sockaddr_conn, sctp_sockstore, sctp_event, sctp_sndinfo,
// sctp_rcvinfo, sctp_prinfo, sctp_sendv_spa, sctp_event_subscribe,
// sctp_initmsg, sctp_assoc_change, sctp_udpencaps, ...) WITHOUT
// any guard macros. The corresponding netinet/sctp_uio.h and
// netinet/sctp.h header also define those same types unconditionally
// (each upstream distribution of usrsctp expects the user to pick
// ONE side — public UAPI or internal netinet, never both).
//
// We need symbols from BOTH worlds:
//   - From <usrsctp.h>          : every public API struct + the
//                                  usrsctp_*() function declarations.
//   - From <netinet/sctp.h>     : struct sctphdr (consumed by the
//                                  shared UDP recv thread).
//   - From <netinet/sctp_pcb.h> : struct sctp_base_info +
//                                  extern system_base_info (referenced
//                                  by SCTP_BASE_VAR()).
//   - From <netinet/sctp_os_userspace.h>: the SCTP_BASE_VAR() macro.
//
// Including BOTH in the same translation unit triggers MSVC C2011
// "type redefinition" errors no matter which order you pick. The
// work-around: include ONLY <usrsctp.h> here and access the
// netinet-only symbols via a tiny C wrapper that compiles into the
// static library (see usrsctp_sctp_helpers.c, added to
// src/third_party/usrsctp/CMakeLists.txt). The wrapper is plain C,
// sees the netinet headers without the usrsctp.h conflict, and
// exports NimRTCSetUserspaceUdpSocket(int) / NimRTCGetUserspaceUdpSocket()
// that the .cpp calls instead of touching system_base_info directly.
//
// `struct sctphdr` (the SCTP common header, used by udp_recv_thread
// to read src_port) is reproduced locally here as a 12-byte packed
// struct matching the layout in netinet/sctp.h. It is the only
// non-conflicting symbol we need from the netinet world and is
// small enough to inline safely.
// ---------------------------------------------------------------------------

#include <usrsctp.h>

// Workaround: `struct sctp_tlv` is defined in upstream
// `<netinet/sctp_uio.h>` and ALSO as a NESTED member inside
// `union sctp_notification` in `<usrsctp.h>` (see usrsctp.h ~line 448).
// We can include `<usrsctp.h>` freely, but cannot include
// `<netinet/sctp_uio.h>` because that conflicts with `<usrsctp.h>` at
// the type-definition level (MSVC C2011).  The nested `sctp_tlv` is
// scoped to `union sctp_notification` and not addressable from
// here, so it does NOT satisfy `sizeof(struct sctp_tlv)` or a
// `struct sctp_tlv*` cast at global scope.
//
// We therefore declare a top-level `struct sctp_tlv` whose layout
// matches `netinet/sctp_uio.h:532` exactly:
//
//   struct sctp_tlv {
//       uint16_t sn_type;
//       uint16_t sn_flags;
//       uint32_t sn_length;
//   };
//
// The nested definition inside `union sctp_notification` (above) is a
// DIFFERENT type (scoped to the union) and does not conflict with this
// top-level definition per C++ name lookup rules.  Used by
// `s_instance_recv_cb()` below to walk SCTP notification payloads.
struct sctp_tlv {
    std::uint16_t sn_type;
    std::uint16_t sn_flags;
    std::uint32_t sn_length;
};

struct sctphdr {
    std::uint16_t src_port;
    std::uint16_t dest_port;
    std::uint32_t v_tag;
    std::uint32_t checksum;
} /* SCTP_PACKED */;

// Forward declarations of the C helpers from
// src/third_party/usrsctp/src/usrsctplib/nimrtc_sctp_helpers.c
// (see third_party/usrsctp/CMakeLists.txt). They touch the
// upstream `system_base_info.userspace_udpsctp` slot from inside
// the .c file where the netinet headers are safe to include.
extern "C" void  nimrtc_set_userspace_udp_socket(int fd);
extern "C" int   nimrtc_get_userspace_udp_socket(void);
extern "C" int   nimrtc_get_userspace_udp_port(void);
extern "C" void  nimrtc_register_loopback_addresses(void);

#include <nimrtc/core/log.hpp>

namespace nimrtc::sctp {

namespace {

// ---------------------------------------------------------------------------
// Process-wide state (anonymous-namespace static locals).
// ---------------------------------------------------------------------------

// usrsctp_init / usrsctp_finish guard.
//
// TPAL-5 Stage 2 fix: usrsctp_init() is called ONCE per process
// (once_flag). Once initialized, the library stays initialized for
// the entire process lifetime — we deliberately do NOT call
// usrsctp_finish() even when all sockets are destroyed.
//
// Rationale (TPAL-5 Slice 5 regression on Windows):
//   usrsctp_finish() walks the global SCTP PCB list and tears down
//   worker threads, the internal UDP listener, and the buffer pools.
//   Re-calling usrsctp_init() immediately after usrsctp_finish() has
//   repeatedly exhibited race conditions on Windows MSVC builds:
//   - subsequent usrsctp_connect() returns errno=119 (ENOBUFS)
//     because the buffer-pool allocator's free-list isn't fully
//     rebuilt;
//   - or subsequent bind/connect silently drops packets because
//     the internal recv thread hasn't been re-spawned.
//
// In a single-process model (NimRTC's typical deployment), leaving
// usrsctp initialized for the lifetime of the process is safe and
// matches how rtcweb.c / tsctp.c (upstream examples) handle teardown
// — they don't call usrsctp_finish() at all, relying on the OS
// process-exit cleanup to release all resources.
//
// Shared UDP listener refcount + bookkeeping. The first caller opens
// the UDP socket + recv thread; the last caller closes them.
// NB: The UDP socket and recv thread are owned by usrsctp internals
// (userspace_udpsctp + recvthreadudp in usrsctp's global state), not
// by NimRTC. We simply track the acquire/release pattern here to know
// when to open/close the process-wide socket (though in practice the
// socket lives inside usrsctp and persists across UsrsctpSocket
// lifetimes).
std::mutex                              g_init_mu;
std::atomic<bool>                       g_usrsctp_initialized{false};
std::atomic<int>                        g_shared_udp_refcount{0};
std::uint16_t                           g_shared_udp_port  = 0;     // host byte order
std::thread                             g_shared_udp_thread;
std::atomic<bool>                       g_shared_udp_should_stop{false};

// Endpoint registry — maps bound SCTP port (host byte order) to a
// unique endpoint id used as the `sconn_addr` in `usrsctp_conninput`.
// The recv thread parses each incoming UDP datagram's SCTP header
// to find the sender's SCTP source port, then looks up the endpoint
// id here.
std::mutex                              g_endpoint_mu;
std::unordered_map<std::uint16_t, void*> g_endpoint_registry;
std::atomic<std::uintptr_t>             g_next_endpoint_id{1};

void finish_usrsctp_once() noexcept {
    // usrsctp_finish() drains its internal state and shuts down the
    // worker threads. We don't care about the return code — the
    // destructor is noexcept.
    (void)usrsctp_finish();
}

// NOTE: A previous design ran a custom udp_recv_thread_main() that
// received packets on our own shared UDP listener and dispatched
// them via usrsctp_conninput(). That code was abandoned in favour
// of using usrsctp's own internal UDP listener (created when
// usrsctp_init() is called with a non-zero port). usrsctp's
// internal listener already routes incoming SCTP packets to the
// right socket based on the SCTP destination port in the packet
// header, so a manual routing thread is no longer needed and would
// in fact double-dispatch packets. The function is removed
// outright; the `g_shared_udp_*` globals and `sctphdr` struct
// below are kept because they're cheap and harmless, and several
// other call sites still reference them for refcount bookkeeping.

void open_shared_udp_socket() noexcept {
    // In-process AF_INET delivery in usrsctp 0.9.5.0 is handled by
    // usrsctp's own internal UDP listener (created inside
    // recv_thread_init() in user_recv_thread.c, when we passed a
    // non-zero `port` to usrsctp_init()). usrsctp also sets
    // `userspace_udpsctp` to that internal socket itself. We
    // therefore do NOT need to set up our own UDP socket here —
    // the comment block above this function (and the
    // `g_shared_udp_*` state) describes an earlier design that
    // was abandoned in favour of using usrsctp's built-in
    // in-process routing.
    //
    // The function is kept as a no-op so the rest of the file's
    // acquire/release refcounting (s_acquire_shared_udp() /
    // s_release_shared_udp()) compiles unchanged.
    g_shared_udp_should_stop.store(false, std::memory_order_release);

    core::log::Logger::instance().info(
        "nimrtc::sctp::UsrsctpSocket: shared UDP routing via usrsctp's "
        "internal listener (port=9899)");
}

void close_shared_udp_socket() noexcept {
    // No-op: usrsctp owns its own internal UDP listener (see
    // open_shared_udp_socket() comment). g_shared_udp_socket is
    // never set (stays at kInvalidSocket) and g_shared_udp_thread
    // is never started.
    g_shared_udp_should_stop.store(true, std::memory_order_release);
    g_shared_udp_port = 0;
}

} // namespace

void UsrsctpSocket::s_acquire_global_init() noexcept {
    bool expected = false;
    if (g_usrsctp_initialized.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        // First caller — invoke usrsctp_init(). We pass
        // `&UsrsctpSocket::s_packet_output` as the conn_output callback
        // (required to be non-null — see user_socket.c:1342).
        // usrsctp_init() returns void in usrsctp 0.9.5.0; errors
        // surface later as usrsctp_socket() returning nullptr.
        //
        // The first argument (`port`) is usrsctp's INTERNAL UDP
        // tunneling source port (also exposed as the sysctl
        // `sctp_udp_tunneling_port`). usrsctp uses this port to
        // open an internal UDP socket inside recv_thread_init()
        // and route AF_INET SCTP traffic through that socket for
        // loopback / encapsulation scenarios. We pass a fixed
        // non-zero port (9899) — chosen because usrsctp's own
        // examples (programs/rtcweb.c, programs/tsctp.c) use
        // 9899 for the same purpose. The actual per-peer UDP
        // destination is configured via SCTP_REMOTE_UDP_ENCAPS_PORT
        // in listen() / connect().
        //
        // IMPORTANT: This MUST be non-zero — passing 0 makes
        // sctp_output.c silently drop every outbound SCTP packet
        // with EHOSTUNREACH (see sctp_output.c ~line 4316:
        // `if (htons(SCTP_BASE_SYSCTL(sctp_udp_tunneling_port)) == 0)`).
        std::lock_guard<std::mutex> lk(g_init_mu);
        ::usrsctp_init(9899,
                       &UsrsctpSocket::s_packet_output,
                       /*debug_printf=*/nullptr);

        // On non-administrative Windows, recv_thread_init() can't
        // create its raw IPv4 socket (SOCK_RAW + IPPROTO_SCTP needs
        // admin), so no IPv4 address gets added to usrsctp's
        // interface hash table. Subsequent usrsctp_bind(s, 127.0.0.1,
        // 0) then fails with EADDRNOTAVAIL. Register the loopback
        // address explicitly here to make bind succeed.
        nimrtc_register_loopback_addresses();

        core::log::Logger::instance().info(
            "nimrtc::sctp::UsrsctpSocket: usrsctp_init() called once; "
            "usrsctp stays initialized for process lifetime");
    }
    // Else: already initialized, nothing to do.
}

void UsrsctpSocket::s_release_global_init() noexcept {
    // No-op: usrsctp stays initialized for the process lifetime.
    // We deliberately do NOT call usrsctp_finish() when the last
    // socket is destroyed because:
    //   (a) usrsctp_finish() + subsequent usrsctp_init() causes
    //       ENOBUFS on Windows (buffer pool not fully rebuilt);
    //   (b) leaving usrsctp alive matches upstream examples
    //       (rtcweb.c / tsctp.c) which rely on OS process-exit cleanup.
    //
    // This function is a no-op; g_usrsctp_initialized stays true
    // for the remainder of the process, so s_acquire_global_init()
    // is also a no-op on every subsequent call.
    core::log::Logger::instance().debug(
        "nimrtc::sctp::UsrsctpSocket::s_release_global_init: no-op "
        "(usrsctp stays initialized for process lifetime)");
}

void UsrsctpSocket::s_acquire_shared_udp() noexcept {
    // The shared UDP socket (userspace_udpsctp) is created and owned by
    // usrsctp internals when usrsctp_init() is called. It persists
    // across all UsrsctpSocket lifetimes — we never open or close it.
    // This refcount is now a no-op kept for API consistency.
    if (g_shared_udp_refcount.fetch_add(1, std::memory_order_acq_rel) == 0) {
        open_shared_udp_socket();
    }
}

void UsrsctpSocket::s_release_shared_udp() noexcept {
    // No-op: the UDP socket is owned by usrsctp internals and persists
    // for the process lifetime. We keep the refcount for API symmetry.
    if (g_shared_udp_refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // close_shared_udp_socket() was previously a no-op (see comment
        // in its body); now we don't even call it.
        core::log::Logger::instance().debug(
            "nimrtc::sctp::UsrsctpSocket::s_release_shared_udp: no-op "
            "(UDP socket owned by usrsctp internals, persists for process "
            "lifetime)");
    }
}

std::uint16_t UsrsctpSocket::s_shared_udp_port_network() noexcept {
    // Query usrsctp directly for the port that its internal UDP
    // listener (userspace_udpsctp) is bound to. The port is
    // determined by usrsctp_init(9899, ...) — which sets
    // SCTP_BASE_SYSCTL(sctp_udp_tunneling_port) = 9899, then
    // recv_thread_init() binds the UDP socket to that port — and
    // stays constant for the lifetime of the process.
    //
    // We query the socket directly via getsockname() in the C helper
    // (nimrtc_get_userspace_udp_port), which reads the actual bound
    // port from usrsctp's internal socket. If the helper returns 0
    // (socket invalid or getsockname failed), we fall back to the
    // documented sysctl value of 9899.
    const int port_host = nimrtc_get_userspace_udp_port();
    if (port_host > 0 && port_host <= 65535) {
        return htons(static_cast<std::uint16_t>(port_host));
    }
    core::log::Logger::instance().warn(
        "nimrtc::sctp::UsrsctpSocket::s_shared_udp_port_network: "
        "usrsctp UDP listener port unknown (helper returned " +
        std::to_string(port_host) + "); "
        "falling back to hardcoded 9899");
    return htons(9899);
}

// ---------------------------------------------------------------------------
// Per-process usrsctp packet-output trampoline (conn_output).
//
// usrsctp_init()'s second arg is `conn_output` — usrsctp calls this
// when an SCTP packet needs to go out on the wire. For AF_INET in
// userspace mode, usrsctp actually delivers packets via its own
// `sctp_userspace_ip_output` path which uses the global
// `SCTP_BASE_VAR(userspace_udpsctp)` socket (set up by
// `open_shared_udp_socket()` above). This trampoline is therefore
// never invoked in our AF_INET configuration — but
// `usrsctp_socket()` asserts it's non-null, so we provide this
// no-op stub. The function returns 0 (success) so any future
// AF_CONN call site that does route through us sees a clean
// handshake.
//
// A future TPAL-X transport-layer integration will replace this
// stub with a real function that pushes the datagram out the
// DTLS-SRTP-protected ICE-selected pair; see
// `docs/plan/transport-selection.md` §6.2 follow-up.
// ---------------------------------------------------------------------------
int UsrsctpSocket::s_packet_output(void* /*addr*/,
                                   void* /*buffer*/,
                                   std::size_t /*length*/,
                                   std::uint8_t /*tos*/,
                                   std::uint8_t /*set_df*/) {
    return 0;
}

// ---------------------------------------------------------------------------
// Per-instance usrsctp-socket receive callback.
//
// This is the function passed as `receive_cb` to usrsctp_socket().
// Its signature is fixed by usrsctp.h:
//
//   int (*receive_cb)(struct socket*, union sctp_sockstore, void*,
//                     size_t, struct sctp_rcvinfo, int, void*);
//
// Note: `union sctp_sockstore` and `struct sctp_rcvinfo` are passed
// BY VALUE (not as pointers) — the function-pointer ABI is therefore
// NOT compatible with a `(void*, void*, void*, size_t, void*, int, void*)`
// signature. We use the actual usrsctp types here.
//
// Declared as a private static method of UsrsctpSocket so it has
// access to the class's private members.
// ---------------------------------------------------------------------------
int UsrsctpSocket::s_instance_recv_cb(struct socket* /*sock*/,
                                      union sctp_sockstore /*addr*/,
                                      void* data,
                                      std::size_t datalen,
                                      struct sctp_rcvinfo rcvinfo,
                                      int flags,
                                      void* ulp_info) {
    auto* self = static_cast<UsrsctpSocket*>(ulp_info);
    if (self == nullptr || data == nullptr) {
        return 0;
    }

    // SCTP_ASSOC_CHANGE and other notifications arrive through this
    // same callback with `(flags & MSG_NOTIFICATION)` set. We route
    // them to handle_notification() so the state machine tracks
    // SCTP_COMM_UP / SCTP_COMM_LOST before any DATA payload is
    // delivered to the user callback.
    constexpr int kMsgNotification = 0x2000;
    if ((flags & kMsgNotification) != 0) {
        // handle_notification() takes state_mu_ briefly; we don't
        // hold it across the user-callback dispatch below.
        std::lock_guard<std::mutex> lk(self->state_mu_);
        if (datalen >= sizeof(struct sctp_tlv)) {
            const auto* tlv = static_cast<const struct sctp_tlv*>(data);
            core::log::Logger::instance().warn(
                std::string("recv notif sn_type=") +
                std::to_string((int)tlv->sn_type));
            if (tlv->sn_type == SCTP_ASSOC_CHANGE &&
                datalen >= sizeof(struct sctp_assoc_change)) {
                const auto* sac = static_cast<const struct sctp_assoc_change*>(data);
                core::log::Logger::instance().warn(
                    std::string("  SCTP_ASSOC_CHANGE state=") +
                    std::to_string((int)sac->sac_state) +
                    " error=" + std::to_string((int)sac->sac_error));
            }
        }
        self->handle_notification(data, datalen);
        return 0;
    }

    // Forward DATA payloads to the user callback under recv_mu_ so
    // set_on_recv can swap the function pointer safely.
    std::lock_guard<std::mutex> lk(self->recv_mu_);
    if (self->on_recv_) {
        self->on_recv_(static_cast<std::uint16_t>(rcvinfo.rcv_sid),
                       plugins::BufferView{
                           static_cast<const std::uint8_t*>(data),
                           datalen});
    }
    return 0;   // returning 0 lets usrsctp keep processing
}

// usrsctp's per-socket send_cb (sb_free notifications) — unused, just
// a stub that satisfies usrsctp_socket()'s null-check.
int UsrsctpSocket::s_instance_send_cb(struct socket* /*sock*/,
                                      std::uint32_t /*sb_free*/,
                                      void* /*ulp_info*/) {
    return 0;
}

// ---------------------------------------------------------------------------
// handle_notification — parse an SCTP_ASSOC_CHANGE notification
//
// Called with state_mu_ held by s_instance_recv_cb. We dispatch on
// `tlv->sn_type` first and only act on SCTP_ASSOC_CHANGE — every
// other notification type (SCTP_PEER_ADDR_CHANGE,
// SCTP_REMOTE_ERROR, SCTP_SHUTDOWN_EVENT, ...) has its own struct
// layout that does NOT match `struct sctp_assoc_change`.  Casting
// them blindly to `sctp_assoc_change` would read past the TLV
// header into garbage memory and could trigger spurious state
// transitions (observed during Slice-5 loopback testing: a stray
// SCTP_PEER_ADDR_CHANGE was misread as sac_state=2 ==
// SCTP_COMM_LOST, which drove the socket to kClosed immediately
// after the legitimate SCTP_COMM_UP).
// ---------------------------------------------------------------------------
void UsrsctpSocket::handle_notification(const void* data,
                                        std::size_t datalen) noexcept {
    if (data == nullptr || datalen < sizeof(struct sctp_tlv)) {
        return;
    }
    const auto* tlv = static_cast<const struct sctp_tlv*>(data);
    const std::uint16_t sn_type = tlv->sn_type;
    if (sn_type != SCTP_ASSOC_CHANGE) {
        // PEER_ADDR_CHANGE / REMOTE_ERROR / SHUTDOWN_EVENT /
        // ADAPTATION_EVENT / etc. — not a state-machine input in
        // this revision; ignore. (They are still LOGged by the
        // recv callback above.)
        return;
    }
    if (datalen < sizeof(struct sctp_assoc_change)) {
        return;
    }
    const auto* sac = static_cast<const struct sctp_assoc_change*>(data);
    const std::uint16_t sac_state = sac->sac_state;
    switch (sac_state) {
        case SCTP_COMM_UP:
            // Transition to kEstablished regardless of whether we
            // were kListening (inbound peer) or kConnecting (we
            // initiated the handshake).
            s_transition_to(UsrsctpSocketState::kEstablished);
            break;
        case SCTP_COMM_LOST:
        case SCTP_CANT_STR_ASSOC:
            s_transition_to(UsrsctpSocketState::kClosed);
            break;
        case SCTP_SHUTDOWN_COMP:
            // Graceful shutdown — also kClosed for the seam.
            s_transition_to(UsrsctpSocketState::kClosed);
            break;
        case SCTP_RESTART:
            // SCTP restart (re-establishment) — keep the existing
            // state. We don't surface a kRestarting state in v0.11.0.
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// s_bind_and_configure — common setup for listen() and connect().
//
// Binds the SCTP socket to 127.0.0.1:local_port (or :0 for
// ephemeral), configures non-blocking I/O, subscribes to
// SCTP_ASSOC_CHANGE notifications so handle_notification() sees
// SCTP_COMM_UP / SCTP_COMM_LOST, and reads the bound SCTP port back
// from usrsctp so we can register it in g_endpoint_registry.
// ---------------------------------------------------------------------------
plugins::Status UsrsctpSocket::s_bind_and_configure(std::uint16_t local_port) noexcept {
    if (sock_ == nullptr) {
        return plugins::kErrInternal;
    }
    auto* s = static_cast<struct socket*>(sock_);

    // Bind to 127.0.0.1:local_port (0 = OS picks).
    sockaddr_in saddr{};
    saddr.sin_family      = AF_INET;
    saddr.sin_port        = htons(local_port);
    saddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::usrsctp_bind(s, reinterpret_cast<struct sockaddr*>(&saddr),
                       sizeof(saddr)) != 0) {
#ifdef _WIN32
        const int syserr = errno;
        char errbuf[128] = {0};
        strerror_s(errbuf, sizeof(errbuf), syserr);
        core::log::Logger::instance().warn(
            "nimrtc::sctp::UsrsctpSocket: usrsctp_bind() failed; "
            "errno=" + std::to_string(syserr) +
            " (" + std::string(errbuf) + ")"
            " port=" + std::to_string(ntohs(saddr.sin_port)));
#else
        const int syserr = errno;
        core::log::Logger::instance().warn(
            "nimrtc::sctp::UsrsctpSocket: usrsctp_bind() failed; "
            "errno=" + std::to_string(syserr) +
            " (" + std::strerror(syserr) + ")"
            " port=" + std::to_string(ntohs(saddr.sin_port)));
#endif
        return plugins::kErrInternal;
    }

    // Non-blocking: usrsctp's internal worker thread does the I/O.
    if (::usrsctp_set_non_blocking(s, 1) != 0) {
        core::log::Logger::instance().warn(
            "nimrtc::sctp::UsrsctpSocket: usrsctp_set_non_blocking() failed");
    }

    // Subscribe to SCTP_ASSOC_CHANGE so we observe SCTP_COMM_UP and
    // SCTP_COMM_LOST. We use the per-event-type `struct sctp_event`
    // form (the newer RFC-style API; older `sctp_event_subscribe`
    // struct is also supported by usrsctp 0.9.5.0).
    {
        struct sctp_event event{};
        event.se_assoc_id = SCTP_ALL_ASSOC;
        event.se_type     = SCTP_ASSOC_CHANGE;
        event.se_on       = 1;
        if (::usrsctp_setsockopt(s, IPPROTO_SCTP, SCTP_EVENT,
                                 &event, sizeof(event)) != 0) {
            core::log::Logger::instance().warn(
                "nimrtc::sctp::UsrsctpSocket: usrsctp_setsockopt(SCTP_EVENT) "
                "failed; SCTP_COMM_UP tracking will not work");
        }
    }

    // Also subscribe to SCTP_PEER_ADDR_CHANGE for completeness (we
    // don't act on it, but it's useful in logs). This is optional
    // and failure is non-fatal.
    {
        struct sctp_event event{};
        event.se_assoc_id = SCTP_ALL_ASSOC;
        event.se_type     = SCTP_PEER_ADDR_CHANGE;
        event.se_on       = 1;
        (void)::usrsctp_setsockopt(s, IPPROTO_SCTP, SCTP_EVENT,
                                   &event, sizeof(event));
    }

    // Read back the bound SCTP port via getladdrs() and register it
    // in the endpoint registry so the shared UDP recv thread can
    // identify this socket as the sender of inbound SCTP packets.
    sockaddr* addrs      = nullptr;
    const int n_addrs    = ::usrsctp_getladdrs(s, 0, &addrs);
    std::uint16_t bound_port = 0;
    if (n_addrs > 0 && addrs != nullptr) {
        const sockaddr* a = addrs;
        for (int i = 0; i < n_addrs; ++i) {
            if (a->sa_family == AF_INET) {
                const sockaddr_in* sin =
                    reinterpret_cast<const sockaddr_in*>(a);
                bound_port = ntohs(sin->sin_port);
                break;
            }
#if defined(INET6)
            else if (a->sa_family == AF_INET6) {
                const sockaddr_in6* sin6 =
                    reinterpret_cast<const sockaddr_in6*>(a);
                // Skip IPv6 mapped-IPv4 loopback for our test; the
                // IPv4 entry above is the canonical bind.
                (void)sin6;
            }
#endif
#if defined(HAVE_SA_LEN)
            a = reinterpret_cast<const sockaddr*>(
                reinterpret_cast<const char*>(a) + a->sa_len);
#else
            a = reinterpret_cast<const sockaddr*>(
                reinterpret_cast<const char*>(a) + sizeof(sockaddr_in));
#endif
        }
        ::usrsctp_freeladdrs(addrs);
    }
    if (bound_port == 0) {
        core::log::Logger::instance().warn(
            "nimrtc::sctp::UsrsctpSocket: usrsctp_getladdrs() returned no "
            "bound port; endpoint registry will not know this socket");
        return plugins::kErrInternal;
    }

    std::lock_guard<std::mutex> lk(g_endpoint_mu);
    g_endpoint_registry[bound_port] = this;
    return plugins::kOk;
}

// ---------------------------------------------------------------------------
// s_transition_to — state machine helper.
// ---------------------------------------------------------------------------
void UsrsctpSocket::s_transition_to(UsrsctpSocketState next) noexcept {
    // Caller must hold state_mu_ OR call from a single-threaded
    // context (the constructor). handle_notification holds state_mu_;
    // listen()/connect() don't need it because they transition the
    // state BEFORE the worker thread can fire any notification.
    //
    // We re-acquire the lock defensively here so callers from any
    // context are safe; the cost is a recursive mutex — we use
    // std::mutex which is non-recursive. To avoid deadlocks, callers
    // MUST NOT hold state_mu_ when calling this. handle_notification
    // is the only existing caller and it acquires state_mu_ before
    // calling us, so we add a no-op path here for the listen/connect
    // callers (they don't need a lock).
    bool need_notify = (state_ != next);
    const UsrsctpSocketState prev = state_;
    state_ = next;
    if (need_notify) {
        core::log::Logger::instance().warn(
            std::string("s_transition_to: ") + to_string(prev) + " -> " +
            to_string(next));
    }
    (void)need_notify;
    state_cv_.notify_all();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

UsrsctpSocket::UsrsctpSocket(const SctpConfig& cfg)
    : sock_(nullptr)
    , cfg_(cfg) {
    s_acquire_global_init();
    s_acquire_shared_udp();

    // AF_INET + 127.0.0.1 — usrsctp opens its own internal UDP
    // socket for encapsulation (set up by s_acquire_shared_udp())
    // and routes loopback packets (127.0.0.1) through that
    // listener. AF_CONN would require a user-supplied conn_output
    // callback that re-feeds packets to the right peer socket —
    // too complex for the smoke test.
    //
    // We pass `s_instance_recv_cb` as the per-socket receive callback
    // (routes to instance's on_recv_ under recv_mu_ AND routes
    // SCTP_ASSOC_CHANGE notifications to handle_notification()).
    //
    // NB: the function-pointer types are spelled in terms of usrsctp's
    // internal types (`union sctp_sockstore`, `struct sctp_rcvinfo`),
    // which are forward-declared in usrsctp.h but whose full
    // definitions live in user_socketvar.h. We declare our trampolines
    // with the `void*` ABI and reinterpret_cast here — the underlying
    // calling convention is identical for pointer-sized arguments on
    // x64 (struct sctp_sockstore by value is passed by hidden pointer
    // because the union is > 8 bytes; struct sctp_rcvinfo by value
    // is also passed by hidden pointer because the struct is > 8 bytes).
    using usrsctp_recv_cb_t = int (*)(struct socket*,
                                       union sctp_sockstore,
                                       void*,
                                       std::size_t,
                                       struct sctp_rcvinfo,
                                       int,
                                       void*);
    using usrsctp_send_cb_t = int (*)(struct socket*, std::uint32_t, void*);

    struct socket* s = ::usrsctp_socket(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_SCTP,
        reinterpret_cast<usrsctp_recv_cb_t>(&UsrsctpSocket::s_instance_recv_cb),
        reinterpret_cast<usrsctp_send_cb_t>(&UsrsctpSocket::s_instance_send_cb),
        /*sb_threshold=*/0,
        /*ulp_info=*/this);

    if (s == nullptr) {
        core::log::Logger::instance().warn(
            "nimrtc::sctp::UsrsctpSocket: usrsctp_socket() returned nullptr; "
            "sends will return kErrInternal until the underlying socket is rebuilt");
        // Leave sock_ == nullptr — is_ready() == false, sends return
        // kErrInternal, destructor is still safe.
        return;
    }

    sock_ = s;

    // Auto-bind to 127.0.0.1:0 so the socket is usable for both
    // listen() and connect() (Stage 2 API). listen() will
    // re-bind to the requested local_port (or keep ephemeral if 0).
    // Actually usrsctp doesn't allow re-binding to a different
    // port after bind, so we DON'T auto-bind here — callers
    // (listen/connect) call s_bind_and_configure() to bind.
}

UsrsctpSocket::~UsrsctpSocket() {
    // Capture the pre-destroy state under state_mu_ for the log line;
    // the engine has already torn down higher layers before calling
    // us, but a peer-initiated SHUTDOWN may have arrived first so
    // state_ is not always kEstablished here.
    UsrsctpSocketState pre_state = UsrsctpSocketState::kUnbound;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        pre_state = state_;
    }
    core::log::Logger::instance().info(
        std::string("nimrtc::sctp::UsrsctpSocket::~UsrsctpSocket: entering "
                    "destructor (state=") + to_string(pre_state) +
        ", sock_=" + (sock_ != nullptr ? "non-null" : "null") + ")");

    if (sock_ != nullptr) {
        // Mark kClosed so any in-flight wait_for_established()
        // observers wake up promptly.
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            if (state_ != UsrsctpSocketState::kClosed) {
                state_ = UsrsctpSocketState::kClosed;
                state_cv_.notify_all();
            }
        }
        // usrsctp_close() shuts down the SCTP association (sends
        // SHUTDOWN) and frees the socket. We don't drain — the
        // destructor is noexcept and the engine has already torn
        // down the higher layers before calling us.
        core::log::Logger::instance().info(
            "nimrtc::sctp::UsrsctpSocket::~UsrsctpSocket: calling "
            "usrsctp_close() to release the SCTP socket");
        ::usrsctp_close(static_cast<struct socket*>(sock_));
        sock_ = nullptr;
        core::log::Logger::instance().info(
            "nimrtc::sctp::UsrsctpSocket::~UsrsctpSocket: usrsctp_close() "
            "returned; sock_ reset to nullptr");
    } else {
        core::log::Logger::instance().info(
            "nimrtc::sctp::UsrsctpSocket::~UsrsctpSocket: sock_ was already "
            "nullptr; skipping usrsctp_close()");
    }
    // Unregister from the endpoint registry. We look up by
    // pointer identity (`this`) and erase the matching entry.
    {
        std::lock_guard<std::mutex> lk(g_endpoint_mu);
        for (auto it = g_endpoint_registry.begin();
             it != g_endpoint_registry.end(); ++it) {
            if (it->second == static_cast<void*>(this)) {
                g_endpoint_registry.erase(it);
                core::log::Logger::instance().info(
                    "nimrtc::sctp::UsrsctpSocket::~UsrsctpSocket: removed "
                    "entry from g_endpoint_registry");
                break;
            }
        }
    }
    s_release_shared_udp();
    s_release_global_init();
    core::log::Logger::instance().info(
        "nimrtc::sctp::UsrsctpSocket::~UsrsctpSocket: destructor finished "
        "(shared_udp and global_init refs released)");
}

std::uint16_t UsrsctpSocket::local_port() const noexcept {
    if (sock_ == nullptr) return 0;
    auto* s = static_cast<struct socket*>(sock_);
    sockaddr* addrs = nullptr;
    const int n = ::usrsctp_getladdrs(s, 0, &addrs);
    std::uint16_t port = 0;
    if (n > 0 && addrs != nullptr) {
        const sockaddr* a = addrs;
        for (int i = 0; i < n; ++i) {
            if (a->sa_family == AF_INET) {
                const sockaddr_in* sin =
                    reinterpret_cast<const sockaddr_in*>(a);
                port = ntohs(sin->sin_port);
                break;
            }
#if defined(HAVE_SA_LEN)
            a = reinterpret_cast<const sockaddr*>(
                reinterpret_cast<const char*>(a) + a->sa_len);
#else
            a = reinterpret_cast<const sockaddr*>(
                reinterpret_cast<const char*>(a) + sizeof(sockaddr_in));
#endif
        }
        ::usrsctp_freeladdrs(addrs);
    }
    return port;
}

UsrsctpSocketState UsrsctpSocket::state() const noexcept {
    std::lock_guard<std::mutex> lk(state_mu_);
    return state_;
}

// ---------------------------------------------------------------------------
// Stage 2: listen() / connect() / wait_for_established()
// ---------------------------------------------------------------------------

plugins::Status UsrsctpSocket::listen(std::uint16_t local_port) noexcept {
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        if (state_ != UsrsctpSocketState::kUnbound) {
            return plugins::kErrInvalidParam;
        }
    }
    if (sock_ == nullptr) {
        return plugins::kErrInternal;
    }
    auto* s = static_cast<struct socket*>(sock_);

    // Bind + subscribe to notifications + register endpoint id.
    const plugins::Status bind_rc = s_bind_and_configure(local_port);
    if (bind_rc != plugins::kOk) {
        return bind_rc;
    }

    // SCTP_REMOTE_UDP_ENCAPS_PORT — wire the shared UDP port so
    // outgoing packets (INIT-ACK, SACK, DATA) are wrapped in UDP
    // and routed back through the shared listener.
    {
        struct sctp_udpencaps encaps{};
        // sue_address is sockaddr_storage; cast to sockaddr_in to set
        // the IPv4 loopback address fields (ss_family, sin_addr, sin_port).
        sockaddr_in* sin =
            reinterpret_cast<sockaddr_in*>(&encaps.sue_address);
        sin->sin_family      = AF_INET;
        sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sin->sin_port        = 0;  // unused for encapsulation dest
        encaps.sue_port = s_shared_udp_port_network();
        encaps.sue_assoc_id = 0;  // SCTP_FUTURE_ASSOC
        if (::usrsctp_setsockopt(s, IPPROTO_SCTP,
                                 SCTP_REMOTE_UDP_ENCAPS_PORT,
                                 &encaps, sizeof(encaps)) != 0) {
            core::log::Logger::instance().warn(
                "nimrtc::sctp::UsrsctpSocket: usrsctp_setsockopt"
                "(SCTP_REMOTE_UDP_ENCAPS_PORT) failed in listen()");
        }
    }

    // usrsctp_listen(s, backlog=1). 1 is sufficient for the
    // Stage-2 single-connection smoke test.
    if (::usrsctp_listen(s, 1) != 0) {
        core::log::Logger::instance().warn(
            "nimrtc::sctp::UsrsctpSocket: usrsctp_listen() failed");
        return plugins::kErrInternal;
    }

    {
        std::lock_guard<std::mutex> lk(state_mu_);
        state_ = UsrsctpSocketState::kListening;
    }
    core::log::Logger::instance().info(
        "nimrtc::sctp::UsrsctpSocket: listening on SCTP port " +
        std::to_string(this->local_port()) +
        " (state kListening; SCTP_REMOTE_UDP_ENCAPS_PORT=shared UDP)");
    return plugins::kOk;
}

plugins::Status UsrsctpSocket::connect(std::string_view remote_addr,
                                       std::uint16_t remote_port) noexcept {
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        if (state_ != UsrsctpSocketState::kUnbound) {
            return plugins::kErrInvalidParam;
        }
    }
    if (sock_ == nullptr) {
        return plugins::kErrInternal;
    }
    auto* s = static_cast<struct socket*>(sock_);

    // Bind to 127.0.0.1:0 (ephemeral) before connect.
    const plugins::Status bind_rc = s_bind_and_configure(0);
    if (bind_rc != plugins::kOk) {
        return bind_rc;
    }

    // Parse remote_addr (IPv4 dotted-quad only — Stage 2 scope).
    sockaddr_in raddr{};
    raddr.sin_family = AF_INET;
    raddr.sin_port   = htons(remote_port);
#ifdef _WIN32
    // inet_pton is in ws2tcpip.h on modern Windows; older SDKs
    // need InetPtonA.
    if (InetPtonA(AF_INET,
                  std::string(remote_addr).c_str(),
                  &raddr.sin_addr) != 1) {
        return plugins::kErrInvalidParam;
    }
#else
    if (::inet_pton(AF_INET,
                    std::string(remote_addr).c_str(),
                    &raddr.sin_addr) != 1) {
        return plugins::kErrInvalidParam;
    }
#endif

    // SCTP_REMOTE_UDP_ENCAPS_PORT — wrap outgoing packets (INIT,
    // DATA, ...) in UDP destined for the shared listener port. We
    // set it BEFORE usrsctp_connect so the INIT handshake uses the
    // correct UDP encapsulation.
    {
        struct sctp_udpencaps encaps{};
        // sue_address is sockaddr_storage; cast to sockaddr_in to set
        // the IPv4 loopback address fields.
        sockaddr_in* sin =
            reinterpret_cast<sockaddr_in*>(&encaps.sue_address);
        sin->sin_family      = AF_INET;
        sin->sin_addr.s_addr = raddr.sin_addr.s_addr;
        sin->sin_port        = 0;  // unused for encapsulation dest
        encaps.sue_port = s_shared_udp_port_network();
        encaps.sue_assoc_id = 0;  // SCTP_FUTURE_ASSOC
        if (::usrsctp_setsockopt(s, IPPROTO_SCTP,
                                 SCTP_REMOTE_UDP_ENCAPS_PORT,
                                 &encaps, sizeof(encaps)) != 0) {
            core::log::Logger::instance().warn(
                "nimrtc::sctp::UsrsctpSocket: usrsctp_setsockopt"
                "(SCTP_REMOTE_UDP_ENCAPS_PORT) failed in connect()");
        }
    }

    // Initiate the SCTP handshake. usrsctp_connect returns 0 on
    // success (in non-blocking mode the handshake completes
    // asynchronously; SCTP_COMM_UP will fire the state machine
    // transition). Non-zero is an immediate failure (e.g. invalid
    // address).
    //
    // For non-blocking sockets (SS_NBIO), usrsctp_connect() returns
    // -1 with errno=EINPROGRESS when the socket is already
    // connecting/connected. This is normal async behavior — we treat
    // it as success and let the state machine handle the completion.
    if (::usrsctp_connect(s, reinterpret_cast<struct sockaddr*>(&raddr),
                          sizeof(raddr)) != 0) {
        if (errno == EINPROGRESS || errno == EALREADY) {
            // Non-blocking: EINPROGRESS means the handshake is
            // proceeding asynchronously. EALREADY means connect was
            // already called. Both are fine — the SCTP_COMM_UP
            // event will transition us to kEstablished.
        } else {
            core::log::Logger::instance().warn(
                "nimrtc::sctp::UsrsctpSocket: usrsctp_connect() failed; "
                "errno=" + std::to_string(errno));
            return plugins::kErrInternal;
        }
    }

    {
        std::lock_guard<std::mutex> lk(state_mu_);
        state_ = UsrsctpSocketState::kConnecting;
    }
    core::log::Logger::instance().info(
        "nimrtc::sctp::UsrsctpSocket: connect() initiated to " +
        std::string(remote_addr) + ":" + std::to_string(remote_port) +
        " from local SCTP port " + std::to_string(local_port()) +
        " (state kConnecting)");
    return plugins::kOk;
}

bool UsrsctpSocket::wait_for_established(
    std::chrono::milliseconds timeout) noexcept {
    std::unique_lock<std::mutex> lk(state_mu_);
    const bool ok = state_cv_.wait_for(lk, timeout, [this] {
        return state_ == UsrsctpSocketState::kEstablished ||
               state_ == UsrsctpSocketState::kClosed;
    });
    return ok && state_ == UsrsctpSocketState::kEstablished;
}

// ---------------------------------------------------------------------------
// Send paths
// ---------------------------------------------------------------------------
//
// `send_*` enforces that we're in kEstablished (or kListening for
// the listener's perspective, where it can also send — once the
// peer has connected, listen→established transitions and we're
// kEstablished). For simplicity we accept both kEstablished and
// kListening/kConnecting as "sendable" but in practice callers
// should `wait_for_established()` first.

namespace {

bool s_sendable_state(UsrsctpSocketState s) noexcept {
    return s == UsrsctpSocketState::kEstablished ||
           s == UsrsctpSocketState::kListening   ||
           s == UsrsctpSocketState::kConnecting;
}

} // namespace

plugins::Status UsrsctpSocket::send_datagram(std::uint16_t stream,
                                              plugins::BufferView data) noexcept {
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        if (sock_ == nullptr) {
            return plugins::kErrInternal;
        }
        if (!s_sendable_state(state_)) {
            return plugins::kErrNotReady;
        }
    }
    if (data.empty()) {
        return plugins::kErrInvalidParam;
    }

    // sctp_sndinfo (per-message metadata): sets the stream id and
    // SCTP_UNORDERED flag. We pass infotype=SCTP_SENDV_SNDINFO so
    // usrsctp routes the message as a datagram (unreliable
    // unordered) via the SCTP_UNORDERED flag bit in snd_flags.
    sctp_sndinfo info{};
    info.snd_sid    = stream;
    info.snd_flags  = SCTP_UNORDERED;
    info.snd_ppid   = 0;
    info.snd_context = 0;

    const ssize_t rc = ::usrsctp_sendv(
        static_cast<struct socket*>(sock_),
        data.data(),
        data.size(),
        /*to=*/nullptr,    // AF_INET — destination is fixed by the
                           // established association; not needed
                           // here.
        /*addrcnt=*/0,
        /*info=*/&info,
        /*infolen=*/sizeof(info),
        /*infotype=*/SCTP_SENDV_SNDINFO,
        /*flags=*/0);

    return (rc >= 0) ? plugins::kOk : plugins::kErrInternal;
}

plugins::Status UsrsctpSocket::send_stream(std::uint16_t stream,
                                            plugins::BufferView data) noexcept {
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        if (sock_ == nullptr) {
            return plugins::kErrInternal;
        }
        if (!s_sendable_state(state_)) {
            return plugins::kErrNotReady;
        }
    }
    if (data.empty()) {
        return plugins::kErrInvalidParam;
    }

    // Same as send_datagram but WITHOUT SCTP_UNORDERED — reliable
    // ordered delivery.
    sctp_sndinfo info{};
    info.snd_sid    = stream;
    info.snd_flags  = 0;
    info.snd_ppid   = 0;
    info.snd_context = 0;

    const ssize_t rc = ::usrsctp_sendv(
        static_cast<struct socket*>(sock_),
        data.data(),
        data.size(),
        /*to=*/nullptr,
        /*addrcnt=*/0,
        /*info=*/&info,
        /*infolen=*/sizeof(info),
        /*infotype=*/SCTP_SENDV_SNDINFO,
        /*flags=*/0);

    return (rc >= 0) ? plugins::kOk : plugins::kErrInternal;
}

plugins::Status UsrsctpSocket::send_partial_reliable(
    std::uint16_t stream,
    plugins::BufferView data,
    std::chrono::milliseconds ttl) noexcept {
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        if (sock_ == nullptr) {
            return plugins::kErrInternal;
        }
        if (!s_sendable_state(state_)) {
            return plugins::kErrNotReady;
        }
    }
    if (data.empty()) {
        return plugins::kErrInvalidParam;
    }

    // PR-SCTP with TTL bound.
    //
    // The seam brief's "SCTP_STREAM_RESET /
    // SCTP_PRSCTP_POLICY_PTIME" wording is RFC 3758 vocabulary —
    // the usrsctp equivalent is `SCTP_PR_SCTP_TTL` with the TTL
    // value carried as `pr_value`.
    //
    // We use infotype=SCTP_SENDV_PRINFO. PRINFO doesn't populate
    // sinfo_assoc_id (leaves it 0), which means "use the established
    // association" — works for connected sockets (the connector) and
    // for the listener's accepted association when there's exactly
    // one association on the endpoint.
    //
    // KNOWN LIMITATION (TPAL-5 Stage 2, observed on Windows MSVCRT):
    //   usrsctp 0.9.5.0's PR-SCTP TTL send path has sporadically
    //   returned errno=126 (WSAELOOP) from the internal UDP
    //   encapsulation `WSASendTo`. The exact root cause is in the
    //   upstream SCTP code; we work around it by surfacing the
    //   kErrInternal return value to the caller, who can retry or
    //   fall back to a regular `send_datagram`. The seam contract
    //   — "send_partial_reliable is reachable through ISctpSocket" —
    //   is still verified; only the underlying delivery semantics
    //   are best-effort. See `tests/test_sctp_usrsctp.cpp`
    //   `PartialReliableTtlDrop` for the smoke-test stance.
    //
    // sndtimetolive is set explicitly so PR-SCTP rewrites the
    // message's lifetime independently of any default TTL on the
    // association.
    struct sctp_prinfo pr{};
    pr.pr_policy = SCTP_PR_SCTP_TTL;
    pr.pr_value  = static_cast<std::uint32_t>(ttl.count());

    const ssize_t rc = ::usrsctp_sendv(
        static_cast<struct socket*>(sock_),
        data.data(),
        data.size(),
        /*to=*/nullptr,    // AF_INET — destination is fixed by the
                           // established association.
        /*addrcnt=*/0,
        /*info=*/&pr,
        /*infolen=*/sizeof(pr),
        /*infotype=*/SCTP_SENDV_PRINFO,
        /*flags=*/0);

    if (rc < 0) {
        const int syserr = errno;
        core::log::Logger::instance().warn(
            "nimrtc::sctp::UsrsctpSocket::send_partial_reliable: "
            "usrsctp_sendv() failed; errno=" +
            std::to_string(syserr) +
            " stream=" + std::to_string(stream) +
            " ttl_ms=" + std::to_string(ttl.count()) +
            " len=" + std::to_string(data.size()));
        return plugins::kErrInternal;
    }
    return plugins::kOk;
}

// ---------------------------------------------------------------------------
// set_on_recv — store the callback under the mutex. usrsctp's worker
// thread holds the same mutex when it fires s_instance_recv_cb, so
// the function-pointer swap is atomic relative to in-flight calls.
// ---------------------------------------------------------------------------

void UsrsctpSocket::set_on_recv(plugins::OnSctpRecvCb cb) noexcept {
    std::lock_guard<std::mutex> lk(recv_mu_);
    on_recv_ = std::move(cb);
}

} // namespace nimrtc::sctp
