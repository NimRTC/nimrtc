#!/usr/bin/env python3
"""check_install_tree_module_closure.py — slice v1-install-tree-module-closure (ADR-039).

Computes the engine link closure — the set of `nimrtc_*` static archives a
downstream host must carry to link `nimrtc::engine` from an installed bundle —
then checks that BOTH hand-maintained short lists cover it:

  - `_nimrtc_install_shorts`  (cmake/NimRTCInstall.cmake)   — what gets installed
  - `_nimrtc_module_shorts`   (cmake/NimRTCConfig.cmake.in) — what the installed
    config chains onto `nimrtc::engine`

The closure is the fixpoint over the engine's PRIVATE `nimrtc::X` links plus
every module's own `target_link_libraries(... nimrtc::X ...)`, excluding
`core` / `plugins` / `vendor` (those are not module archives: `core` is the
header-only base lib, `plugins` the header-only interface set, `vendor` the
vendored third-party namespace).

The tool reads the repository tree only — no build, no staged install, no
network — and is deterministic: the same tree produces the same output.

A gap means a module is missing from one list, and the message names both the
module and the list (e.g. a host would hit LNK2019 for its registrar).

Exit status: 0 = both lists cover the closure, 1 = at least one gap.
"""

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

ENGINE_CMAKE = "src/engine/CMakeLists.txt"
MODULES_DIR = "src/modules"
INSTALL_CMAKE = "cmake/NimRTCInstall.cmake"
CONFIG_CMAKE = "cmake/NimRTCConfig.cmake.in"

# Layout invariants: these names are not module archives and are excluded from
# the closure (see module docstring).  `nimrtc::vendor::X` tokens parse as
# "vendor" and are excluded the same way.
EXCLUDED = frozenset({"core", "plugins", "vendor"})


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def strip_comments(text: str) -> str:
    """Drop `# ...` comments (to end of line) from CMake text."""
    return re.sub(r"#[^\n]*", "", text)


# `target_link_libraries(` — group(0) ends right after the opening paren, so the
# target name is the first token of the body.
_TLL_RE = re.compile(r"target_link_libraries\s*\(")
_NIMRTC_RE = re.compile(r"nimrtc::([A-Za-z0-9_]+)")


def iter_tll_bodies(text: str):
    """Yield `(target_link_libraries(...)` opener, body) for every call.

    The body ends at the paren balancing the opener, so nested parens
    (generator expressions etc.) do not truncate it.  `text` should already
    have comments stripped — an unbalanced paren inside a comment would
    otherwise throw off the balance scan.
    """
    i = 0
    n = len(text)
    while i < n:
        m = _TLL_RE.search(text, i)
        if not m:
            return
        start = m.end()
        depth = 1
        j = start
        while j < n and depth:
            if text[j] == "(":
                depth += 1
            elif text[j] == ")":
                depth -= 1
            j += 1
        yield m.group(0), text[start : j - 1]
        i = j


def tll_dep_shorts(body: str, excluded=EXCLUDED):
    """All `nimrtc::X` shorts named in a `target_link_libraries` body."""
    out = set()
    for m in _NIMRTC_RE.finditer(body):
        short = m.group(1)
        if short not in excluded:
            out.add(short)
    return out


def module_tll_deps(repo: Path):
    """Map every `nimrtc_<short>` target to its nimrtc::<short> deps.

    Reads only the top-level `src/modules/<dir>/CMakeLists.txt` files (tests/
    excluded).  Targets like `nimrtc_dtls_seam` that live inside another
    module's file (src/modules/dtls/CMakeLists.txt) are picked up here too —
    the map is keyed by target name, not by directory.
    """
    deps = {}
    for cmake in sorted((repo / MODULES_DIR).glob("*/CMakeLists.txt")):
        text = strip_comments(read(cmake))
        for opener, body in iter_tll_bodies(text):
            target_m = re.match(r"target_link_libraries\s*\(\s*(nimrtc_[A-Za-z0-9_]+)",
                                opener + body)
            if not target_m:
                continue
            target = target_m.group(1)[len("nimrtc_"):]
            deps.setdefault(target, set()).update(tll_dep_shorts(body))
    return deps


