#!/usr/bin/env python3
"""
generate_synthetic_client_hello.py

Produce a binary ClientHello that mimics Chrome 117's wire shape
(1413 bytes, two DTLS records, ~14 ciphersuites, big extensions
block) but WITHOUT the DTLS 1.3 artefacts (`supported_versions`
extension and the 0x130x TLS-1.3 ciphersuites) that trigger the
known wolfSSL 5.9.2 DTLS-1.2 server parse-error.

Output:
  syn_client_hello_record0.bin   (1200 bytes; the first 1187-byte payload)
  syn_client_hello_record1.bin   (263 bytes;  the second 250-byte payload)
"""
import os
import struct
import sys

def make_client_hello_body():
    # version (DTLS 1.2)
    version = b"\xfe\xfd"
    # random (32 bytes)
    random_bytes = b"\x01" * 32
    # session_id_length = 0
    sid = b"\x00"
    # cookie_length = 0  (first ClientHello flight; no cookie yet)
    cookie = b"\x00"
    # 14 ciphersuites, all DTLS 1.2 ECDHE-ECDSA / ECDHE-RSA / RSA — NO 0x130x
    ciphersuites = (
        b"\xc0\x2b"  # TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256
        b"\xc0\x2f"  # TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256
        b"\xcc\xa9"  # TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256
        b"\xcc\xa8"  # TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256
        b"\xc0\x09"  # TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA
        b"\xc0\x13"  # TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA
        b"\xc0\x0a"  # TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA
        b"\xc0\x14"  # TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA
        b"\x00\x9c"  # TLS_RSA_WITH_AES_128_GCM_SHA256
        b"\x00\x2f"  # TLS_RSA_WITH_AES_128_CBC_SHA
        b"\x00\x35"  # TLS_RSA_WITH_AES_256_CBC_SHA
    )
    cs_len = struct.pack("!H", len(ciphersuites))
    # compression: null
    compression = b"\x01\x00"
    # extensions — we mimic the size of Chrome's blob (~1343 bytes) by
    # padding with a single huge "grease"-style extension so the total
    # body lands at 1413 bytes (Chrome's actual length).  We use a
    # well-known extension id (0x00ff = "renegotiation_info", empty)
    # repeated as a single `padding` extension via a synthetic 0xff01
    # (used by Chrome for padding).  To stay within a known extension
    # namespace, we use `0x0015 padding` (RFC 7685) which is harmless
    # for the server.
    # Build the body so that total = 1413.
    header_so_far = (
        version + random_bytes + sid + cookie +
        cs_len + ciphersuites + compression
    )
    # extensions length (2 bytes) + extensions
    # total body = header_so_far + 2 (ext_len) + N (ext) = 1413
    # => N = 1413 - len(header_so_far) - 2
    ext_size = 1413 - len(header_so_far) - 2
    if ext_size < 0:
        sys.exit("FATAL: header bigger than 1413")
    # 4-byte extension header (type 0x0015, length) + data.
    # We need ext_size bytes total.  Use one padding extension.
    if ext_size < 4:
        sys.exit("FATAL: ext_size < 4")
    pad_payload = ext_size - 4
    extensions = struct.pack("!HH", 0x0015, pad_payload) + (b"\x00" * pad_payload)
    assert len(extensions) == ext_size
    body = header_so_far + struct.pack("!H", ext_size) + extensions
    assert len(body) == 1413, f"body is {len(body)} bytes, expected 1413"
    return body

def split_into_records(body, mtu=1200):
    # Wrap a Handshake fragment into DTLS records.  Each record is
    # 13-byte header (ct=22, ver=0xfeff, epoch=0, seq=N, length=L) +
    # payload.  We split the Handshake message into 2 fragments of
    # ~1100 + ~300 bytes to mimic Chrome's real-world fragmentation.
    # The Handshake header (12 bytes) is repeated in each fragment.
    hs_header = struct.pack("!B", 1)  # type 1 = ClientHello
    hs_header += struct.pack("!I", len(body))[1:]  # 3-byte length
    hs_header += struct.pack("!H", 0)  # msg_seq = 0
    # Pick a fragment boundary inside the body that mimics Chrome's
    # 1175 / 238 split (which targets the 1200-byte UDP MTU).
    first_len = 1107  # body bytes in fragment 0
    frag0_body = body[:first_len]
    frag1_body = body[first_len:]
    assert len(frag0_body) + len(frag1_body) == len(body)
    # Build the two Handshake fragments.
    frag0 = (
        struct.pack("!B", 1) +
        struct.pack("!I", len(body))[1:] +
        struct.pack("!H", 0) +               # msg_seq 0
        struct.pack("!I", 0)[1:] +            # frag_off 0
        struct.pack("!I", len(frag0_body))[1:] +  # frag_len
        frag0_body
    )
    frag1 = (
        struct.pack("!B", 1) +
        struct.pack("!I", len(body))[1:] +
        struct.pack("!H", 0) +
        struct.pack("!I", len(frag0_body))[1:] +
        struct.pack("!I", len(frag1_body))[1:] +
        frag1_body
    )
    # Wrap each in a DTLS record (13-byte header).
    def wrap(payload, seq):
        hdr = struct.pack("!BHH", 22, 0xfeff, 0)  # ct=22, ver=0xfeff, epoch=0
        hdr += struct.pack("!Q", seq)[2:]          # 6-byte seq
        hdr += struct.pack("!H", len(payload))     # length
        return hdr + payload
    return wrap(frag0, 0), wrap(frag1, 1)

if __name__ == "__main__":
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(outdir, exist_ok=True)
    body = make_client_hello_body()
    r0, r1 = split_into_records(body)
    open(os.path.join(outdir, "syn_ch_0.bin"), "wb").write(r0)
    open(os.path.join(outdir, "syn_ch_1.bin"), "wb").write(r1)
    print(f"wrote {len(r0)}B fragment0, {len(r1)}B fragment1, total body={len(body)}B")
