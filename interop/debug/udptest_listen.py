"""udptest_listen.py — minimal UDP listener.

Listens on the given port, prints every packet received.  Uses ctypes
to disable SIO_UDP_CONNRESET (so recvfrom() doesn't fail with WSAECONNRESET
after the peer sends ICMP-unreachable).  This is purely for diagnosing
whether NimRTC ↔ 127.0.0.1 UDP loopback actually works.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import socket
import sys
import time


def disable_connreset(sock: socket.socket) -> None:
    """Disable SIO_UDP_CONNRESET on a Windows socket."""
    SIO_UDP_CONNRESET = 0x9800000C
    # DWORD inBuf = FALSE; DWORD bytesReturned.
    in_buf = ctypes.c_ulong(0)
    ret = ctypes.windll.ws2_32.WSAIoctl(
        sock.fileno(),
        SIO_UDP_CONNRESET,
        ctypes.byref(in_buf), ctypes.sizeof(in_buf),
        None, 0,
        ctypes.byref(ctypes.c_ulong(0)),
        None, None,
    )
    if ret != 0:
        print(f"warning: SIO_UDP_CONNRESET failed (ret={ret}, err={ctypes.GetLastError()})", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", default="0.0.0.0:62000")
    ap.add_argument("--duration", type=int, default=30)
    args = ap.parse_args()

    host, port = args.listen.rsplit(":", 1)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    disable_connreset(sock)
    sock.bind((host, int(port)))
    sock.settimeout(0.5)
    print(f"listening on {host}:{port}", flush=True)

    start = time.time()
    rx_count = 0
    while time.time() - start < args.duration:
        try:
            data, addr = sock.recvfrom(2048)
        except socket.timeout:
            continue
        except OSError as e:
            print(f"recv error: {e}", flush=True)
            continue
        rx_count += 1
        if rx_count <= 5 or rx_count % 50 == 0:
            preview = data[:20].hex()
            print(f"RX #{rx_count} from {addr}: {len(data)}B first20={preview}", flush=True)
    print(f"done: {rx_count} packets received", flush=True)


if __name__ == "__main__":
    main()