def engine_private_links(repo: Path):
    """The engine's PRIVATE nimrtc::X links from src/engine/CMakeLists.txt.

    `nimrtc_core_objects` and `nimrtc_vendor_libjuice` carry no `nimrtc::`
    prefix, so they are naturally skipped; the PUBLIC `nimrtc::core` link sits
    before the PRIVATE keyword and is dropped by slicing after it.
    """
    text = strip_comments(read(repo / ENGINE_CMAKE))
    links = set()
    for opener, body in iter_tll_bodies(text):
        if not re.match(r"target_link_libraries\s*\(\s*nimrtc_engine\b", opener + body):
            continue
        priv_at = body.find("PRIVATE")
        priv = body[priv_at + len("PRIVATE"):] if priv_at >= 0 else body
        links.update(tll_dep_shorts(priv))
    return links


def compute_closure(repo: Path):
    """Fixpoint over engine PRIVATE links + module-to-module deps."""
    deps = module_tll_deps(repo)
    closure = set()
    stack = list(engine_private_links(repo))
    while stack:
        cur = stack.pop()
        if cur in closure or cur in EXCLUDED:
            continue
        closure.add(cur)
        for dep in deps.get(cur, ()):
            if dep not in closure and dep not in EXCLUDED:
                stack.append(dep)
    return closure


def short_list(path: Path, var: str):
    """Parse `set(<var> ...)` from a cmake file into a set of shorts."""
    text = read(path)
    m = re.search(r"set\(" + re.escape(var) + r"(.*?)\n\s*\)", text, re.S)
    if not m:
        return None
    body = strip_comments(m.group(1))
    return set(re.findall(r"[A-Za-z0-9_]+", body))


def check_closure(repo: Path):
    """Compare the computed closure against both short lists."""
    results = []
    closure = compute_closure(repo)
    results.append((True, f"engine link closure computed: {len(closure)} modules"))
    results.append((True, "closure: " + " ".join(sorted(closure))))

    install_shorts = short_list(repo / INSTALL_CMAKE, "_nimrtc_install_shorts")
    if install_shorts is None:
        results.append((False, "install rules: _nimrtc_install_shorts parseable"))
    else:
        for mod in sorted(closure - install_shorts):
            results.append((False,
                            f"install list missing closure module: {mod} "
                            f"(_nimrtc_install_shorts in {INSTALL_CMAKE})"))

    module_shorts = short_list(repo / CONFIG_CMAKE, "_nimrtc_module_shorts")
    if module_shorts is None:
        results.append((False, "config source: _nimrtc_module_shorts parseable"))
    else:
        for mod in sorted(closure - module_shorts):
            results.append((False,
                            f"config list missing closure module: {mod} "
                            f"(_nimrtc_module_shorts in {CONFIG_CMAKE})"))

    return results


def run_checks(results):
    """Print every check; return exit code."""
    all_ok = True
    for ok, msg in results:
        tag = "PASS" if ok else "FAIL"
        if not ok:
            all_ok = False
        print(f"{tag} {msg}")
    if all_ok:
        print("OK — engine link closure is fully covered by both short lists")
        return 0
    print("FAIL — engine link closure is missing from the short lists",
          file=sys.stderr)
    return 1


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo", default=str(REPO), help="repository root")
    args = ap.parse_args(argv)
    return run_checks(check_closure(Path(args.repo)))


# ── 输出编码：必须显式钉死 UTF-8 ───────────────────────────────────────
#
# 病灶（实测，中文 Windows + Node 消费者）：
#   Python 的 `sys.stdout.encoding` 跟随系统 locale（这台机器上是 gbk/cp936），
#   于是 `OK — ...` 里的 em-dash 被写成 GBK 字节 `a1 aa`；
#   Node 侧按 UTF-8 解码 → 变成两个替换字符 `fffd fffd` →
#   消费方的正则 `/OK — engine link closure .../` 匹配不上 →
#   验收报「工具必须输出最终 OK」，而人看到的工具输出**完全正常**。
#
# 症状与原因隔着一层编码，肉眼根本看不出来 ——
# 打印到终端时 PowerShell 会按 GBK 再解一次，于是「看着对、不匹配」。
#
# 所以：**输出字节流不能依赖系统 locale**。
# `errors='replace'` 兜底 —— 万一还有别的非 ASCII 字符，
# 宁可替换也不让整个工具因为编码错误崩掉（崩掉 = 门禁红，且原因更难查）。
#
# 注意：这只改**编码**，不改输出内容 —— 判定逻辑一个字没动。
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
