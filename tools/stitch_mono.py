#!/usr/bin/env python3
"""Build a single-DTLS-record version of the synthetic ClientHello."""
import os, struct

with open("build/e2e/replay/syn_ch_0.bin","rb") as f: r0=f.read()
with open("build/e2e/replay/syn_ch_1.bin","rb") as f: r1=f.read()

frag0 = r0[13:]  # 12-byte hs hdr + body_frag0
frag1 = r1[13:]  # 12-byte hs hdr + body_frag1

body0_len = len(frag0) - 12
body1_len = len(frag1) - 12
print(f"frag0: {len(frag0)}B total, body={body0_len}B")
print(f"frag1: {len(frag1)}B total, body={body1_len}B")

hs_hdr = frag0[:12]
nul = b"\x00"
msg_type  = hs_hdr[0]
msg_len   = struct.unpack("!I", nul + hs_hdr[1:4])[0]
msg_seq   = struct.unpack("!H", hs_hdr[4:6])[0]
frag_off  = struct.unpack("!I", nul + hs_hdr[6:9])[0]
frag_len  = struct.unpack("!I", nul + hs_hdr[9:12])[0]
print(f"hs_hdr: type={msg_type} msg_len={msg_len} msg_seq={msg_seq} frag_off={frag_off} frag_len={frag_len}")

body_frag0 = frag0[12:]
body_frag1 = frag1[12:]
combined_body = hs_hdr + body_frag0 + body_frag1
total_len = len(combined_body)
print(f"combined body: {total_len}B (hdr 12 + frag0 {body0_len} + frag1 {body1_len})")

single_record = r0[:11] + struct.pack("!H", total_len) + combined_body
print(f"single record: {len(single_record)}B (rec_hdr 13 + body {total_len})")

os.makedirs("build/e2e/replay", exist_ok=True)
with open("build/e2e/replay/syn_mono.bin","wb") as f: f.write(single_record)
print("wrote build/e2e/replay/syn_mono.bin")
