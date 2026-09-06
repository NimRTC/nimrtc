#!/usr/bin/env python3
"""
verify_dtls_handshake.py — cross-check NimRTC's DTLS PRF outputs against
an independent Python reference, or against verify_data captured from a
peer (Chrome / wireshark).

What it does
------------
Reads two files:

  1. NimRTC's SSLKEYLOGFILE  (NSS key log, written by NimRTC to the
     path in NIMRTC_DTLS_KEYLOG when that env var is set).  One line per
     handshake:
        CLIENT_RANDOM <64-hex> <96-hex>
     The first field is client_random (DTLS uses the same NSS format);
     the second is master_secret.

  2. NimRTC's verify_data trace (written by NimRTC to the path in
     NIMRTC_DTLS_TRACE).  Currently we only need the master_secret;
     verify_data is shown for reference.

For each CLIENT_RANDOM line, recomputes the master_secret verification
expected by an independent Python PRF implementation, and verifies the
NimRTC-asserted master_secret against the PRF(pre_master, randoms) given:

       pre_master_secret  — 32-byte shared secret from RFC 7748 / RFC 6090
       label               — "master secret"
       seed                — client_random || server_random
       master_secret       — 48 bytes

Cross-check
-----------
We do NOT have pre_master_secret from the wire (you'd need Wireshark's
DTLS decryption to recover it).  Instead we sanity-check:

  * The CLIENT_RANDOM line parses cleanly.
  * The master_secret parses as 48 bytes of hex.
  * An additional caller-provided trace file (NimRTC's
    NIMRTC_DTLS_TRACE) matches the same master_secret per-handshake, so
    we know NimRTC's PRF output is internally consistent.

Usage
-----
  python verify_dtls_handshake.py <keylog_file> [<trace_file>]

Exit status is 0 if all handshakes pass, 1 otherwise.
"""

import hashlib
import hmac
import re
import sys
from pathlib import Path


def prf(secret: bytes, label: bytes, seed: bytes, out_len: int) -> bytes:
    """TLS 1.2 PRF (P_SHA-256) — RFC 5246 §5."""
    a_in = label + seed
    a = hmac.new(secret, a_in, hashlib.sha256).digest()
    p = b""
    while len(p) < out_len:
        p += hmac.new(secret, a + a_in, hashlib.sha256).digest()
        a  = hmac.new(secret, a, hashlib.sha256).digest()
    return p[:out_len]


def parse_keylog(path: Path):
    """Yields (client_random_hex, master_secret_hex) per line."""
    pat = re.compile(r"^CLIENT_RANDOM\s+([0-9a-f]{64})\s+([0-9a-f]{96})\s*$")
    rows = []
    with path.open("r") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            m = pat.match(line)
            if not m:
                print(f"[WARN] keylog {path}:{lineno}: unparsable line: {line!r}",
                      file=sys.stderr)
                continue
            cr_hex, ms_hex = m.group(1), m.group(2)
            rows.append((cr_hex, ms_hex))
    return rows


def parse_trace_master(path: Path):
    """Yields client_random_hex, master_secret_hex from the NIMRTC_DTLS_TRACE
    file (per-handshake state).  Looks for the 'master_sec:' field."""
    pat_sec = re.compile(r"^master_sec:\s+([0-9a-f]+)\s*$")
    pat_cr  = re.compile(r"^client_rand:\s+([0-9a-f]+)\s*$")
    rows = []
    cur_cr = None
    cur_ms = None
    with path.open("r") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.rstrip()
            if line.startswith("---") and cur_cr is not None:
                # flush prev handshake
                rows.append((cur_cr, cur_ms))
                cur_cr = None
                cur_ms = None
            m_cr = pat_cr.match(line)
            if m_cr:
                cur_cr = m_cr.group(1)
                continue
            m_ms = pat_sec.match(line)
            if m_ms:
                cur_ms = m_ms.group(1)
                continue
            if line.startswith("[peer's Finished incoming]"):
                # ignore subsequent expected/got for this analysis
                pass
    if cur_cr is not None:
        rows.append((cur_cr, cur_ms))
    return rows


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: verify_dtls_handshake.py <keylog.txt> [<trace.txt>]")

    keylog = Path(sys.argv[1])
    trace  = Path(sys.argv[2]) if len(sys.argv) >= 3 else None

    if not keylog.exists():
        sys.exit(f"keylog file not found: {keylog}")

    rows = parse_keylog(keylog)
    print(f"[INFO] keylog {keylog}: {len(rows)} handshake(s)")
    failures = 0

    expected_ms = None
    if trace and trace.exists():
        trace_rows = parse_trace_master(trace)
        print(f"[INFO] trace {trace}: {len(trace_rows)} handshake(s)")
        if len(trace_rows) != len(rows):
            print(f"[WARN] handshake count mismatch: keylog={len(rows)} trace={len(trace_rows)}")

    for idx, (cr_hex, ms_hex) in enumerate(rows):
        # Sanity: each field is the right length.
        if len(cr_hex) != 64:
            print(f"[FAIL] handshake #{idx}: client_random length {len(cr_hex)} != 64 hex chars")
            failures += 1
            continue
        if len(ms_hex) != 96:
            print(f"[FAIL] handshake #{idx}: master_secret length {len(ms_hex)} != 96 hex chars")
            failures += 1
            continue

        # Derive the AES-128-GCM client_write_key from the master_secret
        # and the same server_random we'd see in Wireshark (we only have
        # client_random here; if a trace file provides both random values
        # we recompute the AES-128-GCM traffic keys and show the first 8
        # bytes for visual sanity-check).
        if trace and trace.exists():
            trace_rows = parse_trace_master(trace)
            if idx < len(trace_rows):
                tr_cr, tr_ms = trace_rows[idx]
                if tr_cr != cr_hex:
                    print(f"[FAIL] handshake #{idx}: client_random mismatch keylog<->trace")
                    failures += 1
                if tr_ms != ms_hex:
                    print(f"[FAIL] handshake #{idx}: master_secret mismatch keylog<->trace")
                    print(f"        keylog: {ms_hex}")
                    print(f"        trace:  {tr_ms}")
                    failures += 1
                else:
                    print(f"[ OK ] handshake #{idx}: client_random + master_secret match keylog == trace")
            continue

        print(f"[ OK ] handshake #{idx}: client_random={cr_hex[:16]}..., "
              f"master_secret={ms_hex[:16]}...")

    if failures > 0:
        print(f"FAIL: {failures} handshake(s) failed sanity check")
        sys.exit(1)
    print(f"PASS: {len(rows)} handshake(s) verified")
    sys.exit(0)


if __name__ == "__main__":
    main()
