#!/usr/bin/env python3
import subprocess, os
# Check working dir
local = 'src/core/include/nimrtc/core/registry.hpp'
data = open(local, 'rb').read()
print(f"LOCAL {local}: {len(data)} bytes, {sum(1 for b in data if b>127)} non-ascii")
print(f"  first 16 bytes: {data[:16]!r}")

# Check what HEAD has in the local repo
res = subprocess.run(['git', 'show', 'HEAD:src/core/include/nimrtc/core/registry.hpp'],
                     capture_output=True, cwd='.')
print(f"HEAD same path: {len(res.stdout)} bytes, {sum(1 for b in res.stdout if b>127)} non-ascii")
print(f"  first 16 bytes: {res.stdout[:16]!r}")
