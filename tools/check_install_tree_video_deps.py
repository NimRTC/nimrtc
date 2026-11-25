#!/usr/bin/env python3
"""check_install_tree_video_deps.py — slice v1-install-tree-video-deps (ADR-038).

Verifies that a staged NimRTC install tree carries the video dependencies a
downstream video host needs, and that the package-config source chains them:

  1. `nimrtc_nvcodec` — the shared NVIDIA / VideoToolbox codec-adapter
     archive.  `nimrtc_h264` links it PUBLIC, so a downstream `nimrtc::engine`
     link that pulls in H.264 needs it on the flat chain.
  2. The three OpenH264 archives (common / encoder / decoder) together with
     their link-order manifest.  The encoder and the decoder reference
     `WelsLog()` / `g_kuiMbCountScan4Idx` from common, so a GNU static linker
     needs common *after* the two consumers.
  3. `d3d11` / `dxgi` on Windows (the D3D11 video sink and the NVENC path).

Exit status: 0 = every check that ran passed, 1 = at least one failed.

Modes
-----
  default                static checks on the source cmake files, then tree
                         checks on every staged install auto-discovered under
                         build/_install/.
  --root PREFIX          run tree checks on exactly these prefixes instead of
                         auto-discovery (repeatable).
  --allow-missing-openh264
                         treat "no OpenH264 manifest / archives" as a
                         documented, passing outcome.  The CI install legs
                         build with NIMRTC_PLUGINS_OPENH264=ON, so the default
                         (strict) mode requires them.
  --self-test            build synthetic positive + negative trees and assert
                         the negatives are reported — the tool cannot be
                         "always green".
  --repo ROOT            repository root (default: derived from this file).
"""

import argparse
import os
import re
import sys
import tempfile
from pathlib import Path

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
REPO = Path(__file__).resolve().parent.parent

CFG_SOURCE = "cmake/NimRTCConfig.cmake.in"
INSTALL_SOURCE = "cmake/NimRTCInstall.cmake"
OPENH264_WRAPPER = "src/third_party/openh264/CMakeLists.txt"

# OpenH264 link-order contract (the wrapper's "WHY THE ARCHIVES ARE IN THIS
# ORDER" note): encoder and decoder come first, common last.  The names are
# the *target* names; the installed archives keep these as their basename
# (plus the platform static-lib prefix/suffix).
OPENH264_ORDER = [
    "nimrtc_vendor_openh264_encoder",
    "nimrtc_vendor_openh264_decoder",
    "nimrtc_vendor_openh264_common",
]

# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def find_libdir(prefix: Path) -> Path:
    """The install's libdir under a prefix (GNUInstallDirs defaults)."""
    for candidate in ("lib", "lib64"):
        p = prefix / candidate
        if p.is_dir():
            return p
    return prefix / "lib"


def find_installed_config(prefix: Path):
    """The installed NimRTCConfig.cmake under the prefix, if any."""
    for p in (prefix / "lib", prefix / "lib64"):
        cfg = p / "cmake" / "NimRTC" / "NimRTCConfig.cmake"
        if cfg.exists():
            return cfg
    return None


def staged_prefixes(repo: Path) -> list:
    """Auto-discover staged installs under build/_install/<platform>/."""
    base = repo / "build" / "_install"
    if not base.is_dir():
        return []
    out = []
    for child in sorted(base.iterdir()):
        if child.is_dir():
            out.append(child)
    return out


# ---------------------------------------------------------------------------
# Static checks (source files)
# ---------------------------------------------------------------------------


def check_source_config(path: Path):
    """The package-config source must chain nvcodec + OpenH264 + d3d11/dxgi."""
    text = read(path)
    results = []

    # nvcodec in the per-module archive list.
    # The closing paren sits on its own line, so anchor on "\n)" — a plain
    # `(.*?)` would stop at the first `)` anywhere, e.g. inside a comment.
    m = re.search(r"set\(_nimrtc_module_shorts(.*?)\n\s*\)", text, re.S)
    if not m:
        results.append((False, "source config has no _nimrtc_module_shorts"))
    else:
        body = m.group(1)
        shorts = set(re.findall(r"^\s*([A-Za-z0-9_]+)\s*$", body, re.M))
        if "nvcodec" not in shorts:
            results.append((False, "source config chains nimrtc_nvcodec "
                                   "(nvcodec missing from _nimrtc_module_shorts)"))
        else:
            # h264 references nvcodec, so nvcodec must come after it in the
            # list (left-to-right GNU resolution).
            idx_h264 = _list_index(body, "h264")
            idx_nvcodec = _list_index(body, "nvcodec")
            if idx_h264 is None or idx_nvcodec is None or idx_nvcodec <= idx_h264:
                results.append((False, "source config orders nvcodec after h264 "
                                       "in _nimrtc_module_shorts"))
            else:
                results.append((True, "source config chains nimrtc_nvcodec "
                                      "(nvcodec after h264 in _nimrtc_module_shorts)"))

    # OpenH264 link-order manifest read.
    if "openh264_link_order.txt" not in text:
        results.append((False, "source config reads the OpenH264 link-order "
                               "manifest (openh264_link_order.txt)"))
    else:
        results.append((True, "source config reads the OpenH264 link-order "
                              "manifest (openh264_link_order.txt)"))

    # d3d11 / dxgi on Windows.
    m_win = re.search(r"if\(WIN32\)(.*?)(?:elseif|endif)", text, re.S)
    if not m_win:
        results.append((False, "source config has a WIN32 system-libs branch"))
    else:
        win_body = m_win.group(1)
        if "d3d11" not in win_body:
            results.append((False, "source config chains d3d11 on WIN32"))
        elif "dxgi" not in win_body:
            results.append((False, "source config chains dxgi on WIN32"))
        else:
            results.append((True, "source config chains d3d11 and dxgi on WIN32"))

    return results


