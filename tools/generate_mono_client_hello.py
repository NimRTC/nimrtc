#!/usr/bin/env python3
"""
generate_mono_client_hello.py

Build a single-DTLS-record (non-fragmented) ClientHello that mirrors
the synthetic body from generate_synthetic_client_hello.py but uses
proper non-fragmented handshake header (frag_off=0, frag_len=total_len).
"""
import os, struct, sys

def make_client_hello_body():
    version        = b"\xfe\xfd"  # DTLS 1.2
    random_bytes   = b"\x01" * 32
    sid            = b"\x00"      # no session id
    cookie         = b"\x00"      # no cookie (first flight)
    ciphersuites   = (
        b"\xc0\x2b" b"\xc0\x2f" b"\xcc\xa9" b"\xcc\xa8"
        b"\xc0\x09" b"\xc0\x13" b"\xc0\x0a" b"\xc0\x14"
        b"\x00\x9c" b"\x00\x2f" b"\x00\x35"
    )
    cs_len         = struct.pack("!H", len(ciphersuites))
    compression    = b"\x01\x00"  # null only
    header = version + random_bytes + sid + cookie + cs_len + ciphersuites + compression
    ext_size      = 1413 - len(header) - 2
    assert ext_size > 4, f"ext_size={ext_size}"
    pad_payload    = ext_size - 4
    extensions     = struct.pack("!HH", 0x0015, pad_payload) + (b"\x00" * pad_payload)
    body           = header + struct.pack("!H", ext_size) + extensions
    assert len(body) == 1413, f"body={len(body)}"
    return body

def build_record(body, seq=0):
    """Wrap a ClientHello handshake body in a DTLS record (non-fragmented)."""
    # Handshake header for a NON-fragmented message:
    #   msg_type(1) + msg_len(3) + msg_seq(2) + frag_off(3) + frag_len(3)
    total = len(body)
    hs = struct.pack("!B", 1)                        # ClientHello type
    hs += struct.pack("!I", total)[1:]               # 3-byte length
    hs += struct.pack("!H", 0)                        # msg_seq = 0
    hs += struct.pack("!I", 0)[1:]                    # frag_off = 0 (non-fragged)
    hs += struct.pack("!I", total)[1:]               # frag_len = total
    hs += body

    # DTLS record header
    rec = struct.pack("!BHH", 22, 0xfeff, 0)          # Handshake, DTLS 1.2, epoch 0
    rec += struct.pack("!Q", seq)[2:]                 # 6-byte sequence
    rec += struct.pack("!H", len(hs))                 # payload length
    rec += hs
    return rec

if __name__ == "__main__":
    outdir = sys.argv[1] if len(sys.argv) > 1 else "build/e2e/replay"
    os.makedirs(outdir, exist_ok=True)
    body = make_client_hello_body()
    record = build_record(body)
    path = os.path.join(outdir, "syn_mono.bin")
    with open(path, "wb") as f:
        f.write(record)
    print(f"wrote {len(record)}B single-record ClientHello (body={len(body)}B) to {path}")
