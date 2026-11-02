#!/usr/bin/env python3
"""Build a release bundle (``.tar.xz``) from an install staging directory.

Why this exists
---------------
The Linux/macOS release jobs pack the staging directory with::

    tar -C "$staging" -cJf bundle.tar.xz .

The Windows job used to do the same thing with 7-Zip::

    7z a -ttar -so "$staging" | 7z a -txz -si bundle.tar.xz

That is broken: with ``-so`` 7-Zip consumes the first positional argument as
the *archive name* (which is then discarded, since the archive goes to
stdout) and, with no file list left, it packs the **current working
directory**.  The published v1.0.1 Windows bundle therefore contained the
whole CI workspace -- sources, ``.git`` and the complete MSVC build tree --
instead of the install staging tree, which is why the asset was 822 MB while
the other three platforms were 2-3 MB.

This script packs exactly the staging tree, with archive members relative to
the staging root, so the Windows bundle has the same layout as the other
platforms::

    include/...
    lib/...

Usage::

    python tools/make_release_bundle.py --staging build/_install/windows \
        --output build/nimrtc-1.0.1-windows-x86_64-msvc.tar.xz

The exit status is non-zero when the staging directory is missing or empty,
so a broken install step can never produce an empty (or wildly oversized)
bundle.
"""

from __future__ import annotations

import argparse
import os
import sys
import tarfile

# Refuse to build a bundle larger than this.  The real Windows bundle is
# ~2-3 MB; the broken one was 822 MB.  The guard is deliberately generous so
# that a legitimate growth of the payload does not break the release, while
# still catching "the wrong directory got archived".
MAX_BUNDLE_BYTES = 64 * 1024 * 1024


def collect(staging: str) -> list[tuple[str, str]]:
    """Return sorted ``(absolute_path, archive_name)`` pairs under *staging*."""
    entries: list[tuple[str, str]] = []
    for root, dirs, files in os.walk(staging):
        dirs.sort()
        for name in sorted(files):
            absolute = os.path.join(root, name)
            relative = os.path.relpath(absolute, staging).replace(os.sep, "/")
            entries.append((absolute, relative))
    return entries


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--staging", required=True,
                        help="install staging directory to pack")
    parser.add_argument("--output", required=True,
                        help="path of the .tar.xz bundle to write")
    args = parser.parse_args(argv)

    staging = os.path.abspath(args.staging)
    output = os.path.abspath(args.output)

    if not os.path.isdir(staging):
        print(f"error: staging directory not found: {staging}", file=sys.stderr)
        return 1

    entries = collect(staging)
    if not entries:
        print(f"error: staging directory is empty: {staging}", file=sys.stderr)
        return 1

    os.makedirs(os.path.dirname(output), exist_ok=True)
    # 'w:xz' == tar -cJf: uncompressed tar piped through xz (level 6).
    with tarfile.open(output, "w:xz") as archive:
        for absolute, relative in entries:
            archive.add(absolute, arcname=relative)

    size = os.path.getsize(output)
    print(f"bundle: {output}")
    print(f"entries: {len(entries)}")
    print(f"size: {size} bytes")
    for absolute, relative in entries[:10]:
        print(f"  {relative}")

    if size > MAX_BUNDLE_BYTES:
        print(f"error: bundle is {size} bytes, over the "
              f"{MAX_BUNDLE_BYTES}-byte sanity limit", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