def _list_index(body: str, item: str) -> int:
    """1-based index of `item` as a standalone list entry line, or None."""
    for i, line in enumerate(body.splitlines(), start=1):
        if line.strip() == item:
            return i
    return None


def check_source_install(path: Path):
    """The install rules must install nvcodec + the OpenH264 archives/manifest."""
    text = read(path)
    results = []

    # nvcodec in the install-shorts list.
    m = re.search(r"set\(_nimrtc_install_shorts(.*?)\n\s*\)", text, re.S)
    if not m:
        results.append((False, "install rules have no _nimrtc_install_shorts"))
    else:
        if "nvcodec" not in m.group(1):
            results.append((False, "install rules install nimrtc_nvcodec "
                                   "(nvcodec missing from _nimrtc_install_shorts)"))
        else:
            results.append((True, "install rules install nimrtc_nvcodec "
                                  "(nvcodec in _nimrtc_install_shorts)"))

    # nvcodec public headers installed.
    if "src/modules/nvcodec/include" not in text:
        results.append((False, "install rules install the nvcodec public headers"))
    else:
        results.append((True, "install rules install the nvcodec public headers"))

    # OpenH264 archives + manifest install section.
    if "NIMRTC_OPENH264_LINK_ARCHIVES" not in text:
        results.append((False, "install rules install the OpenH264 archives "
                               "(reads NIMRTC_OPENH264_LINK_ARCHIVES)"))
    else:
        results.append((True, "install rules install the OpenH264 archives "
                              "(reads NIMRTC_OPENH264_LINK_ARCHIVES)"))

    if "openh264_link_order.txt" not in text:
        results.append((False, "install rules write + install the OpenH264 "
                               "link-order manifest (openh264_link_order.txt)"))
    else:
        results.append((True, "install rules write + install the OpenH264 "
                              "link-order manifest (openh264_link_order.txt)"))

    return results


def check_source_openh264_wrapper(path: Path):
    """The wrapper must publish the archive set + order as a GLOBAL property."""
    text = read(path)
    results = []

    m = re.search(
        r"set_property\(GLOBAL PROPERTY NIMRTC_OPENH264_LINK_ARCHIVES(.*?)\n\s*\)",
        text, re.S)
    if not m:
        results.append((False, "OpenH264 wrapper publishes "
                               "NIMRTC_OPENH264_LINK_ARCHIVES"))
    else:
        body = m.group(1)
        # Preserve the order while ignoring the leading whitespace.
        tokens = [t for t in body.replace("\n", " ").split() if t]
        if tokens != OPENH264_ORDER:
            results.append((False, "OpenH264 wrapper orders the archives as "
                                   "encoder / decoder / common"))
        else:
            results.append((True, "OpenH264 wrapper publishes "
                                  "NIMRTC_OPENH264_LINK_ARCHIVES "
                                  "(encoder / decoder / common)"))
    return results


# ---------------------------------------------------------------------------
# Tree checks (a staged install prefix)
# ---------------------------------------------------------------------------


