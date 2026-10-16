#!/usr/bin/env python3
"""Extract the 2 Chrome ClientHello fragments from the e2e trace
and dump them as 2 raw .bin files (one per record) so a C++ unit
test can replay them against a NimRTC DtlsSessionWolfSSL in Server mode.

Record 0 (seq=0, 1200 bytes): the first 1175-byte fragment of the
1413-byte ClientHello message.
Record 1 (seq=1, 263 bytes): the second 238-byte fragment.

Together they form the full ClientHello Chrome 117 sends at the
start of every DTLS handshake.
"""
import re
import sys
from pathlib import Path

src = Path(sys.argv[1])
outdir = Path(sys.argv[2])
outdir.mkdir(parents=True, exist_ok=True)

text = src.read_text(encoding="utf-8", errors="replace")

# Pull every "feed_inbound <N>B hex=..." line, in order.  These are
# the actual UDP datagrams the engine fed into the DTLS layer.
pattern = re.compile(r"feed_inbound (\d+)B hex=([0-9a-f ]+)")
records = []
for m in pattern.finditer(text):
    n = int(m.group(1))
    hex_str = m.group(2).replace(" ", "")
    if hex_str.endswith("..."):
        # The C++ trace truncates to 64 bytes; only the first 64 bytes
        # are present, which is not enough to drive wolfSSL.  Fail loudly.
        sys.exit(f"ERROR: trace is truncated at line {m.group(0)!r}; cannot reconstruct full ClientHello.")
    if len(hex_str) // 2 != n:
        sys.exit(f"ERROR: feed_inbound line declares {n}B but hex string is {len(hex_str)//2}B")
    records.append(bytes.fromhex(hex_str))

print(f"[INFO] parsed {len(records)} feed_inbound records from trace")
for i, r in enumerate(records):
    (outdir / f"chrome_datagram_{i:02d}.bin").write_bytes(r)
    ct = r[0]
    epoch = (r[3] << 8) | r[4]
    seq = int.from_bytes(r[5:11], "big")
    length = (r[11] << 8) | r[12]
    print(f"  [{i:02d}] {len(r)}B  ct={ct} epoch={epoch} seq={seq} declared_len={length}")

# Sanity: the first record should be a Handshake fragment (ct=22) of
# the ClientHello, and the second its continuation.
if len(records) < 2:
    sys.exit("ERROR: need at least 2 records to reconstruct a fragmented ClientHello")

if records[0][0] != 22 or records[1][0] != 22:
    sys.exit("ERROR: first 2 records are not DTLS Handshake records")

# Parse the Handshake header from record 0:  type(1) length(3) msg_seq(2) frag_off(3) frag_len(3) = 12 bytes
hs_type_0   = records[0][13]
hs_length_0 = int.from_bytes(records[0][14:17], "big")
frag_off_0  = int.from_bytes(records[0][17:20], "big")
frag_len_0  = int.from_bytes(records[0][20:23], "big")
print(f"  [hs0] type={hs_type_0} length={hs_length_0} frag_off={frag_off_0} frag_len={frag_len_0}")

hs_type_1   = records[1][13]
hs_length_1 = int.from_bytes(records[1][14:17], "big")
frag_off_1  = int.from_bytes(records[1][17:20], "big")
frag_len_1  = int.from_bytes(records[1][20:23], "big")
print(f"  [hs1] type={hs_type_1} length={hs_length_1} frag_off={frag_off_1} frag_len={frag_len_1}")

if hs_type_0 != 1 or hs_type_1 != 1:
    sys.exit("ERROR: first 2 records do not carry a ClientHello (type 1)")

if hs_length_0 != hs_length_1:
    sys.exit("ERROR: Handshake length differs between fragments (mismatched ClientHello)")
if frag_off_0 != 0 or frag_off_1 != frag_len_0:
    sys.exit("ERROR: fragments are not contiguous (off=%d, len=%d, next_off=%d)" %
             (frag_off_0, frag_len_0, frag_off_1))
if frag_off_0 + frag_len_1 != hs_length_0:
    sys.exit("ERROR: fragments do not cover the full Handshake message")

print(f"[OK] Chrome ClientHello fully reconstructed: {hs_length_0} bytes across {len(records)} records")
