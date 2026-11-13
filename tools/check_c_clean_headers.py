#!/usr/bin/env python3
"""Check that every ``.h`` under ``src/**/include/`` is C-clean.

Why this exists
---------------

ADR-026 turned the scheduler's C entry points into a real ``extern "C"``
surface.  The header that carries it, ``nimrtc/sched/sched_c.h``, is
C11 with plain C types -- which is the whole point: a C host has to be
able to ``#include`` it and *name* the types in it.

The convention that makes that guarantee checkable is the file
extension:

* ``.h``  -- under ``src/**/include/``, so: consumable from C.
* ``.hpp`` -- C++ only, and no C host is expected to include it.

Without a guard, the convention is only a convention.  Adding one
``#include <string>`` to a ``.h`` -- or one ``namespace`` block -- turns
a public C header into something no C compiler will accept, and the
breakage is invisible until a C consumer tries to build against it.
By then the header is installed and the ABI is public.

So this check compiles every ``.h`` under ``src/**/include/`` as C11,
in isolation, with a real C compiler.  A header that a C compiler
rejects is a header that has stopped being a C header.

Scope
-----

* Only ``.h`` files under an ``include/`` directory of a first-party
  module are checked.  ``src/third_party/`` is excluded: vendored code
  carries its own conventions and its own build.
* ``.hpp`` files are not checked.  They are C++ by definition.
* The check is per-header and in isolation: a header that only compiles
  because some *other* header was included first is exactly the header
  that will break a C consumer who includes it first.

Usage
-----

    python tools/check_c_clean_headers.py [--verbose] [--self-test]
                                            [--require-compiler]

``--self-test`` builds a synthetic module tree containing one C-clean
header and one that uses a C++-only construct, then asserts the checker
flags exactly the latter.  A guard that cannot fail is not a guard.

Exit codes: 0 = clean, 1 = a violation (or a missing compiler under
``--require-compiler``), 2 = a usage error.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# The C standard the public headers promise.  C11 is what
# `sched_c.h` documents and what the `test_sched_c_abi` target builds.
C_STANDARD = "c11"

# Warnings that would make a *correct* C header look broken.  We are
# checking compilability, not style, so the warning set is deliberately
# narrow: `-Wall -Wextra` catches the C++-isms that slip through (an
# implicit function declaration, an incompatible pointer type), and
# `-Werror` is NOT used because a warning is not a C-cleanliness failure.
#
# MSVC's spelling of the same idea: /Zs is "syntax check only, emit
# nothing", and /TC forces the C compiler even though the file is
# `.c` (which it is, but the flag is explicit rather than incidental).
GCC_FLAGS = ["-std=" + C_STANDARD, "-fsyntax-only", "-Wall", "-Wextra"]
MSVC_FLAGS = ["/nologo", "/Zs", "/std:c11", "/TC"]

# Candidate C compilers, in preference order.  `CC` overrides the list.
# `cl` is last because on Windows it is only on PATH inside a developer
# command prompt -- but when it is there it is the *right* compiler,
# because MSVC is what the Windows build uses.
COMPILER_CANDIDATES = ["cc", "gcc", "clang", "cl"]


class Violation:
    """One header that a C compiler rejected."""

    def __init__(self, header: Path, diagnostic: str) -> None:
        self.header = header
        self.diagnostic = diagnostic

    def describe(self, root: Path) -> str:
        try:
            rel = self.header.relative_to(root)
        except ValueError:
            rel = self.header
        lines = [str(rel)]
        for line in self.diagnostic.strip().splitlines():
            lines.append("    " + line)
        return "\n".join(lines)


def repo_root() -> Path:
    """The repository root -- the directory that holds ``src/``."""
    return Path(__file__).resolve().parent.parent


def find_compiler(explicit: str | None = None) -> str | None:
    """Return the C compiler to use, or None when there is none."""
    candidates: list[str] = []
    if explicit:
        candidates.append(explicit)
    env_cc = os.environ.get("CC")
    if env_cc:
        candidates.append(env_cc)
    candidates.extend(COMPILER_CANDIDATES)

    for cand in candidates:
        resolved = shutil.which(cand.split()[0]) if cand else None
        if resolved:
            return cand
    return None


def _walk_first_party(src: Path):
    """Yield every path under ``src/``, pruning ``third_party``.

    ``src/third_party/`` holds mbedTLS, libjuice, libsrtp and more --
    tens of thousands of files.  Walking it to look for two public
    headers is both slow and pointless: vendored code carries its own
    conventions and its own build.  Pruning the subtree keeps the check
    to a fraction of a second.

    Yields ``(dirpath, dirnames, filenames)`` exactly as ``os.walk``
    does, so the caller can look at directories as well as files.
    """
    for dirpath, dirnames, filenames in os.walk(src):
        dirnames[:] = [d for d in dirnames if d != "third_party"]
        yield Path(dirpath), dirnames, filenames


def public_headers(root: Path) -> list[Path]:
    """Every first-party ``.h`` under an ``include/`` directory.

    ``src/third_party/`` is excluded: vendored code brings its own
    conventions and its own build, and a C-cleanliness rule imposed on
    it would be a rule imposed on code nobody here owns.
    """
    src = root / "src"
    if not src.is_dir():
        return []

    found: list[Path] = []
    for dirpath, _dirnames, filenames in _walk_first_party(src):
        for name in filenames:
            if not name.endswith(".h"):
                continue
            path = dirpath / name
            parts = path.relative_to(src).parts
            if "include" not in parts:
                continue
            found.append(path)
    return sorted(found)


def include_dirs(root: Path) -> list[Path]:
    """Every first-party ``include/`` directory, as an ``-I`` root.

    A public header may include a sibling (``<nimrtc/sched/sched_c.h>``
    from another module's header).  So the include path is the union of
    every module's ``include/`` directory, exactly as the build sees it.
    """
    src = root / "src"
    if not src.is_dir():
        return []

    dirs = []
    for dirpath, dirnames, _filenames in _walk_first_party(src):
        for name in dirnames:
            if name != "include":
                continue
            path = dirpath / name
            if path.is_dir():
                dirs.append(path)
    return sorted(dirs)


def compiler_flags(compiler: str) -> list[str]:
    """The syntax-check flags for the given compiler.

    The two families spell the same check differently, and mixing them
    produces a confusing diagnostic ("unknown option") rather than the
    real answer.
    """
    base = os.path.basename(compiler).lower()
    if base.startswith("cl"):
        return list(MSVC_FLAGS)
    return list(GCC_FLAGS)


def include_root_of(header: Path) -> Path | None:
    """The `include/` directory a public header lives under, if any.

    A public header is included by the path *below* its module's
    ``include/`` root: ``nimrtc/sched/sched_c.h``, not
    ``src/modules/sched/include/nimrtc/sched/sched_c.h``.  The root is
    the last ``include`` component in the header's path.
    """
    parts = header.parts
    if "include" not in parts:
        return None
    idx = len(parts) - 1 - parts[::-1].index("include")
    return Path(*parts[: idx + 1])


def compile_header(
    compiler: str,
    header: Path,
    inc_dirs: list[Path],
    root: Path,
) -> tuple[bool, str]:
    """Compile one header as C11, in isolation.

    Returns ``(ok, diagnostic)``.  ``diagnostic`` is the compiler's
    output when the compile failed, and empty when it succeeded.
    """
    try:
        rel = header.relative_to(root).as_posix()
    except ValueError:
        rel = header.as_posix()

    # A C host includes a public header by its *public* name -- the path
    # below its module's `include/` root -- not by its path in the source
    # tree.  Probing with the source-tree path would test the file
    # system rather than the include contract, so the probe uses the
    # public spelling and the module's `include/` directory is on the
    # search path.
    include_root = include_root_of(header)
    if include_root is not None:
        try:
            public_name = header.relative_to(include_root).as_posix()
        except ValueError:
            public_name = rel
    else:
        public_name = rel

    with tempfile.TemporaryDirectory(prefix="nimrtc-c-clean-") as tmp:
        tu = Path(tmp) / "probe.c"
        # The probe includes the header and nothing else: no other
        # project header may be in scope, because the guarantee is that
        # a C host can include this header *first*.
        tu.write_text('#include <%s>\nint main(void) { return 0; }\n' % public_name,
                      encoding="utf-8")

        cmd = [compiler] + compiler_flags(compiler)
        for inc in inc_dirs:
            cmd += ["/I" if os.path.basename(compiler).lower().startswith("cl")
                    else "-I", str(inc)]
        cmd += [str(tu)]

        proc = subprocess.run(
            cmd, cwd=str(root), capture_output=True, text=True,
            encoding="utf-8", errors="replace",
        )
        if proc.returncode == 0:
            return True, ""
        return False, (proc.stdout or "") + (proc.stderr or "")


def check_tree(
    root: Path,
    compiler: str,
    verbose: bool = False,
) -> list[Violation]:
    """Check every public ``.h`` under ``root``.  Returns the violations."""
    headers = public_headers(root)
    inc_dirs = include_dirs(root)
    violations: list[Violation] = []

    for header in headers:
        ok, diagnostic = compile_header(compiler, header, inc_dirs, root)
        if verbose:
            rel = header.relative_to(root) if header.is_relative_to(root) else header
            print("  %-8s %s" % ("ok" if ok else "FAIL", rel))
        if not ok:
            violations.append(Violation(header, diagnostic))
    return violations


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

CLEAN_HEADER = """\
#ifndef PROBE_CLEAN_H
#define PROBE_CLEAN_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ProbeClean ProbeClean;
int probe_clean_take(ProbeClean* handle, uint8_t* dst, size_t len);
#ifdef __cplusplus
}
#endif
#endif /* PROBE_CLEAN_H */
"""

DIRTY_HEADER = """\
#ifndef PROBE_DIRTY_H
#define PROBE_DIRTY_H
#include <string>
namespace probe { struct Dirty { std::string name; }; }
#endif /* PROBE_DIRTY_H */
"""


def run_self_test(compiler: str | None) -> int:
    """Build a synthetic tree and assert the checker tells the two apart."""
    if compiler is None:
        print("self-test: SKIPPED -- no C compiler found")
        return 0

    with tempfile.TemporaryDirectory(prefix="nimrtc-c-clean-selftest-") as tmp:
        root = Path(tmp)
        inc = root / "src" / "probe" / "include" / "nimrtc" / "probe"
        inc.mkdir(parents=True)
        (inc / "clean.h").write_text(CLEAN_HEADER, encoding="utf-8")
        (inc / "dirty.h").write_text(DIRTY_HEADER, encoding="utf-8")

        headers = public_headers(root)
        if len(headers) != 2:
            print("self-test: FAILED -- expected 2 synthetic headers, found %d"
                  % len(headers))
            return 1

        violations = check_tree(root, compiler, verbose=False)
        failed = {v.header.name for v in violations}

        if "dirty.h" not in failed:
            print("self-test: FAILED -- the C++-only header was not flagged; "
                  "the checker would pass a header no C compiler accepts")
            return 1
        if "clean.h" in failed:
            print("self-test: FAILED -- the C-clean header was flagged; "
                  "the checker is rejecting valid C")
            for violation in violations:
                if violation.header.name == "clean.h":
                    print(violation.describe(root))
            return 1

        # A header that is C++-only but hidden behind a .hpp extension
        # must NOT be flagged -- the extension is the opt-out.
        (inc / "dirty.h").unlink()
        hpp = inc / "ok_because_hpp.hpp"
        hpp.write_text(DIRTY_HEADER, encoding="utf-8")
        violations = check_tree(root, compiler, verbose=False)
        if violations:
            print("self-test: FAILED -- a .hpp was checked; the guard's scope "
                  "is .h only")
            for violation in violations:
                print(violation.describe(root))
            return 1

        # A header outside an include/ directory is not public, so it is
        # not checked either.
        priv = root / "src" / "probe" / "src"
        priv.mkdir(parents=True)
        (priv / "private.h").write_text(DIRTY_HEADER, encoding="utf-8")
        headers = public_headers(root)
        if any(h.name == "private.h" for h in headers):
            print("self-test: FAILED -- a header outside include/ was checked")
            return 1

    print("self-test OK: the C-clean guard distinguishes C headers from "
          "C++ ones")
    return 0


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Check that every .h under src/**/include/ is C-clean.",
    )
    parser.add_argument("--verbose", action="store_true",
                        help="print one line per header checked")
    parser.add_argument("--self-test", action="store_true",
                        help="prove the checker can fail, then exit")
    parser.add_argument("--compiler", default=None,
                        help="the C compiler to use (default: $CC, then cc/gcc/clang)")
    parser.add_argument("--require-compiler", action="store_true",
                        help="fail instead of skipping when no C compiler is found")
    args = parser.parse_args(argv)

    compiler = find_compiler(args.compiler)

    if args.self_test:
        return run_self_test(compiler)

    root = repo_root()
    headers = public_headers(root)

    if compiler is None:
        message = ("no C compiler found (looked for $CC, cc, gcc, clang); "
                   "cannot verify that the public headers are C-clean")
        if args.require_compiler:
            print("C-clean check: FAILED -- " + message)
            return 1
        print("C-clean check: SKIPPED -- " + message)
        return 0

    if not headers:
        print("C-clean check: OK (no public .h headers found)")
        return 0

    if args.verbose:
        print("C-clean check: %d public .h header(s), compiler = %s"
              % (len(headers), compiler))

    violations = check_tree(root, compiler, verbose=args.verbose)

    if violations:
        print("C-clean check: %d public header(s) are not C-clean." % len(violations))
        print()
        for violation in violations:
            print(violation.describe(root))
            print()
        print("A `.h` under `src/**/include/` is a promise that a C host can")
        print("#include it.  Either make the header valid C11, or rename it to")
        print("`.hpp` if it was always C++-only.  See")
        print("docs/adr/ADR-026-scheduler-c-abi.md and")
        print("docs/adr/ADR-015-api-stability-policy.md.")
        return 1

    print("C-clean check: OK (%d public .h header(s) compile as %s)"
          % (len(headers), C_STANDARD.upper()))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except KeyboardInterrupt:
        sys.exit(130)
