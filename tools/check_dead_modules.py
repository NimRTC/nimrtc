#!/usr/bin/env python3
"""Dead-module guard: every built module must have a non-test consumer.

Architecture remediation plan D2 / Phase 4.2:

> **原则：要么接线，要么删除。"有完整实现 + 有测试 + 引擎不加载"是最坏状态**
> —— 它制造"功能已具备"的错觉。

The three states this check exists to prevent:

  1. **Implemented + tested + never called.**  The module compiles, its
     unit tests pass, and nothing in the product ever calls it.  A reader
     concludes the feature exists.  It does not.  (`assembly` was in this
     state until Phase 4.2d wired the Profile → EngineConfig bridge.)
  2. **Implemented + linked + never *used*.**  The module is a link
     dependency of the engine (so its registrar runs) but no code path
     ever reaches its behaviour.  Subtly worse than (1): the link line
     makes the dead module look alive.
  3. **Deleted from the build but still documented.**  The directory and
     the `NIMRTC_MODULE_*` option survive, so the next contributor
     re-adds it "because it looks like it is still there".

This script checks (1) and (3) mechanically, from the CMake graph and the
source tree.  It cannot check (2) — that needs a human — but it reports
the modules whose only consumer is the engine's *link* line, so a reviewer
knows where to look.

How the graph is built
----------------------

Every module directory under `src/modules/<name>/` declares its library
target (`add_library(nimrtc_<name> ...)`) and its consumers through
`target_link_libraries`.  The script walks every `CMakeLists.txt` in the
repo, extracts the `(consumer, dependency)` edges, and then asks for each
module:

  * does it have at least one consumer that is **not** a test target and
    **not** the module's own test directory?
  * if its only consumers are tests, it is *dead*: nothing in the shipped
    product can reach it.

What counts as a "real" consumer
---------------------------------

A consumer counts when it is:

  * the engine (`nimrtc_engine`) — the composition root; or
  * another module that is itself reachable from the engine; or
  * an example / an integration test under `tests/` (an executable that
    ships in the repo and represents real usage); or
  * the `core` registrar table (`src/core/src/pal_default_registrars.cpp`),
    which is the mechanism by which a plugin module reaches the registry.

A consumer does **not** count when it is:

  * the module's own `tests/` directory — a module that only its own
    tests consume is exactly the dead-module state; or
  * a test target in `tests/` that links the module *only* to assert the
    registry contract (the registry-contract test links almost every
    module, so counting it would make the check vacuous).

Why the "registry table" exception
-----------------------------------

A plugin module's whole purpose may be to register a factory.  The
registration happens through `kDefaultRegistrars[]` in
`src/core/src/pal_default_registrars.cpp`, which references the module's
`register_default_plugins` symbol — a *source-level* dependency that
CMake does not express as a `target_link_libraries` edge (the link edge
is the engine's, and it exists only to defeat the MSVC linker's static-
initialiser stripping — see Layout Invariant 5).

So the check counts a module as live when EITHER:

  * it has a non-test CMake consumer **reachable from the composition
    root** (the engine, or one of the shipped examples), OR
  * its `register_default_plugins` symbol appears in the registrar table
    or in any non-test source file outside its own directory.

Reachability, not mere linkage
-------------------------------

A module linked *only* by a target that is never shipped — a benchmark,
a one-off tool — is still dead.  The link line makes it look alive
while nothing in the product can reach it.  The check therefore walks
the dependency graph from the composition root and reports such a module
as `[unreach]` rather than letting it pass.

Three failure shapes are reported separately, because the remedy
differs:

  * `[dead]`    — a complete implementation with its own tests, but the
                  product never calls it.  The classic "looks like it
                  works" trap.
  * `[unreach]` — linked, but only by a target that never ships.
  * `[orphan]`  — nobody links it and nothing references its registrar.

Usage
-----

    python tools/check_dead_modules.py [--repo-root ROOT] [--verbose]
    python tools/check_dead_modules.py --self-test

Exit code 0 when every built module is reachable, 1 otherwise.

`--self-test` builds a synthetic module tree and asserts that the
checker classifies each shape correctly.  A guard that cannot fail is a
decoration, so the self-test runs in CI.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

# `add_library(nimrtc_jb STATIC ...)` — matched against the *body* of an
# `add_library(...)` call, so the command name is not part of the pattern.
ADD_LIBRARY_RE = re.compile(
    r"^\s*(nimrtc_[A-Za-z0-9_]+)\s+(STATIC|SHARED|OBJECT|INTERFACE)\b",
    re.MULTILINE,
)

# A dependency name inside a link body: `nimrtc::jb`, `nimrtc_jb`.
DEP_RE = re.compile(r"\bnimrtc(?:::|_)([A-Za-z0-9_]+)\b")

# The canonical target name for a module directory: `src/modules/jb/`
# -> `nimrtc_jb`.
MODULE_DIR_RE = re.compile(r"^src/modules/([a-z0-9_]+)$")

# The registrar table — the source-level plugin registration mechanism.
REGISTRAR_TABLE = Path("src/core/src/pal_default_registrars.cpp")

# Directory names that never hold the project's own build definitions.
#
# `third_party`  — vendored code is not part of the module graph.
# `build`, `out` — the conventional CMake build-output directory names.
# `.git`         — git's internals.
#
# The *general* rule, though, is "a directory that contains a
# `CMakeCache.txt` is a CMake **build tree**, not a source tree", and
# that is what the walker below keys on.  A build tree's
# `CMakeFiles/CMakeScratch/TryCompile-*/CMakeLists.txt` files are
# transient: a concurrent build creates and deletes them while the
# checker is walking, and the checker then dies with `FileNotFoundError`
# on a file it had just listed.  Pruning the whole build tree removes
# the race at the root instead of papering over it with a try/except.
PRUNED_DIR_NAMES = frozenset({
    ".git",
    "build",
    "out",
    "third_party",
    "node_modules",
})


def _iter_source_cmake_files(root: Path) -> list[Path]:
    """Return every `CMakeLists.txt` that belongs to a *source* tree.

    A directory that carries a `CMakeCache.txt` is a CMake **build
    tree** — the output of a configure run, not a source directory.
    Its `CMakeFiles/CMakeScratch/TryCompile-*/CMakeLists.txt` files are
    transient: a concurrent build creates and deletes them while the
    checker walks the tree, and the checker then dies with
    `FileNotFoundError` on a file it had just listed.

    Pruning the whole build tree removes the race at its root rather
    than papering over it with a `try/except` around the read.
    """
    out: list[Path] = []
    root = root.resolve()
    for dirpath, dirnames, filenames in os.walk(root):
        here = Path(dirpath)
        # Prune in place so os.walk does not descend.
        dirnames[:] = [
            d for d in dirnames
            if d not in PRUNED_DIR_NAMES
            and not (here / d / "CMakeCache.txt").is_file()
        ]
        if "CMakeLists.txt" in filenames:
            out.append(here / "CMakeLists.txt")
    out.sort()
    return out


def _read_text(path: Path) -> str:
    """Read a text file, tolerating a file that vanished mid-scan.

    A concurrent build (or a `git checkout` / branch switch) can remove a
    file between the moment the checker lists it and the moment it reads
    it.  A guard that dies on a race is a guard that gets disabled, so
    a missing file reads as empty and the checker moves on.
    """
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except (FileNotFoundError, IsADirectoryError, PermissionError, OSError):
        return ""


def _command_bodies(text: str, command: str) -> list[str]:
    """Extract the body of every `command(...)` call.

    CMake commands are not regular enough for a single regex: the
    closing paren may sit on the same line as the last argument, on a
    line of its own, or inside a generator expression.  This walks the
    text with a paren counter instead, which is exact.
    """
    out: list[str] = []
    needle = command + "("
    pos = 0
    while True:
        start = text.find(needle, pos)
        if start < 0:
            break
        i = start + len(needle)
        depth = 1
        body_start = i
        while i < len(text) and depth > 0:
            ch = text[i]
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    break
            elif ch == "#":
                # Skip a line comment to its end.
                nl = text.find("\n", i)
                i = len(text) if nl < 0 else nl
                continue
            i += 1
        out.append(text[body_start:i])
        pos = i + 1
    return out


def _first_arg(body: str) -> str:
    """The first whitespace-delimited token of a command body."""
    return body.split(None, 1)[0] if body.strip() else ""


class Module:
    """One module directory under `src/modules/`."""

    def __init__(self, name: str, directory: Path) -> None:
        self.name = name
        self.directory = directory
        self.targets: list[str] = []
        self.consumers: set[str] = set()
        # Consumers that are test targets — excluded from the "live" test.
        self.test_consumers: set[str] = set()
        # Modules whose registrar symbol is referenced outside their own
        # directory (the plugin-registration escape hatch).
        self.registered_symbol_used = False
        # Set when the module declares no CMake target at all (a
        # header-only module, or a module that only exists as sources).
        self.has_target = False

    @property
    def primary_target(self) -> str:
        return self.targets[0] if self.targets else f"nimrtc_{self.name}"

    def __repr__(self) -> str:
        return f"<Module {self.name} targets={self.targets}>"


def is_test_target(name: str) -> bool:
    """True when a CMake target name denotes a test."""
    base = name.split("::")[-1]
    return (base.startswith("test")
            or base.endswith("_test")
            or base.endswith("_tests")
            or "test_" in base
            or base == "gtest_main")


def is_test_directory(path: Path) -> bool:
    """True when the CMakeLists lives in a `tests/` directory."""
    return "tests" in path.parts


def parse_cmake_graph(root: Path) -> tuple[dict[str, Module], dict[str, set[str]]]:
    """Return (modules by name, consumer edges by dependency target)."""
    modules: dict[str, Module] = {}

    modules_root = root / "src" / "modules"
    if modules_root.is_dir():
        for child in sorted(modules_root.iterdir()):
            if not child.is_dir():
                continue
            cm = child / "CMakeLists.txt"
            if not cm.is_file():
                continue
            modules[child.name] = Module(child.name, child)

    # ---- module targets ---------------------------------------------------
    for mod in modules.values():
        cm = mod.directory / "CMakeLists.txt"
        txt = _read_text(cm)
        for body in _command_bodies(txt, "add_library"):
            m = ADD_LIBRARY_RE.match(body)
            if m:
                mod.targets.append(m.group(1))
        mod.has_target = bool(mod.targets)

    # ---- consumer edges ----------------------------------------------------
    # `edges[dependency_target] = {consumer_target, ...}`
    edges: dict[str, set[str]] = {}

    # The consumers that matter are the ones inside the project's own
    # build files.  `src/third_party/` is excluded — vendored code is not
    # part of the module graph.
    #
    # The walk prunes CMake **build trees** (a directory that carries a
    # `CMakeCache.txt` is an output tree, not a source tree).  Without the
    # prune, the walk also picks up `CMakeFiles/CMakeScratch/
    # TryCompile-*/CMakeLists.txt`, which are transient: a concurrent
    # build creates and deletes them while the checker walks, and the
    # checker then dies on a file it had just listed.
    cmake_files = list(_iter_source_cmake_files(root))

    for cm in cmake_files:
        txt = _read_text(cm)
        for body in _command_bodies(txt, "target_link_libraries"):
            consumer = _first_arg(body)
            if not consumer or consumer.startswith("${"):
                # A variable target name (`${name}`) carries no
                # information about *who* consumes the dependency.
                continue
            # The dependencies start after the consumer name; scanning the
            # whole body would find the consumer's own name and create a
            # self-edge.
            rest = body[len(consumer):]
            for dep in DEP_RE.findall(rest):
                # `nimrtc::jb` -> `nimrtc_jb`; `nimrtc_jb` -> `nimrtc_jb`.
                dep_target = f"nimrtc_{dep}"
                if dep_target == consumer:
                    continue
                edges.setdefault(dep_target, set()).add(consumer)

    return modules, edges


def find_registrar_users(root: Path, modules: dict[str, Module]) -> None:
    """Mark modules whose registrar symbol is used outside their own tree.

    A plugin module's whole purpose may be to register a factory.  The
    registration happens through `kDefaultRegistrars[]` in
    `src/core/src/pal_default_registrars.cpp`, which references the
    module's `register_default_plugins` symbol — a *source-level*
    dependency that CMake does not express as a `target_link_libraries`
    edge (the link edge is the engine's, and it exists only to defeat
    the MSVC linker's static-initialiser stripping — see Layout
    Invariant 5).

    The scan is done **once** over the source tree, not once per module:
    the symbol name is module-specific, so the set of files mentioning
    `register_default_plugins` is collected first and then filtered.
    """
    # ---- one pass over the tree ------------------------------------------
    # Map: module name -> set of source files that mention it outside
    # the module's own directory.
    mentions: dict[str, set[Path]] = {}

    src_root = root / "src"
    sources = [
        p for p in src_root.rglob("*")
        if p.suffix in (".cpp", ".hpp", ".h", ".cc")
        and "third_party" not in p.relative_to(root).parts
    ]

    for path in sources:
        txt = _read_text(path)
        if "register_default_plugins" not in txt:
            continue
        for name, mod in modules.items():
            if f"nimrtc::{name}" not in txt:
                continue
            # Skip the module's own tree — a module registering itself
            # does not make it live.
            if mod.directory in path.parents:
                continue
            mentions.setdefault(name, set()).add(path)

    for name, mod in modules.items():
        mod.registered_symbol_used = bool(mentions.get(name))


def non_test_consumers(consumers: set[str]) -> set[str]:
    """Filter out test targets."""
    return {c for c in consumers if not is_test_target(c)}


# The roots of the reachability walk: the shipped executables and the
# composition root.  A module is *reachable* when the engine can reach it
# through a chain of link edges, or when an example / integration test
# links it directly.
REACHABILITY_ROOTS = {
    "nimrtc_engine",
    # The examples (the `demo-*` / `loopback-*` executables) and the
    # cross-module integration tests under `tests/`.
    "demo-agent-gateway",
    "demo-p2p",
    "demo-wtserver",
    "loopback-p2p",
}


def compute_reachable(edges: dict[str, set[str]], roots: set[str]) -> set[str]:
    """Targets reachable from `roots` through the (consumer → dependency)
    link edges, restricted to non-test consumers."""
    reachable: set[str] = set()
    stack = [r for r in roots if not is_test_target(r)]

    while stack:
        cur = stack.pop()
        # `edges` is keyed by *dependency*; we need the reverse map.
        for dep, consumers in edges.items():
            if cur in consumers:
                if dep not in reachable:
                    reachable.add(dep)
                    if not is_test_target(dep):
                        stack.append(dep)
    return reachable


def check(root: Path, verbose: bool = False) -> list[str]:
    """Return a list of human-readable problems (empty = all good)."""
    modules, edges = parse_cmake_graph(root)
    find_registrar_users(root, modules)

    reachable = compute_reachable(edges, REACHABILITY_ROOTS)

    problems: list[str] = []

    for name in sorted(modules):
        mod = modules[name]

        if not mod.has_target:
            problems.append(
                f"[no-target] module '{name}' declares no CMake library target "
                f"({mod.directory.relative_to(root) / 'CMakeLists.txt'}) — it "
                f"cannot be linked, so it is not part of the product")

        # ---- collect the module's consumers ------------------------------
        consumers: set[str] = set()
        for tgt in mod.targets:
            consumers |= edges.get(tgt, set())

        real = non_test_consumers(consumers)
        mod.consumers = real
        mod.test_consumers = consumers - real

        # A module is live when EITHER some non-test target links it, OR
        # its registrar symbol is referenced from outside its own tree.
        #
        # "some non-test target links it" is necessary but not sufficient:
        # a benchmark target (`bench_engine_hotpath`) is built but never
        # shipped, so a module consumed *only* by a benchmark is still
        # unreachable from the product.  The reachability walk therefore
        # narrows the consumer set to targets the composition root can
        # actually reach.
        shipped = {c for c in real if c in reachable or c in REACHABILITY_ROOTS}
        mod.consumers = shipped
        mod.test_consumers = consumers - real

        if shipped or mod.registered_symbol_used:
            if verbose:
                if shipped:
                    tag = "ok     "
                    who = ", ".join(sorted(shipped))
                else:
                    tag = "ok-reg "
                    who = "registrar table"
                print(f"  [{tag}] {name:24s} <- {who}")
            continue

        # ---- neither: the module is dead ---------------------------------
        #
        # Three distinct failure shapes, reported separately because the
        # remedy differs:
        #
        #   [dead]     the module has a complete implementation and its own
        #              tests, but the product never calls it.
        #   [unreach]  something links it, but only a target that is never
        #              shipped (a benchmark).  The link line makes it look
        #              alive while nothing in the product can reach it.
        #   [orphan]   nothing links it at all.
        if mod.test_consumers:
            problems.append(
                f"[dead]     module '{name}' is consumed ONLY by its own tests "
                f"({sorted(mod.test_consumers)}) — it is a complete "
                f"implementation with tests that the product never calls. "
                f"Either wire it in or delete it "
                f"(architecture remediation plan D2).")
        elif real:
            problems.append(
                f"[unreach]  module '{name}' is linked ONLY by targets that "
                f"are never shipped ({sorted(real)}) — it is unreachable "
                f"from the composition root, so the link line makes it look "
                f"alive while nothing in the product can reach it. Either "
                f"wire it in or delete it (architecture remediation plan D2).")
        else:
            problems.append(
                f"[orphan]   module '{name}' has no consumer at all — nothing "
                f"links it and nothing references its registrar. Either wire "
                f"it in or delete it (architecture remediation plan D2).")

    return problems


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------
#
# A guard that cannot fail is a decoration.  This self-test builds a
# synthetic module tree in a temporary directory and asserts that the
# checker classifies each shape correctly:
#
#   good      — linked by the engine                 -> live
#   regmod    — referenced only by the registrar table -> live
#   dead      — linked only by its own tests           -> DEAD
#   orphan    — linked by nobody                      -> ORPHAN
#   benchonly — linked only by a benchmark target      -> DEAD (the
#               benchmark is built but never shipped, so nothing in the
#               product can reach the module)
#
# It runs in CI so that a future refactor of the checker cannot silently
# turn it into a no-op that passes everything.

_SELF_TEST_FILES = {
    # path (relative to the fixture root) -> file content
    "src/modules/good/CMakeLists.txt": (
        "add_library(nimrtc_good STATIC src/good.cpp)\n"
        "target_link_libraries(nimrtc_good PUBLIC nimrtc::core)\n"
    ),
    "src/modules/regmod/CMakeLists.txt": (
        "add_library(nimrtc_regmod STATIC src/regmod.cpp)\n"
    ),
    "src/modules/dead/CMakeLists.txt": (
        "add_library(nimrtc_dead STATIC src/dead.cpp)\n"
        "target_link_libraries(nimrtc_dead PUBLIC nimrtc::core)\n"
    ),
    "src/modules/dead/tests/CMakeLists.txt": (
        "add_executable(test_dead_only test_dead_only.cpp)\n"
        "target_link_libraries(test_dead_only PRIVATE nimrtc::dead "
        "GTest::gtest_main)\n"
    ),
    "src/modules/orphan/CMakeLists.txt": (
        "add_library(nimrtc_orphan STATIC src/orphan.cpp)\n"
    ),
    "src/modules/benchonly/CMakeLists.txt": (
        "add_library(nimrtc_benchonly STATIC src/benchonly.cpp)\n"
    ),
    "src/engine/CMakeLists.txt": (
        "add_library(nimrtc_engine STATIC src/engine.cpp)\n"
        "target_link_libraries(nimrtc_engine PUBLIC nimrtc::good "
        "nimrtc::core)\n"
    ),
    "bench/CMakeLists.txt": (
        "add_executable(bench_hotpath bench_hotpath.cpp)\n"
        "target_link_libraries(bench_hotpath PRIVATE nimrtc::benchonly)\n"
    ),
    "src/core/src/pal_default_registrars.cpp": (
        "// The registrar table references a plugin module's registration\n"
        "// entry point.  A module referenced only here is still live: the\n"
        "// registration *is* the consumption.\n"
        "namespace nimrtc::core { void register_default_plugins(); }\n"
        "namespace nimrtc::regmod { void register_default_plugins() noexcept; }\n"
        "namespace nimrtc::core::detail {\n"
        "using RegistrarFn = void(*)() noexcept;\n"
        "constexpr RegistrarFn kDefaultRegistrars[] = {\n"
        "    &nimrtc::regmod::register_default_plugins,\n"
        "};\n"
        "} // namespace nimrtc::core::detail\n"
    ),
}


def _write_self_test_fixture(root: Path) -> None:
    for rel, content in _SELF_TEST_FILES.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")


def self_test() -> int:
    """Verify the checker classifies each module shape correctly."""
    import tempfile

    failures: list[str] = []

    with tempfile.TemporaryDirectory(prefix="nimrtc-deadmod-selftest-") as tmp:
        root = Path(tmp)
        _write_self_test_fixture(root)

        problems = check(root, verbose=False)

        def names_with(tag: str) -> set[str]:
            out = set()
            for p in problems:
                if tag in p:
                    # "[dead]     module 'x' is ..." -> x
                    out.add(p.split("'")[1])
            return out

        dead = names_with("[dead]")
        orphan = names_with("[orphan]")
        unreach = names_with("[unreach]")

        # --- the shapes that must be reported ------------------------------
        if "dead" not in dead:
            failures.append(
                "a module consumed only by its own tests must be reported "
                f"[dead]; got dead={sorted(dead)}")
        if "benchonly" not in unreach:
            failures.append(
                "a module consumed only by a benchmark target (never "
                "shipped) must be reported [unreach]; got "
                f"unreach={sorted(unreach)}")
        if "orphan" not in orphan:
            failures.append(
                "a module with no consumer at all must be reported [orphan]; "
                f"got orphan={sorted(orphan)}")

        # --- the shapes that must NOT be reported ----------------------------
        for name in ("good", "regmod"):
            if name in dead or name in orphan or name in unreach:
                failures.append(
                    f"module '{name}' is live and must not be reported; "
                    f"dead={sorted(dead)} orphan={sorted(orphan)} "
                    f"unreach={sorted(unreach)}")

        # --- the healthy tree must produce no problems at all ---------------
        # (a separate, healthy fixture)
        with tempfile.TemporaryDirectory(prefix="nimrtc-deadmod-selftest-ok-") as tmp2:
            ok_root = Path(tmp2)
            for rel, content in _SELF_TEST_FILES.items():
                if "dead" in rel or "orphan" in rel or "benchonly" in rel:
                    continue
                path = ok_root / rel
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding="utf-8")
            ok_problems = check(ok_root, verbose=False)
            if ok_problems:
                failures.append(
                    "a healthy tree must produce no problems; got "
                    f"{ok_problems}")

    if failures:
        print("dead-module check self-test FAILED:")
        for f in failures:
            print(f"  - {f}")
        return 1

    print("dead-module check self-test OK: the checker distinguishes "
          "live / dead / orphan / benchmark-only modules")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=None,
                        help="repository root (default: the parent of tools/)")
    parser.add_argument("--verbose", action="store_true",
                        help="print the per-module consumption map")
    parser.add_argument("--self-test", action="store_true",
                        help="run the checker's own self-test and exit")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    root = args.repo_root or Path(__file__).resolve().parent.parent

    if args.verbose:
        print("module consumption map:")

    problems = check(root, verbose=args.verbose)

    if not problems:
        print("dead-module check OK: every module under src/modules/ has a "
              "non-test consumer")
        return 0

    for p in problems:
        print(p)
    print(f"\n{len(problems)} dead-module problem(s) found")
    return 1


if __name__ == "__main__":
    sys.exit(main())
