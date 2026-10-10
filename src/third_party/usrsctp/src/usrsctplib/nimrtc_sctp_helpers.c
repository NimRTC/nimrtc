/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026, NimRTC contributors. All rights reserved.
 *
 * nimrtc_sctp_helpers.c — NimRTC/UsrsctpSocket seam helpers.
 *
 * This file is the bridge between NimRTC's C++ seam (src/sctp/src/
 * usrsctp_socket.cpp) and the upstream usrsctp 0.9.5.0 internals.
 * The C++ seam can't include the netinet/ headers directly because
 * they RE-DEFINE the SCTP UAPI struct tags that usrsctp.h already
 * defined (sockaddr_conn, sctp_event, sctp_event_subscribe, sctp_sndinfo,
 * sctp_rcvinfo, sctp_prinfo, sctp_sendv_spa, sctp_assoc_change, etc.),
 * and MSVC rejects the resulting C2011 "type redefinition" errors.
 *
 * This file is plain C; it sees ONLY the upstream netinet/ headers
 * (which define the same struct tags without re-defining them
 * against <usrsctp.h>) and exposes thin C-linkage symbols that
 * the .cpp calls to:
 *   - read/write the `userspace_udpsctp` slot of upstream's
 *     `system_base_info` (used to point usrsctp's IPv4/IPv6
 *     "userspace IP output" path at our shared UDP listener),
 *   - manually register the IPv4 loopback / INADDR_ANY addresses
 *     with usrsctp's interface table.
 *
 * The latter is needed because on Windows the standard way for
 * usrsctp to discover its host's addresses (calling
 * `socket(AF_INET, SOCK_RAW, IPPROTO_SCTP)` in `recv_thread_init`
 * in user_recv_thread.c) requires Administrator privileges. On a
 * non-admin build that call fails silently and `sctp_find_ifa_by_addr`
 * later returns NULL for 127.0.0.1, causing `usrsctp_bind()` to fail
 * with `EADDRNOTAVAIL`. We work around that by registering 127.0.0.1
 * (and 0.0.0.0) explicitly via `sctp_add_addr_to_vrf()` after
 * `usrsctp_init()` has run.
 */

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

#include <netinet/sctp_os_userspace.h>
#include <netinet/sctp_pcb.h>

void
nimrtc_set_userspace_udp_socket(int fd)
{
	/* On Windows `_WIN32 && !__MINGW32__` userspace_udpsctp is
	 * declared a SOCKET (pointer-sized handle); on POSIX/other
	 * builds it's `int`. Casting through `int` is safe because
	 * SOCKET values are bounded by the kernel socket table size
	 * (well under INT_MAX on any reasonable platform). */
	SCTP_BASE_VAR(userspace_udpsctp) = fd;
}

int
nimrtc_get_userspace_udp_socket(void)
{
	return (int)SCTP_BASE_VAR(userspace_udpsctp);
}

/*
 * nimrtc_get_userspace_udp_port — return the host-byte-order port
 * that usrsctp's internal IPv4 UDP listener is bound to, or 0 on
 * failure.
 *
 * usrsctp_init(port, ...) sets SCTP_BASE_SYSCTL(sctp_udp_tunneling_port) =
 * `port`, and recv_thread_init() binds userspace_udpsctp to that same
 * port. We query the socket directly via getsockname() rather than
 * tracking it in a NimRTC-side variable, because (a) the socket lives
 * inside usrsctp and we don't own it, and (b) the value is determined
 * once at usrsctp_init() time and never changes during the lifetime of
 * the process.
 *
 * Called from UsrsctpSocket::s_shared_udp_port_host() to configure
 * SCTP_REMOTE_UDP_ENCAPS_PORT on every per-socket listen()/connect()
 * call.
 */
int
nimrtc_get_userspace_udp_port(void)
{
#if defined(_WIN32) && !defined(__MINGW32__)
	SOCKET fd = SCTP_BASE_VAR(userspace_udpsctp);
	if (fd == INVALID_SOCKET) {
		return 0;
	}
#else
	int fd = SCTP_BASE_VAR(userspace_udpsctp);
	if (fd < 0) {
		return 0;
	}
#endif
	struct sockaddr_in sin;
	int sinlen = (int)sizeof(sin);
	memset(&sin, 0, sizeof(sin));
#if defined(_WIN32)
	int rc = getsockname((SOCKET)fd, (struct sockaddr *)&sin, &sinlen);
#else
	int rc = getsockname(fd, (struct sockaddr *)&sin, (socklen_t *)&sinlen);
#endif
	if (rc != 0) {
		return 0;
	}
	int port = (int)ntohs(sin.sin_port);
	return port;
}

void
nimrtc_register_loopback_addresses(void)
{
	/* On non-administrative Windows builds, recv_thread_init()'s
	 * raw IPv4 socket creation fails silently (SOCK_RAW + IPPROTO_SCTP
	 * requires admin on Windows). Without that socket, no IPv4
	 * address is added to usrsctp's interface hash table, and
	 * subsequent usrsctp_bind(s, 127.0.0.1, 0) calls fail with
	 * EADDRNOTAVAIL (sctp_find_ifa_by_addr returns NULL).
	 *
	 * We register 127.0.0.1 and 0.0.0.0 explicitly here so the
	 * bind succeeds. The interfaces don't carry a real raw socket
	 * — they're bookkeeping-only — because our NimRTC seam routes
	 * outbound SCTP packets via the conn_output callback / shared
	 * UDP listener rather than a real kernel SCTP socket.
	 *
	 * Calling this more than once is safe; sctp_add_addr_to_vrf
	 * walks the bucket chain and either finds an existing ifa or
	 * creates a new one. */
	struct sockaddr_in sin_loopback;
	struct sockaddr_in sin_any;
	const char *ifname = "NimRTC-loopback";

	memset(&sin_loopback, 0, sizeof(sin_loopback));
#ifdef HAVE_SIN_LEN
	sin_loopback.sin_len = sizeof(sin_loopback);
#endif
	sin_loopback.sin_family = AF_INET;
	sin_loopback.sin_port = htons(0);
	sin_loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	(void)sctp_add_addr_to_vrf(SCTP_DEFAULT_VRFID,
	                            (void *)ifname,
	                            0,
	                            0,
	                            ifname,
	                            NULL,
	                            (struct sockaddr *)&sin_loopback,
	                            0,
	                            0);

	memset(&sin_any, 0, sizeof(sin_any));
#ifdef HAVE_SIN_LEN
	sin_any.sin_len = sizeof(sin_any);
#endif
	sin_any.sin_family = AF_INET;
	sin_any.sin_port = htons(0);
	sin_any.sin_addr.s_addr = htonl(INADDR_ANY);

	(void)sctp_add_addr_to_vrf(SCTP_DEFAULT_VRFID,
	                            (void *)ifname,
	                            0,
	                            0,
	                            ifname,
	                            NULL,
	                            (struct sockaddr *)&sin_any,
	                            0,
	                            0);
}
