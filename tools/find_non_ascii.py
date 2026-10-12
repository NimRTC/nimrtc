#!/usr/bin/env python3
"""Find non-ASCII bytes in registry.hpp."""
data = open('src/core/include/nimrtc/core/registry.hpp', 'rb').read()
lines = data.split(b'\n')
hits = 0
for lineno, line in enumerate(lines, 1):
    for i, b in enumerate(line):
        if b > 127:
            ctx = line[max(0,i-2):min(len(line),i+10)]
            print(f"Line {lineno} col {i+1}: byte=0x{b:02x}  ctx={ctx!r}")
            hits += 1
            if hits > 30:
                break
    if hits > 30:
        break
print(f"total: {hits}")
