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

## Build scripts

| Script | Platform | Purpose |
|--------|----------|---------|
| `scripts/build.bat` | Windows | Interactive build: configure + build + test (MSVC preset) |
| `scripts/build.sh` | Linux/macOS | Interactive build: configure + build + test (GCC/Clang preset) |

Both scripts:
- Auto-detect the installed compiler
- Choose Debug/Release/asan preset interactively (or via flags)
- Run tests after build (ask confirmation, skippable)
- Support `--rebuild`, `--configure`, `--build`, `--test` flags

```
.\scripts\build.bat --rebuild --release   # Windows: clean + Release
bash scripts/build.sh --rebuild --preset=release  # Linux/macOS
```

## Scripts planned

| Script | Phase | Purpose |
|--------|-------|---------|
| `check_prerequisites.py` | **P0 (done)** | Check CMake/compiler/Ninja/Python versions before build |
| `scripts/codegen.py` | P1 | Generate codec registration tables from SDP media types |
| `scripts/release.py` | P1 | Bump version, tag, build release artifacts |
| `scripts/lint.py` | P0 | Wrapper around clang-tidy, cmake-lint |
| `ci/package.sh` | P1 | Build distribution packages (.tar.gz, .zip) |
