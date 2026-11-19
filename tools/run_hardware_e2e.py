#!/usr/bin/env python3
# Copyright (c) 2026 NimRTC contributors.
# SPDX-License-Identifier: MIT
"""Record the v1.3 plan's Q7 hardware-encode evidence.

What this tool is for
---------------------
The v1.3 plan's Q7 asks for the *hardware* codec path to be "exercised
end-to-end by something re-runnable, and its result ... asserted from the
stats" (docs/plan/v1.3-plan.md §3 Q7).  The runner of the path is
`examples/hw-e2e`: it pins the H.264 backend to hardware, encodes 1080p
frames, carries the bitstream through the shipped sender / receiver,
decodes it, and prints one JSON record.  This tool is the recorder:

  * it locates (or, with ``--build``, builds) the ``hw-e2e`` binary;
  * runs it;
  * folds the harness record together with the machine's **driver stamp**
    (the NVIDIA driver version, or the platform equivalent) and the
    tree's **git provenance**;
  * re-evaluates the green criteria from the archived numbers, so a
    harness bug that faked a pass is still caught;
  * appends one line to ``build/hardware/<platform>-<YYYY-MM-DD>.jsonl``
    -- the archive the workflow uploads as an artifact.

The record is self-describing: a reader can see which tree produced it,
which driver ran it, and what the pipeline actually did.

Honesty rules (the same ones `tools/run_soak.py` and
`tools/run_soak_impaired.py` apply)
------------------------------------
  * No hardware encoder on the host: the record is **SKIPPED**, never
    PASS.  ``--require-hardware`` turns a skip into a failing exit code,
    so a CI runner that silently lost its GPU is visible.
  * The verdict is re-derived from the archived numbers, not read from
    the harness's ``"status"`` field.
  * A record whose tree was dirty says so, because a dirty tree is not
    the commit it claims.

Usage
-----
    python tools/run_hardware_e2e.py                 # run + archive
    python tools/run_hardware_e2e.py --build         # build first
    python tools/run_hardware_e2e.py --require-hardware
    python tools/run_hardware_e2e.py --summary       # print the window
    python tools/run_hardware_e2e.py --expect-green  # gate mode

The exit code is the verdict of *this* run: 0 pass, 1 fail, 3 skipped
(or 1 under ``--require-hardware``).
"""

from __future__ import annotations

import argparse
import datetime as _dt
import glob
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
EVIDENCE_DIR = REPO_ROOT / "build" / "hardware"
SCHEMA = "nimrtc-hw-e2e-evidence/1"

# The green criteria, applied to the *archived* numbers.  They mirror the
# harness's own checks on purpose: the archive is re-evaluated so a
# harness that stopped checking something (or a hand-edited record) does
# not turn into a green day.
MIN_FRAMES = 1
MAX_LUMA_MAE = 14.0
MIN_LUMA_PEARSON = 0.93


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

def _run(cmd: list[str], cwd: Path | None = None,
         timeout: int = 600) -> tuple[int, str, str]:
    """Run a command, returning (returncode, stdout, stderr)."""
    try:
        proc = subprocess.run(
            cmd, cwd=str(cwd) if cwd else None, capture_output=True,
            text=True, timeout=timeout, errors="replace")
        return proc.returncode, proc.stdout, proc.stderr
    except FileNotFoundError:
        return 127, "", f"command not found: {cmd[0]}"
    except subprocess.TimeoutExpired:
        return 124, "", f"timed out after {timeout}s: {' '.join(cmd)}"


def git_provenance() -> dict:
    """The tree the run measured: ref, commit, and whether it was dirty."""
    rc, ref, _ = _run(["git", "rev-parse", "--abbrev-ref", "HEAD"])
    _, commit, _ = _run(["git", "rev-parse", "HEAD"])
    _, status, _ = _run(["git", "status", "--porcelain"])
    return {
        "git_ref": ref.strip() if rc == 0 else "",
        "git_commit": commit.strip() if rc == 0 else "",
        "git_dirty": bool(status.strip()),
    }


def _first_line(text: str) -> str:
    for line in text.splitlines():
        line = line.strip()
        if line:
            return line
    return ""


def driver_stamp() -> dict:
    """The GPU / driver the run actually used.

    NVIDIA is identified through `nvidia-smi`, which every driver
    installs; the raw first lines are kept so the record stays readable
    on a machine this tool has never seen.  On other platforms the stamp
    says what it can (the OS build), and never claims more.
    """
    stamp: dict = {
        "platform": sys.platform,
        "machine": platform.machine(),
    }
    exe = shutil.which("nvidia-smi")
    if exe:
        rc, out, err = _run([exe, "--query-gpu=name,driver_version",
                             "--format=csv,noheader"], timeout=30)
        stamp["nvidia_smi_rc"] = rc
        if rc == 0:
            gpus = [g.strip() for g in out.splitlines() if g.strip()]
            stamp["gpus"] = gpus
            versions = []
            for g in gpus:
                parts = [p.strip() for p in g.split(",")]
                if len(parts) >= 2:
                    versions.append(parts[-1])
            if versions:
                stamp["driver_version"] = versions[0]
        elif err:
            stamp["nvidia_smi_error"] = _first_line(err)
    return stamp