def check_tree(prefix: Path, allow_missing_openh264: bool):
    """A staged install tree must be self-contained for a video host."""
    libdir = find_libdir(prefix)
    results = []

    # --- nimrtc_nvcodec archive -------------------------------------------
    nvcodec_hits = list(libdir.glob("nimrtc_nvcodec.*")) if libdir.is_dir() else []
    if not nvcodec_hits:
        results.append((False, f"staged tree {prefix}: nimrtc_nvcodec archive "
                               f"present in {libdir}"))
    else:
        results.append((True, f"staged tree {prefix}: nimrtc_nvcodec archive "
                              f"present ({nvcodec_hits[0].name})"))

    # --- OpenH264 manifest + archives -------------------------------------
    manifest = libdir / "openh264_link_order.txt"
    if not manifest.exists():
        if allow_missing_openh264:
            results.append((True, f"staged tree {prefix}: OpenH264 not built — "
                                  f"no manifest (documented, allowed)"))
        else:
            results.append((False, f"staged tree {prefix}: OpenH264 link-order "
                                   f"manifest present ({manifest})"))
        return results

    lines = [ln.strip() for ln in read(manifest).splitlines() if ln.strip()]
    if len(lines) != len(OPENH264_ORDER):
        results.append((False, f"staged tree {prefix}: manifest lists "
                               f"{len(lines)} archives, expected {len(OPENH264_ORDER)}"))
    else:
        for name, expected in zip(lines, OPENH264_ORDER):
            archive = libdir / name
            if not archive.exists():
                results.append((False, f"staged tree {prefix}: manifest archive "
                                       f"{name} exists in {libdir}"))
            else:
                results.append((True, f"staged tree {prefix}: manifest archive "
                                      f"{name} present"))
        # Order: encoder, decoder, common.
        if len(lines) == len(OPENH264_ORDER):
            order_ok = all(expected in name for name, expected in zip(lines, OPENH264_ORDER))
            if order_ok:
                results.append((True, f"staged tree {prefix}: OpenH264 archive "
                                      f"order is encoder / decoder / common"))
            else:
                results.append((False, f"staged tree {prefix}: OpenH264 archive "
                                       f"order is encoder / decoder / common "
                                       f"(got {lines})"))

    # --- installed config chains the video deps ---------------------------
    cfg = find_installed_config(prefix)
    if cfg is None:
        results.append((False, f"staged tree {prefix}: installed "
                               f"NimRTCConfig.cmake present"))
    else:
        cfg_text = read(cfg)
        if "nvcodec" not in cfg_text:
            results.append((False, f"staged tree {prefix}: installed config "
                                   f"chains nimrtc_nvcodec"))
        else:
            results.append((True, f"staged tree {prefix}: installed config "
                                  f"chains nimrtc_nvcodec"))
        if "openh264_link_order.txt" not in cfg_text:
            results.append((False, f"staged tree {prefix}: installed config "
                                   f"reads the OpenH264 link-order manifest"))
        else:
            results.append((True, f"staged tree {prefix}: installed config "
                                  f"reads the OpenH264 link-order manifest"))
        if os.name == "nt":
            if "d3d11" not in cfg_text or "dxgi" not in cfg_text:
                results.append((False, f"staged tree {prefix}: installed config "
                                       f"chains d3d11 and dxgi on Windows"))
            else:
                results.append((True, f"staged tree {prefix}: installed config "
                                      f"chains d3d11 and dxgi on Windows"))

    return results


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------


def run_checks(source_results, tree_results):
    """Print every check; return exit code."""
    all_ok = True
    for ok, msg in source_results + tree_results:
        tag = "PASS" if ok else "FAIL"
        if not ok:
            all_ok = False
        print(f"{tag} {msg}")
    if all_ok:
        print("OK — install tree video dependencies are in place")
        return 0
    print("FAIL — install tree video dependencies are missing or mis-ordered",
          file=sys.stderr)
    return 1


# ---------------------------------------------------------------------------
# Self-test: the tool must not be "always green"
# ---------------------------------------------------------------------------


def _make_tree(base: Path, files):
    """Create a synthetic prefix tree.  `files` maps relative paths to bytes."""
    for rel, content in files.items():
        p = base / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(content)
    return base


