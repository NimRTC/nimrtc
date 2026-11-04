#!/usr/bin/env python3
"""Check relative links and heading anchors in project Markdown files.

Scope: all tracked Markdown outside vendored trees (``src/third_party``),
build output and ``.git``.  Absolute URLs, ``mailto:`` and bare anchors are
handled separately:

* relative file links must point at an existing path
* ``#anchor`` links must match a heading in the same file, using GitHub's
  slug algorithm (lowercase, drop non-word characters, spaces -> dashes)

Usage:
    python tools/check_md_links.py [--verbose] [path ...]
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import unicodedata

SKIP_DIRS = {".git", "build", "out", "node_modules", ".zelto", "third_party"}

LINK_RE = re.compile(r"(?<!\!)\[([^\]]*)\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
IMAGE_RE = re.compile(r"!\[([^\]]*)\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
HTML_HREF_RE = re.compile(r"""href\s*=\s*["']([^"']+)["']""")

# A fenced code block marker; links inside are not real links.
FENCE_RE = re.compile(r"^\s*(```+|~~~+)")


def slugify(text: str) -> str:
    """Reproduce GitHub's heading slug rules closely enough for link checks."""
    text = re.sub(r"`([^`]*)`", r"\1", text)  # inline code
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", text)  # links / images
    text = re.sub(r"[*_~]", "", text)  # emphasis markers
    text = text.strip().lower()
    text = "".join(
        ch for ch in text if ch.isalnum() or ch in " -_" or _is_cjk(ch)
    )
    text = text.replace(" ", "-")
    return text


def _is_cjk(ch: str) -> bool:
    return unicodedata.category(ch) in ("Lo", "Ll", "Lu", "Nd")


def strip_code(lines: list[str]) -> list[str]:
    """Blank out fenced code block content so links there are ignored."""
    out: list[str] = []
    fence: str | None = None
    for line in lines:
        m = FENCE_RE.match(line)
        if fence is None and m:
            fence = m.group(1)[0] * 3
            out.append("")
            continue
        if fence is not None:
            if m and line.strip().startswith(fence):
                fence = None
            out.append("")
            continue
        out.append(line)
    return out


def collect_anchors(path: str) -> set[str]:
    with open(path, encoding="utf-8") as fh:
        lines = strip_code(fh.read().splitlines())
    anchors: set[str] = set()
    for line in lines:
        m = re.match(r"^\s{0,3}(#{1,6})\s+(.*?)\s*#*\s*$", line)
        if not m:
            continue
        slug = slugify(m.group(2))
        anchors.add(slug)
    return anchors


def iter_markdown(root: str, extra: list[str]) -> list[str]:
    if extra:
        files: list[str] = []
        for item in extra:
            if os.path.isdir(item):
                for dirpath, dirnames, filenames in os.walk(item):
                    dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
                    files.extend(
                        os.path.join(dirpath, f) for f in filenames if f.endswith(".md")
                    )
            elif os.path.isfile(item):
                files.append(item)
        return sorted(set(files))

    files = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        files.extend(os.path.join(dirpath, f) for f in filenames if f.endswith(".md"))
    return sorted(files)


def check_file(path: str, root: str, verbose: bool) -> list[str]:
    problems: list[str] = []
    with open(path, encoding="utf-8") as fh:
        raw_lines = fh.read().splitlines()
    lines = strip_code(raw_lines)

    anchors_cache: dict[str, set[str]] = {}

    def anchors_for(target: str) -> set[str]:
        if target not in anchors_cache:
            anchors_cache[target] = collect_anchors(target)
        return anchors_cache[target]

    for lineno, line in enumerate(lines, 1):
        targets = [m.group(2) for m in LINK_RE.finditer(line)]
        targets += [m.group(2) for m in IMAGE_RE.finditer(line)]
        targets += [m.group(1) for m in HTML_HREF_RE.finditer(line)]

        for target in targets:
            if not target or target.startswith(("http://", "https://", "mailto:", "data:")):
                continue

            if target.startswith("#"):
                anchor = target[1:].lower()
                if not anchor:
                    continue
                if anchor not in anchors_for(path):
                    problems.append(f"{path}:{lineno}: unknown anchor #{anchor}")
                continue

            file_part, _, anchor = target.partition("#")
            file_part = file_part.split("?")[0]
            if not file_part:
                continue
            if file_part.startswith("/"):
                resolved = os.path.join(root, file_part.lstrip("/"))
            else:
                resolved = os.path.normpath(os.path.join(os.path.dirname(path), file_part))

            if not os.path.exists(resolved):
                problems.append(f"{path}:{lineno}: missing target {target}")
                continue

            if anchor:
                if not os.path.isfile(resolved):
                    problems.append(f"{path}:{lineno}: anchor target is not a file {target}")
                    continue
                if anchor.lower() not in anchors_for(resolved):
                    problems.append(f"{path}:{lineno}: unknown anchor {target}")

    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("paths", nargs="*", help="files or directories to check")
    ap.add_argument("--root", default=".", help="repository root (default: .)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    files = iter_markdown(root, args.paths)
    if args.verbose:
        print(f"checking {len(files)} markdown files")

    problems: list[str] = []
    for path in files:
        problems.extend(check_file(path, root, args.verbose))

    for p in problems:
        print(p)
    print(f"\n{len(files)} files checked, {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