def find_binary(explicit: str | None) -> Path | None:
    """Locate the `hw-e2e` executable (multi-config layout supported)."""
    if explicit:
        p = Path(explicit)
        return p if p.is_file() else None

    patterns = [
        "build/examples/Debug/hw-e2e.exe",
        "build/examples/Release/hw-e2e.exe",
        "build/examples/hw-e2e",
        "build/hw/examples/Debug/hw-e2e.exe",
        "build/hw/examples/Release/hw-e2e.exe",
        "build/hw/examples/hw-e2e",
        "build/examples/RelWithDebInfo/hw-e2e.exe",
    ]
    for rel in patterns:
        p = REPO_ROOT / rel
        if p.is_file():
            return p
    hits = sorted(glob.glob(str(REPO_ROOT / "build" / "**" / "hw-e2e*"),
                            recursive=True))
    for h in hits:
        if h.endswith(".exe") or os.access(h, os.X_OK):
            if Path(h).is_file() and "CMakeFiles" not in h:
                return Path(h)
    return None


def build_binary(build_dir: str, config: str) -> int:
    """Configure (if needed) and build the `hw-e2e` target."""
    bdir = REPO_ROOT / build_dir
    if not (bdir / "CMakeCache.txt").is_file():
        rc, _, err = _run(["cmake", "-S", ".", "-B", build_dir,
                           "-DNIMRTC_BUILD_EXAMPLES=ON",
                           "-DNIMRTC_PLUGINS_NVENC=ON",
                           "-DNIMRTC_PLUGINS_NVDEC=ON"], cwd=REPO_ROOT)
        if rc != 0:
            sys.stderr.write(err)
            return rc
    rc, _, err = _run(["cmake", "--build", build_dir, "--config", config,
                       "--target", "hw-e2e"], cwd=REPO_ROOT, timeout=3600)
    if rc != 0:
        sys.stderr.write(err[-4000:])
    return rc


def parse_harness_record(stdout: str) -> tuple[dict | None, str]:
    """Extract the JSON record from the harness's stdout.

    The harness logs through the tree's logger (stderr in a normal
    build), but a redirected run may interleave; so the last line that
    parses as a JSON object carrying the schema tag is the record.
    """
    for line in reversed(stdout.splitlines()):
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(obj, dict) and obj.get("schema") == "nimrtc-hw-e2e/1":
            return obj, ""
    return None, "the harness printed no nimrtc-hw-e2e/1 record"


def evaluate(record: dict) -> list[str]:
    """Re-derive the green criteria from the archived numbers."""
    failures: list[str] = []
    if not record.get("encoder_is_hardware"):
        failures.append("the encoder did not report hardware acceleration")
    if int(record.get("frames_encoded", 0)) < MIN_FRAMES:
        failures.append("no frame was encoded")
    if int(record.get("keyframes", 0)) < 1:
        failures.append("no keyframe was produced")
    if int(record.get("encode_errors", 0)) != 0:
        failures.append("the encoder reported errors")
    if int(record.get("rtp_packets", 0)) < 1:
        failures.append("no RTP packet was emitted")
    if int(record.get("frames_reassembled", 0)) < 1:
        failures.append("the receiver reassembled no frame")
    if int(record.get("frames_decoded", 0)) < 1:
        failures.append("no frame was decoded")
    if int(record.get("decode_errors", 0)) != 0:
        failures.append("the decoder reported errors")
    mae = float(record.get("luma_mae", -1.0))
    if not (0.0 <= mae < MAX_LUMA_MAE):
        failures.append(f"luma MAE {mae:.2f} outside [0, {MAX_LUMA_MAE})")
    corr = float(record.get("luma_pearson", -1.0))
    if not (corr > MIN_LUMA_PEARSON):
        failures.append(f"luma Pearson r {corr:.4f} <= {MIN_LUMA_PEARSON}")
    return failures


# ---------------------------------------------------------------------------
# The run
# ---------------------------------------------------------------------------

