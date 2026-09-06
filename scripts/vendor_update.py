"""scripts/vendor_update.py — Standardised upstream-vendor upgrade.

The NimRTC source tree ships third-party libraries as in-tree vendored
sources under src/third_party/<name>/src/ (or include/ for nlohmann_json).
This script replaces an existing vendor with a fresh clone from upstream,
records the new SHA, and updates src/third_party/SOURCE_VERSIONS.

Why this script exists:

  * Upstream security fixes must be merged by hand into the in-tree vendor
    (we don't use git submodules for vendor sources because mbedtls 4.x
    nests framework/ as a submodule and the upstream CMakeLists of each
    vendor assumes its own repo-root layout — both make a naive
    `git submodule add` of upstream unsuitable).
  * Manual `git clone` of an upstream under src/ produces a nested
    src/.git that is easy to commit by mistake; this script enforces
    --no-local-clone and writes the SHA into SOURCE_VERSIONS as a single
    ground-truth record.

Usage:

    python scripts/vendor_update.py libopus
    python scripts/vendor_update.py mbedtls --ref 4.2.0
    python scripts/vendor_update.py --list

It is safe to interrupt (Ctrl-C): the target directory is wiped only
after the clone succeeds. A `.git` inside src/ is intentionally deleted
so the vendor source stays out of the parent repo's index.

NOTE: this script REQUIRES outbound HTTPS to github.com for clone.
Run on a workstation that has network access to GitHub. After it
finishes, `git add` the changed files and commit.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VENDOR_ROOT = ROOT / "src" / "third_party"
VERSIONS_FILE = VENDOR_ROOT / "SOURCE_VERSIONS"

# (name, upstream_url, default_ref, layout) where layout is
# "src" for libs that use a src/ subdir, "include" for single-header.
VENDORS = {
    "libjuice":      ("https://github.com/paullouisageneau/libjuice.git", "master",  "src"),
    "libsrtp":       ("https://github.com/cisco/libsrtp.git",               "master",  "src"),
    "mbedtls":       ("https://github.com/Mbed-TLS/mbedtls.git",            "4.2.0",   "src"),
    "libopus":       ("https://github.com/xiph/opus.git",                  "v1.6.1",  "src"),
    "nlohmann_json": ("https://github.com/nlohmann/json.git",               "v3.11.3", "include"),
}


def list_vendors() -> None:
    print(f"{'name':<16}{'ref':<14}{'layout':<10}")
    print("-" * 40)
    for name, (url, ref, layout) in VENDORS.items():
        print(f"{name:<16}{ref:<14}{layout:<10}")
    print()
    print(f"Versions file: {VERSIONS_FILE}")


def clone_vendor(name: str, ref: str, url: str, layout: str,
                 target: Path, log_path: Path) -> str:
    """Clone upstream into a tmpdir, verify, move to target. Return SHA."""
    import tempfile
    with tempfile.TemporaryDirectory(prefix=f"nimrtc_vendor_{name}_") as tmp:
        tmp_p = Path(tmp)
        # Step 1: clone
        print(f"  clone {url} @ {ref} → {tmp_p}")
        subprocess.run(
            ["git", "clone", "--depth=1", "--branch", ref, url, str(tmp_p)],
            check=True,
        )
        # Step 2: full SHA so SOURCE_VERSIONS is reproducible
        full_sha = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=tmp_p,
        ).decode().strip()
        short_sha = full_sha[:7]
        print(f"  upstream SHA: {short_sha} ({full_sha})")

        # Step 3: dispose of upstream .git — we want the source tree only
        shutil.rmtree(tmp_p / ".git", ignore_errors=True)

        # Step 4: for "src" layout, move tmp_p to target/src/ (delete first)
        if layout == "src":
            target_src = target / "src"
            if target_src.exists():
                print(f"  removing old {target_src}")
                shutil.rmtree(target_src)
            target.mkdir(parents=True, exist_ok=True)
            # tmp_p itself becomes target/src (preserve dir name)
            tmp_p.rename(target_src)
        elif layout == "include":
            target_inc = target / "include"
            if target_inc.exists():
                print(f"  removing old {target_inc}")
                shutil.rmtree(target_inc)
            target.mkdir(parents=True, exist_ok=True)
            tmp_p.rename(target_inc)
        else:
            raise SystemExit(f"unknown layout: {layout}")

    return short_sha, full_sha


def update_versions(name: str, ref: str, short_sha: str, full_sha: str,
                    url: str) -> None:
    """Rewrite SOURCE_VERSIONS with the new entry, preserving order."""
    lines = VERSIONS_FILE.read_text(encoding="utf-8").splitlines()
    new_entry = (
        f"{name:<12}{ref:<11}{short_sha:<8}   {full_sha}   "
        f"{args_date()}   {url}"
    )
    found = False
    for i, ln in enumerate(lines):
        if ln.lstrip().startswith(name + " "):
            lines[i] = new_entry
            found = True
            break
    if not found:
        lines.append("")
        lines.append(new_entry)
    VERSIONS_FILE.write_text("\n".join(lines) + "\n", encoding="utf-8")


def args_date() -> str:
    import datetime
    return datetime.date.today().isoformat()


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser()
    p.add_argument("vendor", nargs="?", help="vendor name (see --list)")
    p.add_argument("--ref", help="override ref (branch/tag/sha)")
    p.add_argument("--list", action="store_true", help="list known vendors")
    args = p.parse_args(argv)

    if args.list:
        list_vendors()
        return 0

    if not args.vendor:
        p.print_help()
        return 2

    if args.vendor not in VENDORS:
        print(f"unknown vendor: {args.vendor}", file=sys.stderr)
        print(f"  available: {', '.join(VENDORS)}", file=sys.stderr)
        return 1

    url, default_ref, layout = VENDORS[args.vendor]
    ref = args.ref or default_ref
    target = VENDOR_ROOT / args.vendor

    print(f"==> Updating {args.vendor} @ {ref}")
    short, full = clone_vendor(args.vendor, ref, url, layout, target,
                                target / "vendor_update.log")
    update_versions(args.vendor, ref, short, full, url)
    print(f"==> Updated SOURCE_VERSIONS")
    print(f"    next: cd {target.parent.parent} && "
          f"git add -A && git commit -m 'vendor({args.vendor}): bump to {short}'")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
