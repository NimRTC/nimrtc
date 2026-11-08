# tools/

Development and release utility scripts (not part of the NimRTC library itself).

## Directory structure

```
tools/
├── *.py            # Utility scripts (lint, release, codegen, CI helpers)
├── *.sh / *.cmd    # Thin wrappers for platform-specific commands
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

## Documentation

| Script | Purpose |
|--------|---------|
| `check_md_links.py` | Verify relative Markdown links and `#anchor` targets in every non-vendored `.md` file. Wired into CI as the `docs / markdown links` job. |

```
python tools/check_md_links.py            # whole repo
python tools/check_md_links.py README.md  # specific files
```

Exit code is non-zero when a link points at a missing file or an anchor
that no heading produces.

## Release evidence (the seven-day soak)

The v1.1 release gate needs one evidence record per platform per day.
The operating procedure is `docs/plan/soak-runbook.md`.

| Script | Purpose |
|--------|---------|
| `run_soak.py` | Runs the acceptance cases and records one machine-readable record per platform per day under `build/soak/`. `--summary` prints the streak it can prove, `--require` turns that summary into a gate (exit 1 unless every platform has 7 consecutive green days), `--self-test` pins the honesty rules. |
| `fetch_soak_evidence.py` | Pulls the CI-produced records back out of the workflow artifacts into `build/soak/`. Idempotent, and fails when an artifact cannot be read. |

```
python tools/run_soak.py --summary --require      # read the window
python tools/fetch_soak_evidence.py --days 21      # fold the CI half in
python tools/fetch_soak_evidence.py --self-test
```

## Scripts planned

| Script | Phase | Purpose |
|--------|-------|---------|
| `check_prerequisites.py` | **P0 (done)** | Check CMake/compiler/Ninja/Python versions before build |
| `scripts/codegen.py` | P1 | Generate codec registration tables from SDP media types |
| `scripts/release.py` | P1 | Bump version, tag, build release artifacts |
| `scripts/lint.py` | P0 | Wrapper around clang-tidy, cmake-lint |
| `ci/package.sh` | P1 | Build distribution packages (.tar.gz, .zip) |