def run_once(args: argparse.Namespace) -> int:
    binary = find_binary(args.binary)
    if binary is None and args.build:
        rc = build_binary(args.build_dir, args.config)
        if rc != 0:
            print(f"run_hardware_e2e: build failed (rc={rc})")
            return 1
        binary = find_binary(args.binary)
    if binary is None:
        print("run_hardware_e2e: hw-e2e was not found; pass --build or "
              "--binary <path>")
        return 1

    cmd = [str(binary), "--width", str(args.width),
           "--height", str(args.height), "--frames", str(args.frames)]
    if args.require_hardware:
        cmd.append("--require-hardware")

    rc, out, err = _run(cmd, cwd=binary.parent)
    record, parse_error = parse_harness_record(out)

    status = "INCONCLUSIVE"
    failures: list[str] = []
    if record is None:
        failures.append(parse_error)
    elif record.get("status") == "skipped":
        status = "SKIPPED"
    else:
        failures = evaluate(record)
        status = "PASS" if not failures else "FAIL"
    if rc not in (0, 77):
        # The harness could not even measure: say so, even when it printed
        # a record that happens to look green.
        if status == "PASS":
            failures.append(f"the harness exited {rc}")
            status = "FAIL"

    today = _dt.date.today().isoformat()
    entry = {
        "schema": SCHEMA,
        "date": today,
        "timestamp_utc": _dt.datetime.now(_dt.timezone.utc)
                               .strftime("%Y-%m-%dT%H:%M:%SZ"),
        "status": status,
        "failures": failures,
        "harness": {
            "binary": str(binary),
            "command": cmd,
            "exit_code": rc,
        },
        "record": record or {},
        "driver": driver_stamp(),
    }
    entry.update(git_provenance())

    EVIDENCE_DIR.mkdir(parents=True, exist_ok=True)
    platform_tag = "windows" if sys.platform == "win32" else (
        "linux" if sys.platform.startswith("linux") else sys.platform)
    path = EVIDENCE_DIR / f"{platform_tag}-{today}.jsonl"
    with path.open("a", encoding="utf-8") as f:
        f.write(json.dumps(entry, sort_keys=True) + "\n")

    print(f"run_hardware_e2e: {status} (archived to "
          f"{path.relative_to(REPO_ROOT)})")
    if record is not None:
        print(f"  encoder={record.get('encoder_backend')} "
              f"hw={record.get('encoder_is_hardware')} "
              f"frames={record.get('frames_encoded')} "
              f"rtp={record.get('rtp_packets')} "
              f"decoded={record.get('frames_decoded')} "
              f"mae={record.get('luma_mae')} "
              f"r={record.get('luma_pearson')}")
    for f in failures:
        print(f"  - {f}")
    drv = entry["driver"]
    if drv.get("driver_version"):
        print(f"  driver={drv.get('driver_version')} "
              f"gpu={'; '.join(drv.get('gpus', []))}")

    if status == "PASS":
        return 0
    if status == "SKIPPED" and not args.require_hardware:
        return 3
    return 1


# ---------------------------------------------------------------------------
# The summary
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# The self-test
# ---------------------------------------------------------------------------
#
# Every assertion is about the tool **refusing to overstate** the
# evidence: a green record has to mean the hardware path really ran, and
# a skipped one has to stay skipped.  The scratch evidence lives in the
# system temp directory, never in the NimRTC working tree.

def _green_record() -> dict:
    """A harness record that satisfies every green criterion."""
    return {
        "schema": "nimrtc-hw-e2e/1",
        "status": "ok",
        "encoder_backend": "nvenc",
        "encoder_is_hardware": True,
        "decoder_backend": "nvcdec",
        "decoder_is_hardware": True,
        "width": 1920, "height": 1080,
        "frames_encoded": 3, "keyframes": 1, "encode_errors": 0,
        "rtp_packets": 4, "frames_reassembled": 1,
        "frames_decoded": 1, "decode_errors": 0,
        "luma_mae": 3.5, "luma_pearson": 0.98,
    }