def self_test():
    """Build positive + negative synthetic trees; negatives must be reported."""
    passed = True

    def emit(ok, msg):
        nonlocal passed
        tag = "PASS" if ok else "FAIL"
        if not ok:
            passed = False
        print(f"{tag} {msg}")

    tmp = Path(tempfile.mkdtemp(prefix="nimrtc_check_install_video_deps_"))

    enc = b"nimrtc_vendor_openh264_encoder.lib"
    dec = b"nimrtc_vendor_openh264_decoder.lib"
    com = b"nimrtc_vendor_openh264_common.lib"
    good_manifest = b"\n".join([enc, dec, com]) + b"\n"
    bad_manifest = b"\n".join([com, enc, dec]) + b"\n"

    cfg_markers = (
        b"set(_nimrtc_module_shorts\n  h264\n  nvcodec\n)\n"
        b"openh264_link_order.txt\n"
        b"if(WIN32)\n  d3d11 dxgi\nendif()\n"
    )
    cfg_missing_nvcodec = (
        b"set(_nimrtc_module_shorts\n  h264\n)\n"
        b"openh264_link_order.txt\n"
        b"if(WIN32)\n  d3d11 dxgi\nendif()\n"
    )
    cfg_missing_d3d11 = (
        b"set(_nimrtc_module_shorts\n  h264\n  nvcodec\n)\n"
        b"openh264_link_order.txt\n"
        b"if(WIN32)\n  ws2_32\nendif()\n"
    )

    def fake_tree(root, manifest, has_nvcodec=True, cfg=cfg_markers):
        files = {
            "lib/openh264_link_order.txt": manifest,
            "lib/nimrtc_vendor_openh264_encoder.lib": b"e",
            "lib/nimrtc_vendor_openh264_decoder.lib": b"d",
            "lib/nimrtc_vendor_openh264_common.lib": b"c",
            "lib/cmake/NimRTC/NimRTCConfig.cmake": cfg,
        }
        if has_nvcodec:
            files["lib/nimrtc_nvcodec.lib"] = b"n"
        return _make_tree(root, files)

    # Negative: missing nvcodec archive.
    t = fake_tree(tmp / "neg_missing_nvcodec", good_manifest, has_nvcodec=False)
    res = check_tree(t, allow_missing_openh264=False)
    if any(not ok for ok, _ in res):
        emit(True, "a tree missing nimrtc_nvcodec is reported")
    else:
        emit(False, "a tree missing nimrtc_nvcodec is reported")

    # Negative: OpenH264 manifest wrong order (common first).
    t = fake_tree(tmp / "neg_wrong_order", bad_manifest)
    res = check_tree(t, allow_missing_openh264=False)
    if any(not ok for ok, _ in res) and any(
            not ok and "order" in msg for ok, msg in res):
        emit(True, "a tree with common before encoder/decoder is reported")
    else:
        emit(False, "a tree with common before encoder/decoder is reported")

    # Negative: missing manifest entirely (strict mode).
    t = fake_tree(tmp / "neg_missing_manifest", good_manifest)
    (t / "lib" / "openh264_link_order.txt").unlink()
    res = check_tree(t, allow_missing_openh264=False)
    if any(not ok for ok, _ in res) and any(
            not ok and "manifest" in msg for ok, msg in res):
        emit(True, "a tree without the OpenH264 manifest is reported")
    else:
        emit(False, "a tree without the OpenH264 manifest is reported")

    # Negative: source config does not chain nvcodec.
    t = fake_tree(tmp / "neg_cfg_no_nvcodec", good_manifest, cfg=cfg_missing_nvcodec)
    cfg_file = t / "lib" / "cmake" / "NimRTC" / "NimRTCConfig.cmake"
    res = check_source_config(cfg_file)
    if any(not ok for ok, _ in res):
        emit(True, "a config not chaining nimrtc_nvcodec is reported")
    else:
        emit(False, "a config not chaining nimrtc_nvcodec is reported")

    # Negative: source config does not chain d3d11.
    t = fake_tree(tmp / "neg_cfg_no_d3d11", good_manifest, cfg=cfg_missing_d3d11)
    cfg_file = t / "lib" / "cmake" / "NimRTC" / "NimRTCConfig.cmake"
    res = check_source_config(cfg_file)
    if any(not ok for ok, _ in res) and any(
            not ok and "d3d11" in msg for ok, msg in res):
        emit(True, "a config not chaining d3d11 is reported")
    else:
        emit(False, "a config not chaining d3d11 is reported")

    # Positive: a complete tree is not reported.
    t = fake_tree(tmp / "pos_complete", good_manifest)
    res = check_tree(t, allow_missing_openh264=False)
    if all(ok for ok, _ in res):
        emit(True, "a complete tree is not reported")
    else:
        emit(False, f"a complete tree is not reported: {res}")

    return 0 if passed else 1


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", action="append", default=[],
                    help="staged install prefix to check (repeatable; "
                         "default: auto-discover build/_install/*)")
    ap.add_argument("--allow-missing-openh264", action="store_true",
                    help="accept a tree where OpenH264 was not built")
    ap.add_argument("--self-test", action="store_true",
                    help="run the negative/positive self-test and exit")
    ap.add_argument("--repo", default=str(REPO), help="repository root")
    args = ap.parse_args(argv)

    repo = Path(args.repo)

    if args.self_test:
        return self_test()

    source_results = []
    source_results += check_source_config(repo / CFG_SOURCE)
    source_results += check_source_install(repo / INSTALL_SOURCE)
    source_results += check_source_openh264_wrapper(repo / OPENH264_WRAPPER)

    tree_results = []
    roots = [Path(r) for r in args.root]
    if not roots:
        roots = staged_prefixes(repo)
        if not roots:
            print("NOTE: no staged install under build/_install/ — "
                  "running source checks only")
    for prefix in roots:
        tree_results += check_tree(prefix, allow_missing_openh264=args.allow_missing_openh264)

    return run_checks(source_results, tree_results)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
