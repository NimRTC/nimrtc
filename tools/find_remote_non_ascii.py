#!/usr/bin/env python3
data = open('/tmp/registry_remote.hpp','rb').read()
lines = data.split(b'\n')
for lineno, line in enumerate(lines, 1):
    for i, b in enumerate(line):
        if b > 127:
            ctx = line[max(0,i-5):min(len(line),i+30)]
            print(f"Line {lineno} col {i+1}: byte=0x{b:02x}  ctx={ctx!r}")