def _self_test() -> int:
    """Pin the verdict semantics."""
    failures = 0

    def check(condition: bool, label: str) -> None:
        nonlocal failures
        if condition:
            print(f"[OK  ] {label}")
        else:
            print(f"[FAIL] {label}")
            failures += 1

    # 1. A healthy record passes every criterion.
    check(not evaluate(_green_record()),
          "a healthy hardware record passes every criterion")

    # 2. A software encoder is not a hardware run, however good the
    #    picture is.
    record = _green_record()
    record["encoder_is_hardware"] = False
    check(bool(evaluate(record)),
          "a record whose encoder was not hardware fails")

    # 3. A picture that did not survive the round trip fails.
    record = _green_record()
    record["luma_mae"] = 90.0
    record["luma_pearson"] = 0.10
    check(bool(evaluate(record)),
          "a record whose picture did not survive fails")

    # 4. A record that encoded nothing fails.
    record = _green_record()
    record["frames_encoded"] = 0
    check(bool(evaluate(record)),
          "a record that encoded no frame fails")

    # 5. The harness's own "status" is not the verdict: a record that
    #    claims success but decoded nothing still fails.
    record = _green_record()
    record["status"] = "ok"
    record["frames_decoded"] = 0
    check(bool(evaluate(record)),
          "a record that decoded nothing fails even though the harness "
          "claimed success")

    # 6. The record is parsed out of a noisy stdout.
    noisy = ("[info] h264: registered video backends\n"
             + json.dumps(_green_record()) + "\n")
    parsed, err = parse_harness_record(noisy)
    check(parsed is not None and err == "",
          "the record is found in a noisy stdout")
    parsed, err = parse_harness_record("[info] nothing here\n")
    check(parsed is None and err,
          "a stdout without a record is an error, not a silent pass")

    # 7. The summary's verdict, over a synthetic evidence file.
    tmp = Path(tempfile.mkdtemp(prefix="nimrtc-hw-e2e-selftest-"))
    try:
        entry = {"schema": SCHEMA, "date": _dt.date.today().isoformat(),
                 "status": "PASS", "failures": [],
                 "git_ref": "main", "git_commit": "0" * 40, "git_dirty": False,
                 "driver": {"driver_version": "999.99"}}
        path = tmp / "windows-2026-11-01.jsonl"
        with path.open("w", encoding="utf-8") as handle:
            handle.write(json.dumps(entry) + "\n")
            handle.write("{ this is not json\n")
        original = EVIDENCE_DIR
        try:
            globals()["EVIDENCE_DIR"] = tmp
            check(summary(3650, expect_green=True) == 0,
                  "the summary accepts a PASS record")
            check(summary(3650, expect_green=False) == 0,
                  "the summary exits 0 without a strict expectation")
        finally:
            globals()["EVIDENCE_DIR"] = original
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # 8. An empty window is not a green window.
    tmp = Path(tempfile.mkdtemp(prefix="nimrtc-hw-e2e-selftest-"))
    try:
        original = EVIDENCE_DIR
        try:
            globals()["EVIDENCE_DIR"] = tmp
            check(summary(3650, expect_green=True) == 1,
                  "an empty window does not satisfy --expect-green")
        finally:
            globals()["EVIDENCE_DIR"] = original
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print(f"self-test: {failures} failure(s)")
    return 1 if failures else 0


def summary(days: int, expect_green: bool) -> int:
    cutoff = _dt.date.today() - _dt.timedelta(days=days)
    entries: list[dict] = []
    for path in sorted(EVIDENCE_DIR.glob("*.jsonl")):
        for line in path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                e = json.loads(line)
            except json.JSONDecodeError:
                continue
            if e.get("schema") != SCHEMA:
                continue
            try:
                d = _dt.date.fromisoformat(e.get("date", ""))
            except ValueError:
                continue
            if d >= cutoff:
                entries.append(e)

    print(f"# hardware-encode evidence (last {days} days)")
    if not entries:
        print("no records")
        return 1 if expect_green else 0

    green = 0
    for e in entries:
        drv = e.get("driver", {})
        print(f"- {e.get('date')} {e.get('status'):>12}  "
              f"ref={e.get('git_ref')}@{str(e.get('git_commit'))[:10]}"
              f"{' (dirty)' if e.get('git_dirty') else ''}  "
              f"driver={drv.get('driver_version', '-')}")
        for f in e.get("failures", []):
            print(f"    - {f}")
        if e.get("status") == "PASS":
            green += 1

    print()
    print(f"{green} PASS / {len(entries)} records")
    if expect_green and green == 0:
        return 1
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Run and archive the hardware-encode end-to-end "
                    "evidence (v1.3 plan Q7).")
    ap.add_argument("--binary", help="path to the hw-e2e executable")
    ap.add_argument("--build", action="store_true",
                    help="build the hw-e2e target before running it")
    ap.add_argument("--build-dir", default="build",
                    help="the CMake build directory (default: build)")
    ap.add_argument("--config", default="Debug",
                    help="the CMake configuration (default: Debug)")
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--frames", type=int, default=3)
    ap.add_argument("--require-hardware", action="store_true",
                    help="a host without a hardware encoder fails the run "
                         "instead of skipping it")
    ap.add_argument("--summary", action="store_true",
                    help="print the archived window instead of running")
    ap.add_argument("--days", type=int, default=30,
                    help="how many days the summary covers (default: 30)")
    ap.add_argument("--expect-green", action="store_true",
                    help="fail when the window has no PASS record")
    ap.add_argument("--self-test", action="store_true",
                    help="run the built-in self-test and exit")
    args = ap.parse_args(argv)

    if args.self_test:
        return _self_test()

    if args.summary:
        return summary(args.days, args.expect_green)
    return run_once(args)


if __name__ == "__main__":
    sys.exit(main())
