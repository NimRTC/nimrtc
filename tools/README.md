# tools/

Development and release utility scripts (not part of the NimRTC library itself).

## Directory structure

```
tools/
├── cmake/          # CMake helper scripts used at configure time
├── scripts/        # Shell / Python utilities (lint, release, codegen)
└── ci/             # CI-specific scripts (packaging, signing)
```

## Conventions

- All scripts must have a shebang (`#!/bin/bash` or `#!/usr/bin/env python3`)
- Scripts that modify source must be idempotent (safe to re-run)
- No credentials or secrets — use environment variables only
- Document usage in a `--help` block or a top-level comment

## Scripts planned

| Script | Phase | Purpose |
|--------|-------|---------|
| `scripts/codegen.py` | P1 | Generate codec registration tables from SDP media types |
| `scripts/release.py` | P1 | Bump version, tag, build release artifacts |
| `scripts/lint.py` | P0 | Wrapper around clang-tidy, cmake-lint |
| `ci/package.sh` | P1 | Build distribution packages (.tar.gz, .zip) |
