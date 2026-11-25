#!/usr/bin/env python3
"""check_install_tree_module_closure_self_test.py — slice v1-install-tree-module-closure.

Builds synthetic repository trees that deliberately break the two hand-maintained
short lists and asserts the closure tool (`check_install_tree_module_closure.py`)
reports the deliberate gaps — the tool cannot be "always green".  Also asserts a
complete tree stays green and an empty engine closure does not produce a false
positive.

Exit status: 0 = every assertion passed, 1 = at least one failed.
"""

import importlib.util
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


def _load_tool():
    spec = importlib.util.spec_from_file_location(
        "check_install_tree_module_closure",
        HERE / "check_install_tree_module_closure.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _write(path: Path, text: str):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def make_repo(base: Path, engine_privates, module_edges, install_shorts, module_shorts):
    """Create a synthetic repo tree the closure tool can parse.

    `engine_privates`  — the `nimrtc::X` links in the engine's PRIVATE block.
    `module_edges`     — {module: [dep, ...]} one `nimrtc::dep` link per module.
    `install_shorts`   — `_nimrtc_install_shorts` entries.
    `module_shorts`    — `_nimrtc_module_shorts` entries.
    """
    priv = "\n".join(f"        nimrtc::{m}" for m in sorted(engine_privates))
    _write(base / "src/engine/CMakeLists.txt",
           "add_library(nimrtc_engine STATIC src/engine.cpp)\n"
           "target_link_libraries(nimrtc_engine\n"
           "    PUBLIC\n"
           "        nimrtc::core\n"
           "        nimrtc_core_objects\n"
           "    PRIVATE\n"
           f"{priv}\n"
           ")\n")

    all_mods = set(engine_privates)
    for deps in module_edges.values():
        all_mods.update(deps)
    for mod in sorted(all_mods):
        deps_lns = "\n".join(
            f"        nimrtc::{d}" for d in module_edges.get(mod, []))
        _write(base / f"src/modules/{mod}/CMakeLists.txt",
               f"add_library(nimrtc_{mod} STATIC src/{mod}.cpp)\n"
               "target_link_libraries(nimrtc_" + mod + "\n"
               "    PUBLIC\n"
               "        nimrtc::core\n"
               "        nimrtc::plugins\n"
               "    PRIVATE\n"
               f"{deps_lns}\n"
               ")\n"
               f"add_library(nimrtc::{mod} ALIAS nimrtc_{mod})\n")

    install_body = "\n".join(f"  {s}" for s in install_shorts)
    _write(base / "cmake/NimRTCInstall.cmake",
           f"set(_nimrtc_install_shorts\n{install_body}\n)\n")

    module_body = "\n".join(f"  {s}" for s in module_shorts)
    _write(base / "cmake/NimRTCConfig.cmake.in",
           f"set(_nimrtc_module_shorts\n{module_body}\n)\n")
    return base


def self_test():
    tool = _load_tool()
    passed = True

    def emit(ok, desc):
        nonlocal passed
        tag = "PASS" if ok else "FAIL"
        if not ok:
            passed = False
        print(f"{tag} {desc}")

    tmp = Path(tempfile.mkdtemp(prefix="nimrtc_check_closure_"))

    engine_privates = ["audio3a", "sched", "video_pipeline"]
    module_edges = {
        "sched": ["pacing"],
        "video_pipeline": ["video_frame", "video_jb"],
    }
    closure = {"audio3a", "sched", "video_pipeline", "pacing", "video_frame", "video_jb"}

    # --- Positive: both lists complete -> green ----------------------------
    repo = make_repo(tmp / "pos_complete", engine_privates, module_edges,
                     sorted(closure), sorted(closure))
    results = tool.check_closure(repo)
    if tool.run_checks(results) == 0:
        emit(True, "a tree with complete lists is green (exit 0)")
    else:
        emit(False, "a tree with complete lists is green (exit 0)")

    # --- Negative: a closure module missing from install list --------------
    repo = make_repo(tmp / "neg_install", engine_privates, module_edges,
                     sorted(closure - {"pacing"}), sorted(closure))
    results = tool.check_closure(repo)
    exit_code = tool.run_checks(results)
    named = any(not ok and "pacing" in msg and "install" in msg for ok, msg in results)
    if exit_code == 1 and named:
        emit(True, "removing pacing from _nimrtc_install_shorts is red and names it")
    else:
        emit(False, "removing pacing from _nimrtc_install_shorts is red and names it")

    # --- Negative: a closure module missing from config list --------------
    repo = make_repo(tmp / "neg_config", engine_privates, module_edges,
                     sorted(closure), sorted(closure - {"video_jb"}))
    results = tool.check_closure(repo)
    exit_code = tool.run_checks(results)
    named = any(not ok and "video_jb" in msg and "config" in msg for ok, msg in results)
    if exit_code == 1 and named:
        emit(True, "removing video_jb from _nimrtc_module_shorts is red and names it")
    else:
        emit(False, "removing video_jb from _nimrtc_module_shorts is red and names it")

    # --- Empty closure: no false positive ----------------------------------
    repo = make_repo(tmp / "empty_closure", [], {}, [], [])
    results = tool.check_closure(repo)
    if tool.run_checks(results) == 0:
        emit(True, "an empty engine closure is green with no false positive")
    else:
        emit(False, "an empty engine closure is green with no false positive")

    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(self_test())
