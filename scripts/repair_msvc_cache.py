#!/usr/bin/env python3
"""Repair a CMake cache that a polluted MSVC environment poisoned.

The MSVC environment variables CL / _CL_ / CLFLAGS are prepended to
every compiler command line.  When one of them carries a language
standard option, two things break:

  * the compile step fails with

        cl : Command line error D8016: '/std:c++20' and '/std:c11'
        options are incompatible

    because the vendored libopus compiles as C11 while CMake passes
    /std:c++20 for the C++ sources;

  * the *configure* step fails its compiler probes, because the
    try_compile inherits the same environment.

The second failure is the sticky one.  CMake records a failed probe as
an empty value in the cache, and an empty value is never recomputed:
the next configure reuses it.  The vendored libopus then concludes
that the compiler supports none of its three stack allocation modes --
no variable-length arrays, no alloca and no pseudostack -- and the
build dies much later with

    fatal error C1189: #error: "Opus requires one of VAR_ARRAYS,
    USE_ALLOCA, or NONTHREADSAFE_PSEUDOSTACK be defined"

Clearing the environment variables stops the *next* configure from
poisoning the cache, but it does not undo a cache that is already
poisoned.  This tool does that undo: it drops the probe results that
a polluted environment produced, so the next configure re-runs them
against a clean compiler.

Note that a probe result of FALSE is *not* damage.  MSVC's C mode has
no variable-length arrays, so COMPILE_VLA_SUPPORTED=FALSE is the
honest answer there and must be left alone.  The signature of the
damage is an *empty* probe value: that is how CMake records a probe
that did not compile.

Usage:
    python scripts/repair_msvc_cache.py [build_dir ...]

With no argument it repairs the build directories the project's CMake
presets use.  Exits 0 when nothing had to be done, 1 when a cache was
repaired, and 2 on error.
"""

from __future__ import annotations

import sys
from pathlib import Path

# The probe results that a polluted environment can poison.  Both are
# INTERNAL cache entries: CMake computed them once and will reuse the
# stored answer forever.
#
#   COMPILE_VLA_SUPPORTED  -- libopus' variable-length-array probe
#   USE_ALLOCA_SUPPORTED   -- libopus' alloca probe
PROBE_KEYS = ("COMPILE_VLA_SUPPORTED", "USE_ALLOCA_SUPPORTED")

# The options that select libopus' stack allocation mode.  They are
# derived from the probes above by cmake_dependent_option(), so they do
# not have to be dropped -- the next configure recomputes them.
ALLOCATION_OPTIONS = (
    "OPUS_VAR_ARRAYS",
    "OPUS_USE_ALLOCA",
    "OPUS_NONTHREADSAFE_PSEUDOSTACK",
)


def read_cache(path: Path) -> list[str]:
    try:
        return path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as exc:
        raise SystemExit("cannot read %s: %s" % (path, exc)) from None


def cache_value(lines: list[str], key: str) -> str | None:
    """The cached value of `key`, or None when it is not cached.

    A cache line has the form "KEY:TYPE=VALUE", so the value is what
    follows the "=" and the type in between is not part of it.
    """
    prefix = key + ":"
    for line in lines:
        if line.startswith(prefix):
            _, _, value = line.partition("=")
            return value.strip()
    return None


def failed_probes(lines: list[str]) -> list[str]:
    """The probe keys whose cached value records a failed probe.

    An empty value is how CMake stores a probe that did not compile.
    A value of FALSE or 0 is a *successful* probe with a negative
    answer -- MSVC's C mode genuinely has no variable-length arrays --
    and must not be treated as damage.
    """
    failed = []
    for key in PROBE_KEYS:
        value = cache_value(lines, key)
        if value is not None and value == "":
            failed.append(key)
    return failed


def repair(path: Path) -> bool:
    """Drop the poisoned probe results from one cache.

    Returns True when something was changed.
    """
    raw = path.read_text(encoding="utf-8", errors="replace")
    lines = raw.splitlines()
    failed = failed_probes(lines)
    if not failed:
        return False

    # Only the probe results are dropped.  The options that select the
    # allocation mode are derived from them by cmake_dependent_option(),
    # which recomputes them on the next configure -- so they are left
    # alone.  Removing them here would leave the cache inconsistent for
    # the duration of the configure run, and CMake refuses to parse a
    # cache whose entries do not line up.
    drop = set(failed)
    kept = []
    for line in lines:
        key = line.split(":", 1)[0]
        if key in drop:
            continue
        kept.append(line)

    # The cache is a "KEY:TYPE=VALUE" list; a blank line or a stray
    # character makes CMake refuse to parse it.  Join with the line
    # ending the file already uses, and keep exactly one trailing
    # newline.
    newline = "\r\n" if "\r\n" in raw else "\n"
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(newline.join(kept) + newline)
    print("  %s: dropped %s -- the next configure re-runs them"
          % (path, ", ".join(failed)))
    print("      against a clean compiler.")
    return True


def default_build_dirs(root: Path) -> list[Path]:
    """The build directories the project's CMake presets use."""
    candidates = [root / "build", root / "build" / "win-msvc"]
    return [c for c in candidates if (c / "CMakeCache.txt").is_file()]


def main(argv: list[str]) -> int:
    root = Path(__file__).resolve().parent.parent
    targets = [Path(a) for a in argv[1:]] if len(argv) > 1 else default_build_dirs(root)

    if not targets:
        print("No build directory with a CMakeCache.txt was found; nothing to repair.")
        return 0

    repaired = 0
    for target in targets:
        cache = target / "CMakeCache.txt"
        if not cache.is_file():
            print("  %s: no CMakeCache.txt -- skipped." % target)
            continue
        print("Checking %s ..." % cache)
        if repair(cache):
            repaired += 1

    if repaired:
        print(
            "\n%d cache(s) repaired.  Re-run the configure step; the compiler "
            "probes will be recomputed against a clean environment." % repaired
        )
        return 1
    print("\nNothing to repair.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))





