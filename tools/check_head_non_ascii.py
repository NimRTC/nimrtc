#!/usr/bin/env python3
import subprocess
res = subprocess.run(['git', 'show', 'HEAD:src/core/include/nimrtc/core/registry.hpp'],
                     capture_output=True, cwd='.')
data = res.stdout
lines = data.split(b'\n')
for lineno, line in enumerate(lines, 1):
    for i, b in enumerate(line):
        if b > 127:
            ctx = line[max(0,i-10):min(len(line),i+20)]
            print(f"Line {lineno} col {i+1}: byte=0x{b:02x}  ctx={ctx!r}")
